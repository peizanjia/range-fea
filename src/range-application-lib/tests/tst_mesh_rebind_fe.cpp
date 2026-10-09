#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <rbl_error.h>
#include <rmesh_rebind_service.h>
#include <rmodel_command_service.h>
#include <rsolverstress.h>

#ifndef RANGE_MESH_REBIND_SOLVER_PATH
#error RANGE_MESH_REBIND_SOLVER_PATH must identify the native fea-solver executable
#endif

namespace
{
constexpr uint physicalIds[4]={101,111,121,131};
constexpr double coordinates[4][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
RModel unboundTetrahedron()
{
    RModel m; m.setNNodes(4); m.setNSurfaces(1); m.setNVolumes(1);
    for (uint n=0;n<4;++n)
    {
        m.getNode(n).set(coordinates[n][0],coordinates[n][1],coordinates[n][2]);
        m.setNodePhysicalId(n,physicalIds[n]);
    }
    const uint faces[4][3]={{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for (const auto &f:faces)
    {
        RElement e(R_ELEMENT_TRI1); for (uint j=0;j<3;++j) e.setNodeId(j,f[j]); m.addElement(e,true,0);
    }
    RElement e(R_ELEMENT_TETRA1); for (uint j=0;j<4;++j) e.setNodeId(j,j); m.addElement(e,true,0);
    m.getSurface(0).setName("outer"); m.getVolume(0).setName("body");
    m.setProblemTaskTree(RProblemTaskItem()); m.setProblemSetup(RProblemSetup()); m.setTimeSolver(RTimeSolver());
    m.clearResults(); m.clearBoundaryConditions(); m.clearInitialConditions(); m.clearEntityVariableData();
    for (uint g=0;g<m.getNElementGroups();++g)
        while (m.getElementGroupPtr(g)->getNEnvironmentConditions()) m.getElementGroupPtr(g)->removeEnvironmentCondition(0u);
    return m;
}
QJsonObject binding()
{
    const QJsonValue null(QJsonValue::Null);
    return {{"unitSI",true},{"staticStress",true},
        {"material",QJsonObject{{"youngModulusPa",1000},{"poissonRatio",0.25},{"densityKgM3",1}}},
        {"constraints",QJsonArray{
            QJsonObject{{"nodePhysicalId",101},{"values",QJsonArray{0,0,0}}},
            QJsonObject{{"nodePhysicalId",111},{"values",QJsonArray{null,0,0}}},
            QJsonObject{{"nodePhysicalId",121},{"values",QJsonArray{null,null,0}}}}},
        {"loads",QJsonArray{QJsonObject{{"nodePhysicalId",131},{"forceN",QJsonArray{0,0,-1}}}}}};
}
QByteArray hash(const QString &path)
{
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(),QCryptographicHash::Sha256);
}
void near(double actual,double expected,double tolerance=1e-9)
{
    QVERIFY2(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,
             qPrintable(QString("actual=%1 expected=%2 tolerance=%3").arg(actual,0,'g',17).arg(expected,0,'g',17).arg(tolerance)));
}

// Textbook constant-strain tetrahedron, independent of Range's element,
// material, quadrature and matrix assembly routines. N={1-x-y-z,x,y,z},
// V=1/6 m^3; engineering strain ordering [xx,yy,zz,xy,yz,xz].
// E=1000 Pa, nu=1/4 -> lambda=mu=400 Pa. K=V B^T D B.
// With six rigid-motion restraints and F_3z=-1 N, uniaxial stress is -6 Pa:
// q_1x=q_2y=0.0015 m, q_3z=-0.006 m, R_0z=1 N, U=0.003 J.
// This single linear tetrahedron is not a general mesh accuracy certificate.
struct IndependentTet
{
    double b[6][12]{},d[6][6]{},k[12][12]{};
    IndependentTet()
    {
        const double gradient[4][3]={{-1,-1,-1},{1,0,0},{0,1,0},{0,0,1}};
        for (int n=0;n<4;++n)
        {
            const auto &g=gradient[n]; b[0][3*n]=g[0]; b[1][3*n+1]=g[1]; b[2][3*n+2]=g[2];
            b[3][3*n]=g[1]; b[3][3*n+1]=g[0]; b[4][3*n+1]=g[2]; b[4][3*n+2]=g[1];
            b[5][3*n]=g[2]; b[5][3*n+2]=g[0];
        }
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) d[i][j]=400+(i==j ? 800 : 0);
        for (int i=3;i<6;++i) d[i][i]=400;
        for (int i=0;i<12;++i) for (int j=0;j<12;++j)
            for (int p=0;p<6;++p) for (int s=0;s<6;++s) k[i][j]+=b[p][i]*d[p][s]*b[s][j]/6;
    }
};
void verifyPhysicalResults(const RModel &m,const QString &evidencePath)
{
    QCOMPARE(m.getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_STRESS);
    QVERIFY(!m.getProblemSetup().getStructuralControlSetup().enabled);
    QVERIFY(!m.getProblemSetup().getStructuralDynamicsSetup().getEnabled());
    QVERIFY(!m.getProblemSetup().getStructuralDynamicsRestartData().available);
    const uint displacement=m.findVariable(R_VARIABLE_DISPLACEMENT);
    QVERIFY(displacement!=RConstants::eod);
    const auto &u=m.getVariable(displacement);
    QCOMPARE(u.getApplyType(),R_VARIABLE_APPLY_NODE); QCOMPARE(u.getUnits(),QString("m"));
    QCOMPARE(u.getNVectors(),3u); QCOMPARE(u.getNValues(),4u);
    std::array<double,12> q{};
    QJsonArray nodal;
    const double exact[12]={0,0,0,0.0015,0,0,0,0.0015,0,0,0,-0.006};
    for (uint n=0;n<4;++n)
    {
        uint index=RConstants::eod;
        for (uint i=0;i<m.getNNodes();++i) if (m.getNodePhysicalId(i)==physicalIds[n]) index=i;
        QVERIFY(index!=RConstants::eod);
        QJsonArray values;
        for (uint c=0;c<3;++c)
        {
            q[3*n+c]=u.getValue(c,index); near(q[3*n+c],exact[3*n+c],1e-11); values.append(q[3*n+c]);
        }
        nodal.append(QJsonObject{{"nodePhysicalId",int(physicalIds[n])},{"displacementM",values}});
    }
    IndependentTet ref;
    double strain[6]{},stress[6]{},internal[12]{},reaction[12]{},energy=0,externalWork=0,supportWork=0;
    const bool fixed[12]={true,true,true,false,true,true,false,false,true,false,false,false};
    double force[12]{}; force[11]=-1;
    for (int i=0;i<6;++i) for (int j=0;j<12;++j) strain[i]+=ref.b[i][j]*q[j];
    for (int i=0;i<6;++i) for (int j=0;j<6;++j) stress[i]+=ref.d[i][j]*strain[j];
    double residual=0; QJsonArray reactions,strains,actualStress;
    for (int i=0;i<12;++i)
    {
        for (int j=0;j<12;++j) internal[i]+=ref.k[i][j]*q[j];
        energy+=q[i]*internal[i]/2; externalWork+=force[i]*q[i];
        if (fixed[i]) { reaction[i]=internal[i]-force[i]; supportWork+=reaction[i]*q[i]; }
        else residual=std::max(residual,std::abs(internal[i]-force[i]));
        near(reaction[i],i==2 ? 1 : 0); reactions.append(reaction[i]);
    }
    QVERIFY2(residual<1e-9,"Independent K*q-f free-DOF residual exceeds 1e-9 N for a 1 N load");
    near(energy,0.003,1e-11); near(supportWork,0,1e-12); near(2*energy,externalWork+supportWork,1e-11);
    double resultant[3]{},moment[3]{};
    for (int n=0;n<4;++n)
    {
        const double f[3]={reaction[3*n]+force[3*n],reaction[3*n+1]+force[3*n+1],reaction[3*n+2]+force[3*n+2]};
        for (int c=0;c<3;++c) resultant[c]+=f[c];
        moment[0]+=coordinates[n][1]*f[2]-coordinates[n][2]*f[1];
        moment[1]+=coordinates[n][2]*f[0]-coordinates[n][0]*f[2];
        moment[2]+=coordinates[n][0]*f[1]-coordinates[n][1]*f[0];
    }
    for (int c=0;c<3;++c) { near(resultant[c],0); near(moment[c],0); }
    QCOMPARE(m.getVolume(0).size(),1u); const uint tet=m.getVolume(0).get(0);
    QCOMPARE(m.getElement(tet).getType(),R_ELEMENT_TETRA1);
    const RVariableType types[6]={R_VARIABLE_STRESS_X,R_VARIABLE_STRESS_Y,R_VARIABLE_STRESS_Z,
                                R_VARIABLE_STRESS_XY,R_VARIABLE_STRESS_YZ,R_VARIABLE_STRESS_XZ};
    for (int c=0;c<6;++c)
    {
        const uint position=m.findVariable(types[c]); QVERIFY(position!=RConstants::eod);
        const auto &v=m.getVariable(position);
        QCOMPARE(v.getApplyType(),R_VARIABLE_APPLY_ELEMENT); QCOMPARE(v.getUnits(),QString("Pa"));
        QCOMPARE(v.getNVectors(),1u); QCOMPARE(v.getNValues(),m.getNElements());
        const double actual=v.getValue(0,tet);
        near(actual,c==2 ? -6 : 0); near(actual,stress[c]);
        actualStress.append(actual); strains.append(strain[c]);
    }
    const QJsonObject evidence{{"scope","one_linear_unit_tetrahedron"},{"solver","RSolverStress::run"},
        {"nodalResults",nodal},{"nativeStressPa_xx_yy_zz_xy_yz_xz",actualStress},
        {"strainFromActualDisplacement",strains},{"reactionsFromIndependentKAndActualDisplacementN",reactions},
        {"freeResidualInfinityNormN",residual},{"strainEnergyFromIndependentKAndActualDisplacementJ",energy},
        {"supportWorkJ",supportWork},{"twiceStrainEnergyMinusExternalAndSupportWorkJ",2*energy-externalWork-supportWork},
        {"forceBalanceN",QJsonArray{resultant[0],resultant[1],resultant[2]}},
        {"momentBalanceNm",QJsonArray{moment[0],moment[1],moment[2]}}};
    QFile output(evidencePath); QVERIFY(output.open(QIODevice::WriteOnly));
    const QByteArray json=QJsonDocument(evidence).toJson(QJsonDocument::Indented);
    QCOMPARE(output.write(json),qint64(json.size())); output.close();
    qInfo().noquote()<<QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact));
}
void solveAndReopen(RModel model,const QString &directory,const QString &prefix)
{
    const QString boundPath=model.write(QDir(directory).filePath(prefix+"-bound.rbm"),false);
    const QByteArray before=hash(boundPath); QVERIFY(!before.isEmpty());
    RModel reopened; reopened.read(boundPath);
    QCOMPARE(RModelCommandService::stateFingerprint(reopened),RModelCommandService::stateFingerprint(model));
    reopened.getMatrixSolverConf(RMatrixSolverConf::CG).setOutputFrequency(0);
    reopened.getMatrixSolverConf(RMatrixSolverConf::CG).setSolverCvgValue(1e-13);
    reopened.getMatrixSolverConf(RMatrixSolverConf::CG).setNInnerIterations(100);
    reopened.getMatrixSolverConf(RMatrixSolverConf::CG).setNOuterIterations(1000);
    const QString resultPath=QDir(directory).filePath(prefix+"-result.rbm");
    QString actualRecord;
    try
    {
        RSolverSharedData shared;
        RSolverStress solver(&reopened,resultPath,QString(),shared,false);
        solver.setResultFrameObserver([&actualRecord](const RSolverResultFrame &frame) { actualRecord=frame.actualRecordPath; });
        solver.run(true,0);
    }
    catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
    catch (const std::exception &error) { QFAIL(error.what()); }
    QVERIFY(!actualRecord.isEmpty()); QVERIFY(QFileInfo::exists(actualRecord));
    QVERIFY(QFileInfo(actualRecord).absoluteFilePath()!=QFileInfo(boundPath).absoluteFilePath());
    QCOMPARE(hash(boundPath),before);
    RModel persisted; persisted.read(actualRecord);
    verifyPhysicalResults(persisted,QDir(directory).filePath(prefix+"-physical-evidence.json"));
}
}

