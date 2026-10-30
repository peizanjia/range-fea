#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <rpiezo_experiment_service.h>
#include <rpiezo_axial_experiment_service.h>
#include <piezo_native_backend_bridge.h>
#include <piezo_axial_backend_bridge.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rbl_error.h>
#include <cmath>
#include <algorithm>
#include <set>
#include <stdexcept>

#ifndef RANGE_PIEZO_BACKEND_PYTHON
#error "Local-direction tests require the real private Python backend"
#endif
#ifndef RANGE_PIEZO_BACKEND_MODULE_ROOT
#error "Local-direction tests require the actual workflow source"
#endif
namespace {
using V=std::vector<double>;
using M=std::vector<V>;
void require(bool b,const char *message){if(!b)throw std::runtime_error(message);}
QByteArray bytes(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QString sha(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
V vector(const QJsonValue &value){V v;for(auto x:value.toArray())v.push_back(x.toDouble());return v;}
M matrix(const QJsonValue &value){M a;for(auto row:value.toArray())a.push_back(vector(row));return a;}
QJsonArray array(const V &v){QJsonArray a;for(double x:v)a.append(x);return a;}
M rotation(){const double c=std::cos(.71),s=std::sin(.71),n=std::sqrt(14.);V u{1/n,2/n,3/n};M r(3,V(3));for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)r[i][j]=(i==j?c:0)+(1-c)*u[i]*u[j];r[0][1]-=s*u[2];r[0][2]+=s*u[1];r[1][0]+=s*u[2];r[1][2]-=s*u[0];r[2][0]-=s*u[1];r[2][1]+=s*u[0];return r;}
V mv(const M &a,const V &x){V y(a.size());for(size_t i=0;i<a.size();++i)for(size_t j=0;j<x.size();++j)y[i]+=a[i][j]*x[j];return y;}
V rotate(const V &x){auto r=rotation();V y(x.size());require(x.size()%3==0,"Rotation block dimensions");for(size_t k=0;k<x.size();k+=3)for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)y[k+i]+=r[i][j]*x[k+j];return y;}
void close(const V &a,const V &b,double tol=2e-7){require(a.size()==b.size(),"Comparison shape");double delta=0,norm=0;for(size_t i=0;i<a.size();++i){delta=std::hypot(delta,a[i]-b[i]);norm=std::hypot(norm,a[i]);norm=std::hypot(norm,b[i]);}require(delta<=tol*std::max(norm,1e-20),"Relative vector comparison");}
void close(double a,double b,double tol=2e-7){require(std::abs(a-b)<=tol*std::max({std::abs(a),std::abs(b),1e-20}),"Relative scalar comparison");}
QString python(){return QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON);}
QString modules(){return QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT);}
QJsonObject force(uint node,const QString &axis,double amplitude){return {{"channelId",QString("node:%1:%2").arg(node).arg(axis)},{"unit",axis.contains('R')?"N m":"N"},{"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression",QString::number(amplitude,'g',17)+"*(1+t)"}}}}};}
struct Fixture {RModel source;QJsonObject recipe;uint tip=0;};
Fixture fixture(const QString &family,bool rotated=false){
    RModel empty;empty.getTimeSolver().setInputStartTime(.001);auto recipe=RPiezoExperimentService::exampleRecipe(empty,family);
    require(!recipe.isEmpty(),"Example recipe unavailable");auto source=RPiezoExperimentService::templateCandidate(empty,recipe["template"].toObject());recipe.remove("template");recipe["replaceLegacyConditions"]=true;recipe["steps"]=6;
    const auto output=recipe["design"].toObject()["outputs"].toArray()[0].toObject();const uint tip=uint(output["physicalNodeId"].toDouble());
    recipe["design"]=QJsonObject{{"outputs",QJsonArray{QJsonObject{{"id","globalAcceleration"},{"physicalNodeId",double(tip)},{"dof","Z"},{"quantity","acceleration"},{"unit","m/s^2"}}}}};
    recipe["controller"]=family=="axialRod"?QJsonObject{{"kind","none"}}:QJsonObject{{"kind","PI"},{"selectedOutputs",QJsonArray{"sense"}},{"kp",.2},{"ki",.1}};
    recipe["excitation"]=QJsonObject{{"channels",QJsonArray{force(tip,"local:EZ",.03),force(tip,"local:RY",.002)}}};
    // Every global component of a clamped node is zero; the adapter determines
    // the independent constraint rank. This keeps the fixture physically equal
    // under a proper rotation, rather than constraining arbitrary global rows.
    std::set<uint> fixed;for(auto v:recipe["supports"].toArray())fixed.insert(uint(v.toObject()["physicalNodeId"].toDouble()));QJsonArray supports;const char *axes[]={"UX","UY","UZ","RX","RY","RZ"};
    for(uint node:fixed)for(auto axis:axes)if(family!="axialRod")supports.append(QJsonObject{{"physicalNodeId",double(node)},{"dof",axis},{"value",0.}});if(family!="axialRod")recipe["supports"]=supports;
    if(rotated){const auto r=rotation();V origin{.3,-.2,.4};for(uint i=0;i<source.getNNodes();++i){auto &n=source.getNode(i);auto p=mv(r,V{n.getX(),n.getY(),n.getZ()});n.setX(p[0]+origin[0]);n.setY(p[1]+origin[1]);n.setZ(p[2]+origin[2]);}
        auto frame=recipe["frame"].toObject();frame["origin"]=array(origin);const char *axes[]={"ex","ey","ez"};for(size_t j=0;j<3;++j)frame[axes[j]]=QJsonArray{r[0][j],r[1][j],r[2][j]};recipe["frame"]=frame;
    }return {source,recipe,tip};
}
RModel compose(const Fixture &f){const auto p=RPiezoExperimentService::preview(f.source,f.recipe);require(p.valid,qPrintable(p.message));return p.candidate;}
QJsonObject external(const QString &path,const QByteArray &source,const QJsonArray &channels){return {{"sourcePath",path},{"functionName","load"},{"sourceSha256",sha(source)},{"dependencyFiles",QJsonArray{}},{"environment",QJsonObject{{"pythonVersion","3.13.5"},{"numpyVersion","2.2.6"},{"scipyVersion","1.15.3"}}},{"channels",channels},{"missingPolicy","error"},{"cacheExactTime",true}};}
void write(const QString &path,const QByteArray &source){QFile f(path);require(f.open(QIODevice::WriteOnly)&&f.write(source)==source.size(),"External source write");}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoLocalDirections:public QObject {
    Q_OBJECT
private slots:
    void rotation_data();void rotation();void rejectedDirections_data();void rejectedDirections();
    void overlapAndExternal();void externalFamilies_data();void externalFamilies();
};
void TestPiezoLocalDirections::rotation_data()try {QTest::addColumn<QString>("family");QTest::newRow("actual-BEAM")<<QString("planarBeam");QTest::newRow("actual-QUAD")<<QString("flatPlate");} REPORT_EXCEPTIONS
void TestPiezoLocalDirections::rotation()try {
    QFETCH(QString,family);auto base=fixture(family),turned=fixture(family,true);auto a=compose(base),b=compose(turned);auto ab=PiezoNativeBackendBridge::create(a,python(),modules()),bb=PiezoNativeBackendBridge::create(b,python(),modules());
    close(vector(ab.exportPackage["design"].toObject()["omega"]),vector(bb.exportPackage["design"].toObject()["omega"]));
    const auto catalog=bb.exportPackage["forceCatalog"].toArray();size_t local=0;for(auto v:catalog){auto o=v.toObject();if(o["coordinateSystem"]=="local"){++local;QVERIFY(!o.contains("physicalIndex"));QCOMPARE(o["physicalDirection"].toArray().size(),6);}else if(family=="planarBeam")QFAIL("A generic beam rotation admits no pure global force/moment axis");else QVERIFY(!o["dof"].toString().startsWith('R'));}QVERIFY(local>0);
    const auto &e=bb.exportPackage;const auto S=matrix(e["physicalForceDirections"]),P=matrix(e["P"]),T=matrix(e["T"]),B=matrix(e["Bforce"]),D=matrix(e["Dforce"]),direct=matrix(e["physicalForceDirectFeedthrough"]);const size_t nf=B[0].size();
    // Independent contraction from exported actual FE P/T; no catalog row pick.
    for(size_t j=0;j<nf;++j){V s(S.size());for(size_t i=0;i<S.size();++i)s[i]=S[i][j];V pt(P[0].size());for(size_t i=0;i<P.size();++i)for(size_t k=0;k<pt.size();++k)pt[k]+=P[i][k]*s[i];V expected(T[0].size());for(size_t i=0;i<T.size();++i)for(size_t k=0;k<expected.size();++k)expected[k]+=T[i][k]*pt[i];V actual(B.size());for(size_t i=0;i<B.size();++i)actual[i]=B[i][j];close(actual,expected,1e-10);const auto total=mv(direct,s);for(size_t i=0;i<D.size();++i)close(D[i][j],total[i],1e-10);}
    // Acceleration direct derivative by an independent actual FE dynamics force
    // perturbation at the same initial state (consistent native mass solve).
    const auto &setup=b.getProblemSetup().getStructuralPiezoLayeredSetup();RStructuralPiezoLayeredModelAdapter adapter(b,setup);const auto force0=bb.callbacks.perturbationForce(0);auto perturbed=force0;const double h=1e-5;for(size_t i=0;i<S.size();++i)perturbed[i]+=h*S[i][0];
    RStructuralPiezoLayeredDynamics d0(adapter,RMatrixSolverConf(RMatrixSolverConf::GMRES),0,[force0](double){return force0;},"fd0"),d1(adapter,RMatrixSolverConf(RMatrixSolverConf::GMRES),0,[perturbed](double){return perturbed;},"fd1");const auto x0=d0.initialize(),x1=d1.initialize();const auto nodes=e["nodes"].toArray();size_t physical=0;for(auto v:nodes){auto o=v.toObject();if(uint(o["physicalNodeId"].toDouble())==turned.tip)physical=size_t(o["physicalOffset"].toDouble())+2;}close((x1.physicalAccelerations[physical]-x0.physicalAccelerations[physical])/h,D.back()[0],2e-6);
    RStructuralPiezoLayeredNativeRun ar(a,ab.callbacks),br(b,bb.callbacks);for(uint i=0;i<6;++i){auto ap=ar.prepare(a),bp=br.prepare(b);close(rotate(ap->result().physicalDisplacements),bp->result().physicalDisplacements);close(rotate(ap->result().physicalVelocities),bp->result().physicalVelocities);close(rotate(ap->result().physicalAccelerations),bp->result().physicalAccelerations);close(ap->result().kineticEnergy,bp->result().kineticEnergy);close(ap->result().storedEnergy,bp->result().storedEnergy);close(ap->result().backgroundExternalWork,bp->result().backgroundExternalWork);close(ap->result().electricalWork,bp->result().electricalWork);close(ap->runtime().lastApplied,bp->runtime().lastApplied);QCOMPARE(ap->result().readouts.size(),bp->result().readouts.size());for(size_t k=0;k<ap->result().readouts.size();++k)close(ap->result().readouts[k].value,bp->result().readouts[k].value);ar.validatePrepared(*ap);br.validatePrepared(*bp);a=ap->candidateModel();b=bp->candidateModel();ar.commit(*ap);br.commit(*bp);}
} REPORT_EXCEPTIONS
void TestPiezoLocalDirections::rejectedDirections_data()try {QTest::addColumn<QString>("family");QTest::addColumn<QString>("axis");for(auto axis:{"local:EY","local:RX","local:RZ","X","RY"})QTest::newRow((QByteArray("beam-")+axis).constData())<<QString("planarBeam")<<QString(axis);for(auto axis:{"local:RZ","RX","RY","RZ"})QTest::newRow((QByteArray("plate-")+axis).constData())<<QString("flatPlate")<<QString(axis);for(auto axis:{"local:EX","RX"})QTest::newRow((QByteArray("axial-")+axis).constData())<<QString("axialRod")<<QString(axis);} REPORT_EXCEPTIONS
void TestPiezoLocalDirections::rejectedDirections()try {QFETCH(QString,family);QFETCH(QString,axis);auto f=fixture(family,family!="axialRod");f.recipe["excitation"]=QJsonObject{{"channels",QJsonArray{force(f.tip,axis,0.)}}};const auto p=RPiezoExperimentService::preview(f.source,f.recipe);QVERIFY(!p.valid);QCOMPARE(p.candidate.getNNodes(),f.source.getNNodes());QVERIFY(!p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup().enabled);QTemporaryDir temp;QVERIFY(temp.isValid());const auto path=temp.filePath("zero.py");const QByteArray source("def load(t):\n    return {}\n");write(path,source);auto declared=external(path,source,QJsonArray{QJsonObject{{"channelId",QString("node:%1:%2").arg(f.tip).arg(axis)},{"unit",axis.contains('R')?"N m":"N"}}});declared["missingPolicy"]="zero";f.recipe["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{declared}}};QVERIFY(!RPiezoExperimentService::preview(f.source,f.recipe).valid);} REPORT_EXCEPTIONS
void TestPiezoLocalDirections::overlapAndExternal()try {
    auto f=fixture("planarBeam");const auto local=QString("node:%1:local:EZ").arg(f.tip);QTemporaryDir temp;QVERIFY(temp.isValid());const auto path=temp.filePath("sum.py");const auto source=("def load(t):\n    return {'"+local+"': {'value': 0.04*(1+t), 'unit': 'N'}}\n").toUtf8();write(path,source);
    f.recipe["excitation"]=QJsonObject{{"channels",QJsonArray{force(f.tip,"Z",.02),force(f.tip,"local:EZ",.03)}},{"externalFunctions",QJsonArray{external(path,source,QJsonArray{QJsonObject{{"channelId",local},{"unit","N"}}})}}};auto model=compose(f);auto bridge=PiezoNativeBackendBridge::create(model,python(),modules());auto forceAt=bridge.callbacks.perturbationForce(.007);const auto nodes=bridge.exportPackage["nodes"].toArray();for(auto v:nodes){auto o=v.toObject();if(uint(o["physicalNodeId"].toDouble())==f.tip)close(forceAt[size_t(o["physicalOffset"].toDouble())+2],.09*1.007,1e-12);}QCOMPARE(bridge.exportPackage["disturbanceInputs"].toArray().size(),2);
    RStructuralPiezoLayeredNativeRun run(model,bridge.callbacks);auto step=run.prepare(model);const auto before=run.acceptedRuntime().opaqueControllerState;run.validatePrepared(*step);model=step->candidateModel();run.commit(*step);QVERIFY(before!=run.acceptedRuntime().opaqueControllerState);
    const auto saved=temp.filePath("local.rbm");model.write(saved,false);RModel restored;restored.read(saved);restored.getProblemSetup().setRestart(true);auto restarted=PiezoNativeBackendBridge::create(restored,python(),modules());RStructuralPiezoLayeredNativeRun resumed(restored,restarted.callbacks);auto a=run.prepare(model),b=resumed.prepare(restored);close(a->result().physicalDisplacements,b->result().physicalDisplacements,1e-12);QCOMPARE(a->runtime().opaqueControllerState,b->runtime().opaqueControllerState);
    const auto accepted=run.acceptedRuntime().opaqueControllerState;write(path,source+"# stale\n");QVERIFY_EXCEPTION_THROWN(run.prepare(model),std::exception);QCOMPARE(run.acceptedRuntime().opaqueControllerState,accepted);QCOMPARE(run.completedSteps(),1u);
} REPORT_EXCEPTIONS
void TestPiezoLocalDirections::externalFamilies_data()try {QTest::addColumn<QString>("family");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");QTest::newRow("axial")<<QString("axialRod");} REPORT_EXCEPTIONS
void TestPiezoLocalDirections::externalFamilies()try {
    QFETCH(QString,family);auto f=fixture(family,family!="axialRod");if(family=="axialRod")f.recipe["controller"]=QJsonObject{{"kind","none"}};const auto id=QString("node:%1:%2").arg(f.tip).arg(family=="axialRod"?"X":"local:EZ");QTemporaryDir temp;QVERIFY(temp.isValid());const auto path=temp.filePath("loads.py");const auto source=("def load(t):\n    return {'"+id+"': {'value': 0.01*(1+t), 'unit': 'N'}}\n").toUtf8();write(path,source);auto declaration=external(path,source,QJsonArray{QJsonObject{{"channelId",id},{"unit","N"}}});f.recipe["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{declaration}}};auto model=compose(f);
    if(family=="axialRod"){auto bridge=PiezoAxialBackendBridge::create(model,python(),modules());QCOMPARE(bridge.exportPackage["disturbanceInputs"].toArray().size(),1);auto l=bridge.callbacks.perturbationForce(.001),r=bridge.callbacks.perturbationForce(.002);QVERIFY(l!=r);close(l,bridge.callbacks.perturbationForce(.001),1e-12);}
    else{auto bridge=PiezoNativeBackendBridge::create(model,python(),modules());QCOMPARE(bridge.exportPackage["disturbanceInputs"].toArray().size(),1);auto l=bridge.callbacks.perturbationForce(.001),r=bridge.callbacks.perturbationForce(.002);QVERIFY(l!=r);close(l,bridge.callbacks.perturbationForce(.001),1e-12);}
    const auto evaluate=[&](const QJsonObject &entry){auto changed=f;changed.recipe["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{entry}}};auto candidate=compose(changed);if(family=="axialRod"){auto bridge=PiezoAxialBackendBridge::create(candidate,python(),modules());return bridge.callbacks.perturbationForce(.001);}auto bridge=PiezoNativeBackendBridge::create(candidate,python(),modules());return bridge.callbacks.perturbationForce(.001);};
    auto bad=declaration;bad["sourceSha256"]=QString(64,'0');QVERIFY_EXCEPTION_THROWN(evaluate(bad),std::exception);
    bad=declaration;auto env=bad["environment"].toObject();env["numpyVersion"]="0.invalid";bad["environment"]=env;QVERIFY_EXCEPTION_THROWN(evaluate(bad),std::exception);
    const auto dep=temp.filePath("reference.txt");const QByteArray depBytes("frozen dependency\n");write(dep,depBytes);bad=declaration;bad["dependencyFiles"]=QJsonArray{QJsonObject{{"path",dep},{"sha256",sha(depBytes)}}};const auto validDependency=evaluate(bad);QVERIFY(!validDependency.empty());write(dep,depBytes+"stale\n");QVERIFY_EXCEPTION_THROWN(evaluate(bad),std::exception);
    const QByteArray missing("def load(t):\n    return {}\n");write(path,missing);bad=declaration;bad["sourceSha256"]=sha(missing);QVERIFY_EXCEPTION_THROWN(evaluate(bad),std::exception);bad["missingPolicy"]="zero";for(double x:evaluate(bad))QCOMPARE(x,0.);write(path,source);
    // Every family persists the accepted controller/force identity and resumes
    // using a newly started production provider with the same exact clock.
    const auto saved=temp.filePath("restart.rbm");
    if(family=="axialRod"){auto bridge=PiezoAxialBackendBridge::create(model,python(),modules());RStructuralPiezoAxialNativeRun run(model,bridge.callbacks);auto first=run.prepare(model);run.validatePrepared(*first);model=first->candidateModel();run.commit(*first);model.write(saved,false);RModel restored;restored.read(saved);restored.getProblemSetup().setRestart(true);auto other=PiezoAxialBackendBridge::create(restored,python(),modules());RStructuralPiezoAxialNativeRun resumed(restored,other.callbacks);auto uninterrupted=run.prepare(model),restarted=resumed.prepare(restored);close(uninterrupted->result().displacements,restarted->result().displacements,1e-12);QCOMPARE(uninterrupted->runtime().opaqueControllerState,restarted->runtime().opaqueControllerState);}
    else{auto bridge=PiezoNativeBackendBridge::create(model,python(),modules());RStructuralPiezoLayeredNativeRun run(model,bridge.callbacks);auto first=run.prepare(model);run.validatePrepared(*first);model=first->candidateModel();run.commit(*first);model.write(saved,false);RModel restored;restored.read(saved);restored.getProblemSetup().setRestart(true);auto other=PiezoNativeBackendBridge::create(restored,python(),modules());RStructuralPiezoLayeredNativeRun resumed(restored,other.callbacks);auto uninterrupted=run.prepare(model),restarted=resumed.prepare(restored);close(uninterrupted->result().physicalDisplacements,restarted->result().physicalDisplacements,1e-12);QCOMPARE(uninterrupted->runtime().opaqueControllerState,restarted->runtime().opaqueControllerState);}
    // Shared parser remains strict independently of successful execution.
    declaration["arbitraryDirection"]=QJsonArray{1,1,1};f.recipe["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{declaration}}};QVERIFY(!RPiezoExperimentService::preview(f.source,f.recipe).valid);
} REPORT_EXCEPTIONS
QTEST_GUILESS_MAIN(TestPiezoLocalDirections)
#include "tst_piezo_local_directions.moc"
