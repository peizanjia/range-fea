#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <iomanip>
#include "stage9_test_models.h"

namespace {
QString binary(const char *name) {
    QString filename = QString::fromLatin1(name);
#ifdef Q_OS_WIN
    filename += ".exe";
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(filename);
}
QString example(const char *name) { return QDir(QStringLiteral(RANGE_CONTROL_EXAMPLES)).filePath(name); }
QJsonObject json(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool run(const QString &program, const QStringList &args, QByteArray &log) {
    QProcess process; process.start(program, args);
    if (!process.waitForFinished(180000)) {
        process.kill(); process.waitForFinished(); log = process.errorString().toUtf8(); return false;
    }
    log = process.readAllStandardOutput()+process.readAllStandardError();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
double nodal(const RModel &model, RVariableType type, uint node) {
    return model.getVariable(model.findVariable(type)).getValue(0,node);
}
}

class TestControllerAdaptersCli: public QObject {
    Q_OBJECT
private slots:
    void cppAndLearnedPolicyRunFullFE();
};

void TestControllerAdaptersCli::cppAndLearnedPolicyRunFullFE()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    constexpr uint count=4, steps=400;
    constexpr double dt=.005, amplitude=.001, pi=3.14159265358979323846;
    auto model=Stage9TestModels::makeTruss(count);
    auto &dynamics=model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true); dynamics.setTimeStep(dt); dynamics.setNSteps(steps); dynamics.setOutputFrequency(steps);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setSolverCvgValue(1e-13);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setNOuterIterations(1000);
    for (uint node=1;node<=count;++node) {
        RElement point(R_ELEMENT_POINT); point.setNodeId(0,node);
        const uint group=model.getNPoints(); model.addElement(point,true,group);
        RInitialCondition initial(R_INITIAL_CONDITION_DISPLACEMENT);
        initial.getComponent(initial.findComponentPosition(R_VARIABLE_DISPLACEMENT_X)).add(0,amplitude*std::sin(pi*node/(2*count)));
        model.getPoint(group).addInitialCondition(initial);
    }
    auto &control=model.getProblemSetup().getStructuralControlSetup();
    control.enabled=true; control.controllerId="alternative-controller"; control.deterministicSeed=17;
    const auto input=model.write(directory.filePath("input.ram"),false);
    const auto matrix=directory.filePath("linear.json"), gainPath=directory.filePath("gain.json"), referencePath=directory.filePath("reference.json");
    QByteArray log;
    QVERIFY2(run(binary("fea-solver"),{"--file="+input,"--linear-model-output="+matrix},log),log.constData());
    const auto python=QStringLiteral(RANGE_CONTROL_PYTHON);
    QVERIFY2(run(python,{example("stage9/stage9_velocity_controller.py"),"--model",matrix,"--target-damping","0.1","--sensor-id","tip-velocity-x","--actuator-id","tip-force-x","--output",gainPath},log),log.constData());
    const auto linear=json(matrix); const auto dofs=linear["dofs"].toArray();
    QJsonArray q0;
    for (const auto &value:dofs) {
        const auto dof=value.toObject(); const auto node=dof["nodePosition"].toInt();
        q0.append(dof["globalDirection"].toArray()[0].toDouble()*amplitude*std::sin(pi*node/(2*count)));
    }
    QVERIFY2(run(python,{example("stage9/stage9_velocity_controller.py"),"--replay-model",matrix,"--gains",gainPath,"--dt","0.005","--steps","400","--initial-q-json",QString::fromUtf8(QJsonDocument(q0).toJson(QJsonDocument::Compact)),"--output",referencePath},log),log.constData());
    const auto reference=json(referencePath)["fullOrder"].toObject();
    const auto gain=QString::number(json(gainPath)["gain"].toDouble(),'g',17);
    struct Run { QString name,program; QStringList arguments; double qTolerance,vTolerance; };
    QVector<Run> runs{{"cpp",binary("range-control-velocity"),{gain},1e-8,1e-7}};
    const auto matlabResult=qEnvironmentVariable("RANGE_MATLAB_RESULT");
    if (!matlabResult.isEmpty()) runs.append({"matlab-tcp",{}, {},1e-8,1e-7});
#ifdef RANGE_CONTROL_TORCH_AVAILABLE
    const auto policy=directory.filePath("policy.json");
    QVERIFY2(run(python,{example("train_control_policy.py"),"--gain",gain,"--sensor-id","tip-velocity-x","--actuator-id","tip-force-x","--velocity-scale","0.1","--epochs","600","--seed","17","--output",policy},log),log.constData());
    runs.append({"learned",python,{example("control_learned_python.py"),"--policy="+policy},2e-5,3e-4});
    runs.append({"learned-alias",python,{example("control_learned_python.py"),"--weights="+policy},2e-5,3e-4});
#endif
    for (const auto &entry:runs) {
        const auto path=entry.program.isEmpty() ? matlabResult : model.write(directory.filePath(entry.name+".ram"),false);
        QJsonArray argv; for (const auto &arg:entry.arguments) argv.append(arg);
        if (!entry.program.isEmpty()) {
            QVERIFY2(run(binary("fea-solver"),{"--file="+path,"--control-program="+entry.program,
                "--control-args-json="+QString::fromUtf8(QJsonDocument(argv).toJson(QJsonDocument::Compact)),"--control-timeout-ms=10000"},log),log.constData());
        }
        RModel result; result.read(path);
        double qError=0,vError=0,energy=0;
        for (uint i=0;i<count;++i) {
            const auto di=dofs[int(i)].toObject(); const auto ni=uint(di["nodePosition"].toInt());
            const double si=di["globalDirection"].toArray()[0].toDouble();
            const double q=si*nodal(result,R_VARIABLE_DISPLACEMENT,ni),v=si*nodal(result,R_VARIABLE_VELOCITY,ni);
            qError=std::max(qError,std::abs(q-reference["q"].toArray()[int(i)].toDouble()));
            vError=std::max(vError,std::abs(v-reference["v"].toArray()[int(i)].toDouble()));
            for (uint j=0;j<count;++j) {
                const auto dj=dofs[int(j)].toObject(); const auto nj=uint(dj["nodePosition"].toInt());
                const double sj=dj["globalDirection"].toArray()[0].toDouble();
                energy+=.5*sj*(v*linear["M"].toArray()[int(i)].toArray()[int(j)].toDouble()*nodal(result,R_VARIABLE_VELOCITY,nj)
                    +q*linear["K"].toArray()[int(i)].toArray()[int(j)].toDouble()*nodal(result,R_VARIABLE_DISPLACEMENT,nj));
            }
        }
        std::cout<<std::setprecision(17)<<entry.name.toStdString()<<" q error m="<<qError
                 <<" v error m/s="<<vError<<" final energy J="<<energy<<std::endl;
        QVERIFY2(qError<entry.qTolerance,qPrintable(entry.name+" displacement mismatch"));
        QVERIFY2(vError<entry.vTolerance,qPrintable(entry.name+" velocity mismatch"));
        QVERIFY(energy<2e-5); // Independent passive four-rod baseline is 6.09e-4 J.
        if (entry.name.startsWith("learned")) {
            // Editing a file passed as --policy=path/--weights=path must invalidate
            // a checkpoint even if the JSON edit does not change the network.
            const auto option=entry.arguments.last();
            QFile policyFile(option.mid(option.indexOf('=')+1));
            QVERIFY(policyFile.open(QIODevice::WriteOnly|QIODevice::Append));
            QCOMPARE(policyFile.write("\n"),qint64(1)); policyFile.close();
            result.getProblemSetup().setRestart(true);
            result.getProblemSetup().getStructuralDynamicsSetup().setNSteps(steps+1);
            result.write(path,false);
            QFile acceptedFile(path); QVERIFY(acceptedFile.open(QIODevice::ReadOnly));
            const auto acceptedBytes=acceptedFile.readAll(); acceptedFile.close();
            QVERIFY(!run(binary("fea-solver"),{"--file="+path,"--control-program="+entry.program,
                "--control-args-json="+QString::fromUtf8(QJsonDocument(argv).toJson(QJsonDocument::Compact)),"--control-timeout-ms=10000"},log));
            QVERIFY2(log.contains("Structural control restart protocol, seed, actuator map or ledger does not match"),log.constData());
            QVERIFY(acceptedFile.open(QIODevice::ReadOnly));
            QCOMPARE(acceptedFile.readAll(),acceptedBytes);
        }
    }
}

QTEST_GUILESS_MAIN(TestControllerAdaptersCli)
#include "tst_controller_adapters_cli.moc"