class MeshRebindFETest : public QObject
{
    Q_OBJECT
private slots:
    void nativeUnboundSourceFailsWithoutResultOrController()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const RModel source=unboundTetrahedron();
        QCOMPARE(source.getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_NONE);
        const QString input=source.write(directory.filePath("unbound.rbm"),false);
        const QByteArray before=hash(input);
        QProcess process; process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QStringLiteral(RANGE_MESH_REBIND_SOLVER_PATH),{"--file="+input,"--nthreads=1","--log-quiet"});
        QVERIFY2(process.waitForStarted(10000),qPrintable(process.errorString()));
        const bool finished=process.waitForFinished(30000);
        if (!finished) { process.kill(); process.waitForFinished(5000); }
        const QByteArray log=process.readAll();
        QVERIFY2(finished,log.constData()); QCOMPARE(process.exitStatus(),QProcess::NormalExit);
        QVERIFY2(process.exitCode()!=0,log.constData());
        QVERIFY2(log.contains("No solver task is configured")
                 && log.contains("requires explicit physics rebinding"),log.constData());
        QCOMPARE(hash(input),before);
        QCOMPARE(QDir(directory.path()).entryList({"*.rbm","*.ram"},QDir::Files),QStringList{"unbound.rbm"});
        RModel after; after.read(input); QCOMPARE(after.getNVariables(),0u);
        QVERIFY(!after.getProblemSetup().getStructuralControlSetup().enabled);
        QVERIFY(after.getProblemSetup().getStructuralControlSetup().controllerId.isEmpty());
        QVERIFY(!after.getProblemSetup().getStructuralDynamicsRestartData().available);
    }
    void reboundSavedModelRunsActualNativeStressFE()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const RModel source=unboundTetrahedron(); const QString state=RModelCommandService::stateFingerprint(source);
        const auto rebound=RMeshRebindService::preview(source,binding()); QVERIFY2(rebound.valid,qPrintable(rebound.message));
        QVERIFY(!RMeshRebindService::hasMaterialFreeStaticBoundaryRoles(source));
        const QString boundState=RModelCommandService::stateFingerprint(rebound.candidate);
        QVERIFY(RMeshRebindService::hasMaterialFreeStaticBoundaryRoles(rebound.candidate));
        QCOMPARE(RModelCommandService::stateFingerprint(rebound.candidate),boundState);
        QCOMPARE(rebound.candidate.getSurface(0).getMaterial().size(),0u);
        QCOMPARE(rebound.candidate.getSurface(0).getThickness(),0.0);
        for (uint p=0;p<rebound.candidate.getNPoints();++p) {
            QCOMPARE(rebound.candidate.getPoint(p).getMaterial().size(),0u);
            QCOMPARE(rebound.candidate.getPoint(p).getVolume(),0.0);
        }
        solveAndReopen(rebound.candidate,directory.path(),"rebound");
        QCOMPARE(RModelCommandService::stateFingerprint(source),state); QCOMPARE(source.getNVariables(),0u);
    }
    void redoSavedModelRunsActualNativeStressFE()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const RModel source=unboundTetrahedron();
        const auto preview=RModelCommandService::preview(source,"mesh.rebind.static",binding());
        QVERIFY2(preview.valid,qPrintable(preview.message));
        const auto undo=RModelCommandService::applyDelta(preview.candidate,preview.delta,false);
        QVERIFY2(undo.valid,qPrintable(undo.message));
        QCOMPARE(undo.candidate.getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_NONE);
        QCOMPARE(undo.candidate.getNVariables(),0u); QCOMPARE(undo.candidate.getNPoints(),0u);
        const auto redo=RModelCommandService::applyDelta(undo.candidate,preview.delta,true);
        QVERIFY2(redo.valid,qPrintable(redo.message));
        QCOMPARE(RModelCommandService::stateFingerprint(redo.candidate),RModelCommandService::stateFingerprint(preview.candidate));
        solveAndReopen(redo.candidate,directory.path(),"redo");
    }
};

QTEST_GUILESS_MAIN(MeshRebindFETest)
#include "tst_mesh_rebind_fe.moc"
