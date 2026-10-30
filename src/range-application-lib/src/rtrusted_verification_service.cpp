#include <rtrusted_verification_service.h>
#include <rnative_result_broker.h>
#include <rtrusted_mechanics_service.h>
#include <rml_model.h>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using Vec = std::vector<double>;
using Mat = std::vector<Vec>;
constexpr double pi = 3.14159265358979323846;
constexpr double displacementLimit = 0.01;
constexpr double frequencyLimit = 0.02;

Mat matrix(size_t n) { return Mat(n, Vec(n, 0.0)); }
bool finite(const Vec &v) { return std::all_of(v.begin(), v.end(), [](double x){ return std::isfinite(x); }); }
bool sha256(const QString &s)
{
    if(s.size()!=64) return false;
    return std::all_of(s.begin(),s.end(),[](QChar c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
// Independent dense elimination, deliberately not a production solver call.
bool solve(Mat a, Vec b, Vec &x)
{
    const size_t n = b.size();
    for (size_t k = 0; k < n; ++k) {
        size_t p = k;
        for (size_t i = k+1; i < n; ++i) if (std::abs(a[i][k]) > std::abs(a[p][k])) p = i;
        if (!std::isfinite(a[p][k]) || a[p][k] == 0.0) return false;
        std::swap(a[p], a[k]); std::swap(b[p], b[k]);
        for (size_t i = k+1; i < n; ++i) {
            const double r = a[i][k]/a[k][k];
            for (size_t j = k+1; j < n; ++j) a[i][j] -= r*a[k][j];
            b[i] -= r*b[k];
        }
    }
    x.assign(n, 0.0);
    for (size_t i = n; i-- > 0;) {
        double v = b[i];
        for (size_t j = i+1; j < n; ++j) v -= a[i][j]*x[j];
        x[i] = v/a[i][i];
    }
    return finite(x);
}

struct System
{
    bool beam = false;
    int stride = 1;
    Mat k, m, c;
    std::vector<int> free;
    Vec x;
};

// Standard two-node axial/torsion and cubic Hermite Euler-Bernoulli matrices.
// The reference contains no calls to production assembly or time integration.
System assemble(const RModel &model, const QJsonObject &s)
{
    System r;
    r.beam = s["modelType"] == QLatin1String("cantileverBeam"); r.stride = r.beam ? 6 : 1;
    const int n = int(model.getNNodes()), nd = n*r.stride;
    r.k = matrix(nd); r.m = matrix(nd); r.c = matrix(nd); r.x.resize(n);
    const auto material = s["material"].toObject(), section = s["section"].toObject();
    const double e = material["youngModulus"].toDouble(), rho = material["density"].toDouble();
    const double area = section["area"].toDouble(), iy = section["iy"].toDouble(), iz = section["iz"].toDouble();
    const double g = e/(2*(1+material["poissonRatio"].toDouble()));
    for (int i = 0; i < n; ++i) {
        r.x[i] = model.getNode(uint(i)).getX();
        if (r.x[i] != 0.0) for (int d = 0; d < r.stride; ++d) r.free.push_back(i*r.stride+d);
    }
    for (uint el = 0; el < model.getNElements(); ++el) {
        const auto &element = model.getElement(el);
        if (element.getType() != (r.beam ? R_ELEMENT_BEAM1 : R_ELEMENT_TRUSS1)) continue;
        int a = int(element.getNodeId(0)), b = int(element.getNodeId(1));
        if (r.x[a] > r.x[b]) std::swap(a,b);
        const double l = r.x[b]-r.x[a];
        auto pair = [&](int d, double stiffness, double mass) {
            const int u = a*r.stride+d, v = b*r.stride+d;
            r.k[u][u] += stiffness/l; r.k[v][v] += stiffness/l;
            r.k[u][v] -= stiffness/l; r.k[v][u] -= stiffness/l;
            r.m[u][u] += mass*l/3; r.m[v][v] += mass*l/3;
            r.m[u][v] += mass*l/6; r.m[v][u] += mass*l/6;
        };
        pair(0, e*area, rho*area);
        if (!r.beam) continue;
        pair(3, g*section["torsionalConstant"].toDouble(), rho*(iy+iz));
        for (int plane = 0; plane < 2; ++plane) {
            const double sign = plane == 0 ? 1.0 : -1.0;
            const double inertia = plane == 0 ? iz : iy;
            const int d = plane == 0 ? 1 : 2, rot = plane == 0 ? 5 : 4;
            const int ids[4] = {a*6+d,a*6+rot,b*6+d,b*6+rot};
            const double transform[4] = {1,sign,1,sign};
            const double kk[4][4] = {{12,6*l,-12,6*l},{6*l,4*l*l,-6*l,2*l*l},
                {-12,-6*l,12,-6*l},{6*l,2*l*l,-6*l,4*l*l}};
            const double mm[4][4] = {{156,22*l,54,-13*l},{22*l,4*l*l,13*l,-3*l*l},
                {54,13*l,156,-22*l},{-13*l,-3*l*l,-22*l,4*l*l}};
            for (int i=0;i<4;++i) for(int j=0;j<4;++j) {
                r.k[ids[i]][ids[j]] += transform[i]*transform[j]*e*inertia/(l*l*l)*kk[i][j];
                r.m[ids[i]][ids[j]] += transform[i]*transform[j]*rho*area*l/420*mm[i][j];
            }
        }
    }
    const auto analysis = s["analysis"].toObject();
    for(int i=0;i<nd;++i) for(int j=0;j<nd;++j)
        r.c[i][j] = analysis["rayleighAlpha"].toDouble()*r.m[i][j]+analysis["rayleighBeta"].toDouble()*r.k[i][j];
    return r;
}

Vec forceAt(const System &r, const QJsonObject &s, double t, const QString &revision)
{
    Vec f(r.k.size(),0.0);
    const double length = s["geometry"].toObject()["length"].toDouble();
    Q_UNUSED(revision);
    for (const auto &value : s["loads"].toArray()) {
        const auto load = value.toObject();
        const auto selector = load["selector"].toObject();
        double position = length;
        if(selector["kind"] == QLatin1String("fixedEnd")) position = 0.0;
        else if(selector["kind"] == QLatin1String("position")) {
            const double requested=selector["x"].toDouble();
            position=*std::min_element(r.x.begin(),r.x.end(),[&](double a,double b){return std::abs(a-requested)<std::abs(b-requested);});
        }
        QJsonArray components = load["components"].toArray();
        const auto history = load["history"].toArray();
        if(!history.isEmpty()) {
            components = history.first().toObject()["components"].toArray();
            for(int h=1;h<history.size();++h) {
                const auto left = history[h-1].toObject(), right = history[h].toObject();
                if(t >= right["time"].toDouble()) { components = right["components"].toArray(); continue; }
                const double fraction = std::max(0.0,(t-left["time"].toDouble())/(right["time"].toDouble()-left["time"].toDouble()));
                components = {};
                for(int axis=0;axis<3;++axis) components.append((1-fraction)*left["components"].toArray()[axis].toDouble()+fraction*right["components"].toArray()[axis].toDouble());
                break;
            }
        }
        for(size_t i=0;i<r.x.size();++i) if(std::abs(r.x[i]-position) <= 1e-12*std::max(1.0,length))
            for(int axis=0;axis<(r.beam?3:1);++axis) f[i*r.stride+axis] += components[axis].toDouble();
    }
    return f;
}

Mat reduced(const Mat &a, const std::vector<int> &ids)
{
    Mat b = matrix(ids.size());
    for(size_t i=0;i<ids.size();++i) for(size_t j=0;j<ids.size();++j) b[i][j] = a[ids[i]][ids[j]];
    return b;
}
Vec reduced(const Vec &a, const std::vector<int> &ids)
{
    Vec b; for(int id:ids) b.push_back(a[id]); return b;
}

bool field(const RModel &m, RVariableType type, Vec &values)
{
    const uint id = m.findVariable(type);
    if(id == RConstants::eod) return false;
    const auto &v = m.getVariable(id);
    if(v.getApplyType()!=R_VARIABLE_APPLY_NODE || v.getNVectors()!=3 || v.getNValues()!=m.getNNodes()
        || v.getUnits()!=RVariable::getUnits(type)) return false;
    values.clear();
    for(uint node=0;node<m.getNNodes();++node) for(uint axis=0;axis<3;++axis) values.push_back(v.getValue(axis,node));
    return finite(values);
}

double relativeError(const Vec &actual, const Vec &reference)
{
    if(actual.size()!=reference.size() || !finite(actual) || !finite(reference)) return std::numeric_limits<double>::infinity();
    double scale=0,error=0;
    for(size_t i=0;i<actual.size();++i) { scale=std::max(scale,std::abs(reference[i])); error=std::max(error,std::abs(actual[i]-reference[i])); }
    // Zero reference is an absolute zero check, never a silently enlarged relative denominator.
    return scale==0.0 ? (error==0.0?0.0:std::numeric_limits<double>::infinity()) : error/scale;
}
}

QJsonObject RTrustedVerificationService::verify(const QJsonObject &problemSpec, const QJsonObject &expectedIdentity,
                                               const QString &manifestPath, const QString &expectedManifestSha256,
                                               bool includeConvergenceMeasurements)
{
    QJsonObject metrics;
    QJsonObject convergenceMeasurements;
    const QStringList names{"identity","specification","modelSupport","nodalFields","supportValues","timeGrid",
        "displacement","frequency","dynamicEquilibrium","referenceConvergence","staticResidual",
        "reactionBalance","supportReactions","strainEnergy","modalResidual","modeShape","energyBalance","kinematicConsistency",
        "sdofAnalyticDisplacement","frozenForcedOscillator","rodHistoryReference"};
    for(const auto &name:names) metrics.insert(name,QJsonObject{{"status","notComputed"},{"required",true},{"reason","Prerequisite not satisfied."}});
    auto metric = [&](const QString &name,const QString &status,const QString &reason, QJsonObject evidence=QJsonObject{}) {
        QJsonObject m{{"status",status},{"required",status!=QLatin1String("notApplicable")},{"reason",reason}};
        if(!evidence.isEmpty()) m.insert("evidence",evidence);
        metrics.insert(name,m);
    };
    QJsonObject bound{{"manifestSha256",expectedManifestSha256}};
    double effectiveDisplacement=displacementLimit,effectiveFrequency=frequencyLimit;
    QJsonObject thresholds{{"displacementRelative",displacementLimit},{"frequencyRelative",frequencyLimit},
        {"fixedDisplacementRelative",displacementLimit},{"fixedFrequencyRelative",frequencyLimit},
        {"sdofScaledAbsoluteDisplacement",1e-4},{"rodHistoryReferenceAbsoluteM",1e-4}};
    auto finish = [&]() {
        bool passed=true;
        for(auto it=metrics.begin();it!=metrics.end();++it) {
            const auto m=it.value().toObject();
            if(m["required"].toBool() && m["status"]!=QLatin1String("pass")) passed=false;
        }
        QJsonObject report{{"schemaVersion",1},{"schema","range.verification-report/v1"},
            {"status",passed?"pass":"fail"},{"passed",passed},{"identity",bound},{"metrics",metrics},
            {"thresholds",thresholds}};
        if (includeConvergenceMeasurements && !convergenceMeasurements.isEmpty())
            report.insert("convergenceMeasurements", convergenceMeasurements);
        return report;
    };
    const auto validation=RTrustedMechanicsService::validate(problemSpec);
    metric("specification",validation.valid?"pass":"fail",validation.valid?"ProblemSpec v1 normalized to SI.":"Invalid ProblemSpec or units.",{{"issues",validation.issues}});
    if(!validation.valid) return finish();
    const auto s=validation.normalizedSpec;
    const QString analysisKind=s["analysis"].toObject()["kind"].toString();
    const QString precisionKey=analysisKind=="modal"?"frequencyRelative":(analysisKind=="linearTransient"?"responseRelative":"displacementRelative");
    const double requestedPrecision=s["precision"].toObject()[precisionKey].toDouble();
    if(analysisKind=="modal") effectiveFrequency=std::min(frequencyLimit,requestedPrecision);
    else effectiveDisplacement=std::min(displacementLimit,requestedPrecision);
    thresholds.insert("requestedPrecision",QJsonObject{{"quantity",precisionKey},{"relative",requestedPrecision}});
    thresholds.insert("effectiveDisplacementRelative",effectiveDisplacement);thresholds.insert("effectiveFrequencyRelative",effectiveFrequency);
    bound.insert("normalizedProblemSpecSha256",QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(s).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()));
    bound.insert("physicalSummarySha256",RTrustedMechanicsService::summaryDigest(RTrustedMechanicsService::physicalSummary(s)));
    const auto data=RNativeResultBroker::readVerifiedModels(manifestPath,expectedManifestSha256);
    if(!data.valid) { metric("identity","fail",data.code+": "+data.message); return finish(); }
    bound.insert("artifactId",data.artifactId);bound.insert("jobId",data.jobId);
    bound.insert("inputSnapshotSha256",data.inputSnapshotSha256);bound.insert("sourceSnapshotRef",data.sourceSnapshotRef);
    bound.insert("inputPackageSha256",data.inputPackageSha256);
    bound.insert("inputModelSha256",data.sourceSnapshotRef["modelSha256"]);
    if(data.sourceSnapshotRef.contains("modelFingerprint")) bound.insert("inputModelFingerprint",data.sourceSnapshotRef["modelFingerprint"]);
    QJsonArray recordIdentities;
    for(const auto &record:data.records) recordIdentities.append(QJsonObject{{"recordNo",record.recordNo},{"timeS",record.timeS},{"originKind",record.originKind},{"modelSha256",record.modelSha256}});
    bound.insert("resultRecords",recordIdentities);
    bool identity=true;
    for(const QString &key:{QString("artifactId"),QString("jobId"),QString("inputSnapshotSha256"),QString("inputPackageSha256")})
        identity &= expectedIdentity[key].isString() && !expectedIdentity[key].toString().isEmpty() && expectedIdentity[key]==bound[key];
    identity &= expectedIdentity["sourceSnapshotRef"].isObject() && !expectedIdentity["sourceSnapshotRef"].toObject().isEmpty()
        && expectedIdentity["sourceSnapshotRef"]==bound["sourceSnapshotRef"];
    const auto expectedModelSha=expectedIdentity["sourceSnapshotRef"].toObject()["modelSha256"].toString();
    identity &= sha256(data.inputSnapshotSha256)&&sha256(data.inputPackageSha256)&&sha256(expectedModelSha)
        && sha256(data.sourceSnapshotRef["modelSha256"].toString()) && expectedModelSha==data.sourceSnapshotRef["modelSha256"].toString();
    metric("identity",identity?"pass":"fail","Broker hashes and all expected job/source/artifact identities must match.");
    if(!identity) return finish();
    const QString kind=s["analysis"].toObject()["kind"].toString();
    const bool transient=kind==QLatin1String("linearTransient"), modal=kind==QLatin1String("modal");
    const bool beam=s["modelType"]==QLatin1String("cantileverBeam");
    if(data.sourceKind!=QLatin1String("native-passive-fe") || data.analysisType!=(transient?QString("structural-dynamics"):kind) || data.records.isEmpty()) {
        metric("modelSupport","fail","Only completed native passive Stage 1 static/modal/transient results are supported.");return finish();
    }
    if(!transient) {
        metric("sdofAnalyticDisplacement","notApplicable","Exact SDOF transient reference applies only to an undamped one-element axial rod.");
        metric("frozenForcedOscillator","notApplicable","OSC-FORCED-01 qualifies the scalar integration kernel, not a native FE result artifact.");
        metric("rodHistoryReference","notApplicable","APP-ROD-HISTORY-01 applies only to its declared one-element axial-rod transient profile.");
        metric("timeGrid","notApplicable","Terminal static/modal result.");
        metric("dynamicEquilibrium","notApplicable","No transient balance to check.");
        metric("referenceConvergence","notApplicable","Closed form continuum reference.");
    }
    if(modal) metric("displacement","notApplicable","Mode shape amplitude is arbitrary.");
    else metric("frequency","notApplicable","No selected modal frequency.");
    if(modal||transient) {
        metric("staticResidual","notApplicable","Not a static equilibrium result.");
        metric("strainEnergy","notApplicable","Static strain-energy/external-work identity does not apply.");
        metric("reactionBalance","notApplicable","Static support reaction and global force/moment balance does not apply.");
        metric("supportReactions","notApplicable","Signed static support reactions require a static equilibrium result.");
    }
    if(!modal) {
        metric("modalResidual","notApplicable","No modal eigenpair.");
        metric("modeShape","notApplicable","No modal eigenvector.");
    }
    if(!transient) {
        metric("energyBalance","notApplicable","Covered by static strain-energy/work metric or modal eigenpair residual.");
        metric("kinematicConsistency","notApplicable","No transient velocity/acceleration integration.");
    }
    const auto analysis=s["analysis"].toObject();
    bool grid = transient ? data.records.size()==analysis["steps"].toInt() : data.records.size()==1;
    bool models=true, fields=true, supports=true;
    QJsonArray modelIssues;
    QMap<QString,int> modelIssuePositions;
    QVector<Vec> states, velocities, accelerations;
    const QString revision=expectedIdentity["sourceSnapshotRef"].toObject()["sourceRevision"].toString("verification");
    for(int index=0;index<data.records.size();++index) {
        const auto &record=data.records[index];
        if(transient) {
            const double expectedTime=double(index+1)*analysis["timeStep"].toDouble();
            grid &= record.recordNo==index+1 && record.originKind==QLatin1String("produced")
                && std::isfinite(record.timeS) && std::abs(record.timeS-expectedTime)<=64*std::numeric_limits<double>::epsilon()*std::max(std::abs(expectedTime),analysis["timeStep"].toDouble());
        }
        if(!record.model) { models=false; fields=false; continue; }
        const auto &model=*record.model;
        if(modal||transient) models &= model.getProblemSetup().getMassMatrixType()==RMassMatrixType::Consistent;
        const auto diagnosis=RTrustedMechanicsService::diagnose(problemSpec,&model,"verification",revision);
        for(const auto &issue:diagnosis["issues"].toArray()) if(issue.toObject()["severity"]==QLatin1String("error")) {
            models=false;
            const auto details=issue.toObject();
            const auto evidenceBytes=QJsonDocument(details["evidence"].toObject()).toJson(QJsonDocument::Compact);
            const auto evidenceSha256=QString::fromLatin1(QCryptographicHash::hash(evidenceBytes,QCryptographicHash::Sha256).toHex());
            const QString key=details["code"].toString()+QLatin1Char('|')+evidenceSha256;
            if(modelIssuePositions.contains(key)) {
                const int position=modelIssuePositions.value(key);
                auto summary=modelIssues.at(position).toObject();summary["occurrences"]=summary["occurrences"].toInt()+1;
                auto recordNos=summary["recordNos"].toArray();recordNos.append(record.recordNo);summary["recordNos"]=recordNos;
                modelIssues.replace(position,summary);
            } else {
                auto summary=details;summary.insert("firstRecordNo",record.recordNo);summary.insert("recordNos",QJsonArray{record.recordNo});summary.insert("occurrences",1);
                summary.insert("evidenceSha256",evidenceSha256);
                modelIssuePositions.insert(key,modelIssues.size());modelIssues.append(summary);
            }
        }
        if(beam && s["section"].toObject()["localY"].toArray()!=QJsonArray{0.0,1.0,0.0}) models=false;
        // Reference uses a straight, ordered, positive-x mesh and one standard clamp.
        const int segments=s["geometry"].toObject()["segments"].toInt();
        const double length=s["geometry"].toObject()["length"].toDouble();
        models &= model.getNNodes()==uint(segments+1);
        std::vector<unsigned> restrained(model.getNNodes(),0);
        const QStringList dofs{"ux","uy","uz","rx","ry","rz"};
        for(const auto &support:s["supports"].toArray()) {
            const auto entry=support.toObject();
            const auto binding=RTrustedMechanicsService::resolveSelector(model,entry["selector"].toObject(),revision,revision);
            models &= binding["valid"].toBool();
            for(const auto &node:binding["nodePositions"].toArray()) for(const auto &dof:entry["dofs"].toArray())
                restrained[size_t(node.toInt())] |= 1u << dofs.indexOf(dof.toString());
        }
        for(uint node=0;node<model.getNNodes();++node) models &= restrained[node] == (node==0?(beam?63u:7u):(beam?0u:6u));
        for(uint node=0;node<model.getNNodes();++node) {
            const auto &p=model.getNode(node);
            models &= std::isfinite(p.getX()) && p.getY()==0.0 && p.getZ()==0.0
                && std::abs(p.getX()-length*double(node)/segments)<=1e-12*std::max(1.0,length);
        }
        Vec u,v,a,rotation,rv,ra;
        bool good=field(model,R_VARIABLE_DISPLACEMENT,u);
        if(transient) good &= field(model,R_VARIABLE_VELOCITY,v) && field(model,R_VARIABLE_ACCELERATION,a);
        if(beam) {
            good &= field(model,R_VARIABLE_ROTATION,rotation);
            if(transient) good &= field(model,R_VARIABLE_ANGULAR_VELOCITY,rv) && field(model,R_VARIABLE_ANGULAR_ACCELERATION,ra);
        }
        fields &= good;
        if(!good) continue;
        Vec q(model.getNNodes()*(beam?6:1)), dq(q.size()), ddq(q.size());
        for(uint node=0;node<model.getNNodes();++node) {
            for(int axis=0;axis<(beam?3:1);++axis) {
                q[node*(beam?6:1)+axis]=u[node*3+axis];
                if(transient) {dq[node*(beam?6:1)+axis]=v[node*3+axis];ddq[node*(beam?6:1)+axis]=a[node*3+axis];}
                if(beam) {
                    q[node*6+3+axis]=rotation[node*3+axis];
                    if(transient) {dq[node*6+3+axis]=rv[node*3+axis];ddq[node*6+3+axis]=ra[node*3+axis];}
                }
            }
            if(!beam) for(int axis=1;axis<3;++axis) {
                supports &= u[node*3+axis]==0.0;
                if(transient) supports &= v[node*3+axis]==0.0 && a[node*3+axis]==0.0;
            }
        }
        for(int d=0;d<(beam?6:1);++d) { supports &= q[d]==0.0; if(transient) supports &= dq[d]==0.0 && ddq[d]==0.0; }
        states.append(q); velocities.append(dq); accelerations.append(ddq);
    }
    metric("modelSupport",models?"pass":"fail","Actual geometry, elements, material, section, loads, supports and analysis must match supported Stage 1 physics.",{{"issues",modelIssues}});
    metric("nodalFields",fields?"pass":"fail","Every accepted record requires finite, complete, SI nodal fields (including beam angular states).");
    metric("supportValues",fields&&supports?"pass":"fail","All constrained displacement, velocity and acceleration values must be exactly zero.");
    if(transient) metric("timeGrid",grid?"pass":"fail","Every prescribed produced record must exist at exactly step * dt; resumed/subsampled grids are not qualified.",{{"records",data.records.size()},{"expectedSteps",analysis["steps"]}});
    if(!models||!fields||!supports||!grid) return finish();
    auto selectedDisplacementMeasurements = [&]() -> QJsonArray {
        QJsonArray values;
        const QStringList components{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")};
        const auto &firstModel = *data.records.first().model;
        for (const QJsonValue &value : s.value(QStringLiteral("observables")).toArray()) {
            const QJsonObject observable = value.toObject();
            if (observable.value(QStringLiteral("quantity")).toString() != QLatin1String("displacement"))
                continue;
            const QString component = observable.value(QStringLiteral("component")).toString();
            const int axis = components.indexOf(component);
            if (axis < 0 || (!beam && axis != 0)) return {};
            const QJsonObject selector = observable.value(QStringLiteral("selector")).toObject();
            const QJsonObject resolved = RTrustedMechanicsService::resolveSelector(
                firstModel, selector, revision, revision);
            const QJsonArray nodes = resolved.value(QStringLiteral("nodePositions")).toArray();
            if (!resolved.value(QStringLiteral("valid")).toBool() || nodes.size() != 1
                || !nodes.first().isDouble()) return {};
            const int node = nodes.first().toInt(-1);
            if (node < 0 || node >= firstModel.getNNodes()) return {};
            const auto &point = firstModel.getNode(uint(node));
            QJsonArray coordinate{point.getX(), point.getY(), point.getZ()};
            QJsonArray sampleTimes;
            QJsonArray samples;
            const int sampleCount = transient ? states.size() : 1;
            if (sampleCount <= 0 || sampleCount != data.records.size()) return {};
            for (int sample = 0; sample < sampleCount; ++sample) {
                const int stateIndex = int(size_t(node) * size_t(beam ? 6 : 1) + size_t(axis));
                if (stateIndex < 0 || size_t(stateIndex) >= states[size_t(sample)].size()) return {};
                const double result = states[size_t(sample)][size_t(stateIndex)];
                if (!std::isfinite(result)) return {};
                samples.append(result);
                if (transient) sampleTimes.append(data.records.at(sample).timeS);
            }
            QJsonObject measurement{{QStringLiteral("selector"), selector},
                {QStringLiteral("component"), component}, {QStringLiteral("coordinateM"), coordinate},
                {QStringLiteral("values"), samples}};
            if (transient) measurement.insert(QStringLiteral("timesS"), sampleTimes);
            values.append(measurement);
        }
        return values;
    };
    int expectedDisplacementCount = 0;
    for (const QJsonValue &value : s.value(QStringLiteral("observables")).toArray())
        if (value.toObject().value(QStringLiteral("quantity")).toString() == QLatin1String("displacement"))
            ++expectedDisplacementCount;
    const double length=s["geometry"].toObject()["length"].toDouble();
    const auto material=s["material"].toObject(), section=s["section"].toObject();
    const double e=material["youngModulus"].toDouble(),rho=material["density"].toDouble(),area=section["area"].toDouble();
    if(size_t(data.records.first().model->getNNodes())*size_t(beam?6:1)>192) {
        metric(modal?"modalResidual":(transient?"displacement":"staticResidual"),"notComputed","Independent dense reference limit exceeded (192 total DOFs); no matrix allocated.");return finish();
    }
    const auto system=assemble(*data.records.first().model,s);
    for(const auto *matrixValue:{&system.k,&system.m,&system.c}) for(const auto &row:*matrixValue) if(!finite(row)) {
        metric("modelSupport","fail","Independent SI operators overflowed or contain nonfinite values.");return finish();
    }
    if(modal) {
        const auto &setup=data.records.first().model->getProblemSetup().getModalSetup();
        const uint selected=setup.getMode();
        if(selected>=uint(analysis["modes"].toInt()) || selected>=100) {metric("frequency","fail","Selected mode is outside the requested Stage 1 mode range.");return finish();}
        Vec frequencies;
        for(uint i=0;i<=selected;++i) {
            frequencies.push_back((2*double(i)+1)/(4*length)*std::sqrt(e/rho));
            if(beam) {
                // Stable root of cos(beta) + sech(beta) = 0.
                double beta=(double(i)+0.5)*pi;
                for(int it=0;it<30;++it) {
                    const double sech=1/std::cosh(beta);
                    beta-=(std::cos(beta)+sech)/(-std::sin(beta)-sech*std::tanh(beta));
                }
                for(const QString &inertia:{QString("iy"),QString("iz")}) frequencies.push_back(beta*beta/(2*pi*length*length)*std::sqrt(e*section[inertia].toDouble()/(rho*area)));
                const double g=e/(2*(1+material["poissonRatio"].toDouble()));
                frequencies.push_back((2*double(i)+1)/(4*length)*std::sqrt(g*section["torsionalConstant"].toDouble()/(rho*(section["iy"].toDouble()+section["iz"].toDouble()))));
            }
        }
        std::sort(frequencies.begin(),frequencies.end());
        const double reference=frequencies[selected],actual=setup.getFrequency();
        const double error=std::abs(actual-reference)/reference;
        const bool good=selected<uint(analysis["modes"].toInt())&&std::isfinite(actual)&&actual>0&&std::isfinite(error)&&error<=effectiveFrequency;
        metric("frequency",good?"pass":"fail","Selected mode compared with fixed-free continuum axial/Euler-Bernoulli/torsional spectrum in Hz.",{{"mode",int(selected)+1},{"actualHz",actual},{"referenceHz",reference},{"relativeError",error}});
        double norm=0,maximumResidual=0,maximumScale=0;
        const auto &q=states.first();const double omega2=4*pi*pi*actual*actual;
        for(int i:system.free) {
            norm=std::max(norm,std::abs(q[i]));double residual=0,scale=0;
            for(size_t j=0;j<q.size();++j) {const double elastic=system.k[i][j]*q[j],inertial=omega2*system.m[i][j]*q[j];residual+=elastic-inertial;scale+=std::abs(elastic)+std::abs(inertial);}
            maximumResidual=std::max(maximumResidual,std::abs(residual));maximumScale=std::max(maximumScale,scale);
        }
        const double eigenError=maximumScale==0?std::numeric_limits<double>::infinity():maximumResidual/maximumScale;
        metric("modeShape",norm>0&&std::isfinite(norm)?"pass":"fail","Selected eigenvector must be finite and nonzero on an unconstrained DOF.");
        metric("modalResidual",norm>0&&std::isfinite(eigenError)&&eigenError<=1e-6?"pass":"fail","Independent consistent-mass K*phi - omega^2*M*phi residual.",{{"normalizedResidual",std::isfinite(eigenError)?QJsonValue(eigenError):QJsonValue()}});
        if (includeConvergenceMeasurements) {
            convergenceMeasurements = {{QStringLiteral("analysisKind"), kind},
                {QStringLiteral("expectedDisplacementCount"), expectedDisplacementCount},
                {QStringLiteral("frequencyHz"), actual}, {QStringLiteral("mode"), int(selected) + 1},
                {QStringLiteral("displacements"), selectedDisplacementMeasurements()}};
        }
        return finish();
    }
    if(!transient) {
        // Exact point-load solution by superposition, at every physical node.
        Vec reference(states.first().size(),0.0);
        const auto &model=*data.records.first().model;
        for(const auto &value:s["loads"].toArray()) {
            const auto load=value.toObject();
            const auto resolved=RTrustedMechanicsService::resolveSelector(model,load["selector"].toObject(),revision,revision);
            const auto ids=resolved["nodePositions"].toArray();
            if(!resolved["valid"].toBool() || ids.size()!=1) {metric("displacement","notComputed","Point-load reference selector could not be resolved.");return finish();}
            const double b=model.getNode(uint(ids.first().toInt())).getX();
            const auto f=load["components"].toArray();
            for(uint node=0;node<model.getNNodes();++node) {
                const double x=model.getNode(node).getX(),z=std::min(x,b);
                reference[node*system.stride]+=f[0].toDouble()*z/(e*area);
                if(beam) for(int plane=0;plane<2;++plane) {
                    const double inertia=section[plane==0?"iz":"iy"].toDouble(),p=f[plane+1].toDouble();
                    const double u=p*(x<=b?x*x*(3*b-x):b*b*(3*x-b))/(6*e*inertia);
                    reference[node*6+plane+1]+=u;
                    reference[node*6+(plane==0?5:4)]+=(plane==0?1:-1)*p*z*(2*b-z)/(2*e*inertia);
                }
            }
        }
        double error=0;
        // Compare each translation separately so one dominant axis cannot hide another.
        for(int axis=0;axis<(beam?3:1);++axis) {
            Vec actual,expected;
            for(size_t node=0;node<system.x.size();++node) {actual.push_back(states.first()[node*system.stride+axis]);expected.push_back(reference[node*system.stride+axis]);}
            error=std::max(error,relativeError(actual,expected));
        }
        metric("displacement",std::isfinite(error)&&error<=effectiveDisplacement?"pass":"fail","Every nodal translation compared with SI closed-form axial/point-loaded cantilever solution.",{{"maximumRelativeError",std::isfinite(error)?QJsonValue(error):QJsonValue()},{"effectiveRelativeLimit",effectiveDisplacement}});
        const auto f=forceAt(system,s,0,revision);const auto &q=states.first();Vec reaction(q.size(),0.0);
        double maximumResidual=0,maximumScale=0,strain=0,work=0;
        for(size_t i=0;i<q.size();++i) {
            double elastic=0,scale=std::abs(f[i]);
            for(size_t j=0;j<q.size();++j) {elastic+=system.k[i][j]*q[j];scale+=std::abs(system.k[i][j]*q[j]);}
            strain+=0.5*q[i]*elastic;work+=0.5*q[i]*f[i];
            if(std::find(system.free.begin(),system.free.end(),int(i))==system.free.end()) reaction[i]=elastic-f[i];
            else {maximumResidual=std::max(maximumResidual,std::abs(elastic-f[i]));maximumScale=std::max(maximumScale,scale);}
        }
        const double residual=maximumScale==0?(maximumResidual==0?0:std::numeric_limits<double>::infinity()):maximumResidual/maximumScale;
        metric("staticResidual",std::isfinite(residual)&&residual<=1e-6?"pass":"fail","Independent K*u=f residual on every unconstrained DOF.",{{"normalizedResidual",std::isfinite(residual)?QJsonValue(residual):QJsonValue()}});
        QJsonArray supportReactionValues;
        bool finiteSupportReactions=true;
        const QStringList reactionComponents{QStringLiteral("ux"),QStringLiteral("uy"),QStringLiteral("uz"),
            QStringLiteral("rx"),QStringLiteral("ry"),QStringLiteral("rz")};
        for(size_t dof=0;dof<reaction.size();++dof) {
            if(std::find(system.free.begin(),system.free.end(),int(dof))!=system.free.end()) continue;
            const double value=reaction[dof];
            finiteSupportReactions &= std::isfinite(value);
            const int component=int(dof%size_t(system.stride));
            supportReactionValues.append(QJsonObject{
                {QStringLiteral("nodePosition"),int(dof/size_t(system.stride))},
                {QStringLiteral("component"),reactionComponents.at(component)},
                {QStringLiteral("value"),std::isfinite(value)?QJsonValue(value):QJsonValue()},
                {QStringLiteral("unit"),component<3?QStringLiteral("N"):QStringLiteral("N*m")}});
        }
        metric("supportReactions",finiteSupportReactions?"pass":"fail",
            "Signed constrained-DOF reactions recovered from the independent SI K*u-f residual; this reports values and does not itself compare them with a separate reference.",
            {{"basis",QStringLiteral("independent SI stiffness assembly; K*u-f")},
             {"values",supportReactionValues}});
        Vec totals(6,0),scales(6,0);
        for(size_t node=0;node<system.x.size();++node) {
            for(int axis=0;axis<(beam?3:1);++axis) {
                const size_t id=node*system.stride+axis;const double applied=f[id]+reaction[id];
                totals[axis]+=applied;scales[axis]+=std::abs(f[id])+std::abs(reaction[id]);
                if(axis==1) {totals[5]+=system.x[node]*applied;scales[5]+=std::abs(system.x[node]*f[id])+std::abs(system.x[node]*reaction[id]);}
                if(axis==2) {totals[4]-=system.x[node]*applied;scales[4]+=std::abs(system.x[node]*f[id])+std::abs(system.x[node]*reaction[id]);}
            }
            if(beam) for(int axis=3;axis<6;++axis) {const size_t id=node*6+axis;totals[axis]+=f[id]+reaction[id];scales[axis]+=std::abs(f[id])+std::abs(reaction[id]);}
        }
        double imbalance=0;
        for(int axis=0;axis<6;++axis) imbalance=std::max(imbalance,scales[axis]==0?(totals[axis]==0?0:std::numeric_limits<double>::infinity()):std::abs(totals[axis])/scales[axis]);
        metric("reactionBalance",std::isfinite(imbalance)&&imbalance<=1e-6?"pass":"fail","Support reactions in the supportReactions metric and applied loads must balance in total force and moment about the clamp.",{{"maximumNormalizedImbalance",std::isfinite(imbalance)?QJsonValue(imbalance):QJsonValue()},{"reactionBasis","derived from persisted displacement; no native reaction-field claim"}});
        const double energyError=std::max(std::abs(strain),std::abs(work))==0?0:std::abs(strain-work)/std::max(std::abs(strain),std::abs(work));
        metric("strainEnergy",std::isfinite(strain)&&std::isfinite(work)&&strain>=0&&std::isfinite(energyError)&&energyError<=1e-6?"pass":"fail","Linear elastic strain energy 0.5*u^T*K*u equals proportional-load external work 0.5*u^T*f.",{{"strainEnergyJ",strain},{"externalWorkJ",work},{"relativeError",energyError}});
        if (includeConvergenceMeasurements) {
            convergenceMeasurements = {{QStringLiteral("analysisKind"), kind},
                {QStringLiteral("expectedDisplacementCount"), expectedDisplacementCount},
                {QStringLiteral("displacements"), selectedDisplacementMeasurements()},
                {QStringLiteral("strainEnergyJ"), strain}};
        }
        return finish();
    }
    // A bounded independent dense reference. Exceeding its bound is explicit,
    // never a partial-record pass. The production solver is never called.
    if(system.free.size()>192 || data.records.size()>10000) {
        metric("displacement","notComputed","Independent dense transient reference limit exceeded (192 free DOFs / 10000 steps).");return finish();
    }
    const auto k=reduced(system.k,system.free),m=reduced(system.m,system.free),c=reduced(system.c,system.free);
    const size_t n=system.free.size(); const double dt=analysis["timeStep"].toDouble();
    constexpr double referenceWorkBudget=2e8;
    const double estimatedWork=double(n)*double(n)*double(n)*double(data.records.size())*48.0;
    if(estimatedWork>referenceWorkBudget) {
        metric("referenceConvergence","notComputed","Independent transient reference operation budget exceeded before integration.",{{"estimatedScalarWork",estimatedWork},{"scalarWorkBudget",referenceWorkBudget}});return finish();
    }
    Vec initialQ(system.k.size(),0),initialV(system.k.size(),0);
    const auto &model=*data.records.first().model;
    for(const auto &value:s["initialConditions"].toArray()) {
        const auto ic=value.toObject();
        const auto resolved=RTrustedMechanicsService::resolveSelector(model,ic["selector"].toObject(),revision,revision);
        if(!resolved["valid"].toBool()) {metric("displacement","notComputed","Initial-condition selector could not be resolved.");return finish();}
        const QString type=ic["type"].toString();
        Vec &target=(type==QLatin1String("velocity")||type==QLatin1String("angularVelocity"))?initialV:initialQ;
        const int offset=(type==QLatin1String("rotation")||type==QLatin1String("angularVelocity"))?3:0;
        for(const auto &node:resolved["nodePositions"].toArray()) for(int axis=0;axis<(beam?3:1);++axis)
            target[size_t(node.toInt()*system.stride+offset+axis)]=ic["components"].toArray()[axis].toDouble();
    }
    const double axialMass=rho*area*length/3.0,axialStiffness=e*area/length;
    bool appRodHistory=!beam && system.x.size()==2 && n==1 && axialMass==1.0 && axialStiffness==4.0
        && analysis["rayleighAlpha"].toDouble()==0.0 && analysis["rayleighBeta"].toDouble()==0.0
        && initialQ[1]==0.0 && initialV[1]==0.0 && dt==0.02 && data.records.size()==100
        && analysis["steps"].toInt()==100;
    const auto loads=s["loads"].toArray();appRodHistory &= loads.size()==1;
    if(loads.size()==1) {
        const auto load=loads.first().toObject(),binding=RTrustedMechanicsService::resolveSelector(model,load["selector"].toObject(),revision,revision);
        const auto nodes=binding["nodePositions"].toArray(),history=load["history"].toArray(),components=load["components"].toArray();
        appRodHistory &= binding["valid"].toBool()&&nodes.size()==1&&nodes.first().toInt()==1&&history.size()==101
            && components==QJsonArray{0.0,0.0,0.0};
        for(int sample=0;sample<history.size();++sample) {
            const auto point=history[sample].toObject();const double time=sample*0.02;const auto force=point["components"].toArray();
            appRodHistory &= std::abs(point["time"].toDouble()-time)<=64*std::numeric_limits<double>::epsilon()*std::max(0.02,time)
                && force.size()==3 && std::abs(force[0].toDouble()-std::sin(time))<=1e-14 && force[1].toDouble()==0.0&&force[2].toDouble()==0.0;
        }
    }
    metric("frozenForcedOscillator","notApplicable","OSC-FORCED-01 is evaluated by its independent scalar integration-kernel fixture; FE load-history qualification is reported separately.");
    if(appRodHistory) {
        const auto history=loads.first().toObject()["history"].toArray();
        const double omega=std::sqrt(axialStiffness/axialMass),omega2=omega*omega;
        const double cosine=std::cos(omega*dt),sine=std::sin(omega*dt);
        double q=0.0,v=0.0,maximumError=0.0;
        bool exactFinite=std::isfinite(omega)&&omega>0.0;
        for(int step=0;step<data.records.size();++step) {
            const auto left=history[step].toObject(),right=history[step+1].toObject();
            const double f0=left["components"].toArray()[0].toDouble();
            const double f1=right["components"].toArray()[0].toDouble();
            const double slope=(f1-f0)/dt;
            const double nextQ=q*cosine+v*sine/omega+f0*(1.0-cosine)/omega2
                +slope*(dt-sine/omega)/omega2;
            const double nextV=-q*omega*sine+v*cosine+f0*sine/omega
                +slope*(1.0-cosine)/omega2;
            exactFinite &= std::isfinite(nextQ)&&std::isfinite(nextV)&&std::isfinite(states[step][1]);
            maximumError=std::max(maximumError,std::abs(states[step][1]-nextQ));
            q=nextQ;v=nextV;
        }
        metric("rodHistoryReference",exactFinite&&maximumError<=1e-4?"pass":"fail",
            "APP-ROD-HISTORY-01: independent exact SDOF propagation for the declared piecewise-linear point-force history; production Newmark integration is not used by the reference.",
            {{"maximumAbsoluteErrorM",maximumError},{"fixedAbsoluteLimitM",1e-4},{"records",data.records.size()},
             {"massKg",axialMass},{"stiffnessNPerM",axialStiffness},{"referenceKind","exact interval propagation for piecewise-linear force"}});
    } else metric("rodHistoryReference","notApplicable","Requires the exact APP-ROD-HISTORY-01 one-element m=1 kg, k=4 N/m, undamped zero-initial-state rod and 101 sine samples on dt=0.02 s through 2.0 s.");
    bool sdof=!beam && system.x.size()==2 && n==1 && analysis["rayleighAlpha"].toDouble()==0.0 && analysis["rayleighBeta"].toDouble()==0.0
        && initialV[1]==0.0;
    double constantForce=0;
    for(const auto &value:s["loads"].toArray()) {
        const auto load=value.toObject();const auto binding=RTrustedMechanicsService::resolveSelector(model,load["selector"].toObject(),revision,revision);
        const auto nodes=binding["nodePositions"].toArray();
        sdof &= binding["valid"].toBool()&&nodes.size()==1&&nodes.first().toInt()==1;
        const auto components=load["components"].toArray();
        sdof &= components[1].toDouble()==0.0 && components[2].toDouble()==0.0;
        constantForce+=components[0].toDouble();
        for(const auto &sample:load["history"].toArray()) sdof &= sample.toObject()["components"].toArray()==components;
    }
    // Frozen exact oracles cover a step from rest and free vibration from a
    // prescribed displacement. Other transient forms retain generic FE checks.
    sdof &= (initialQ[1]==0.0 && constantForce!=0.0) || (initialQ[1]!=0.0 && constantForce==0.0);
    if(sdof) {
        const double stiffness=e*area/length,mass=rho*area*length/3.0,omega=std::sqrt(stiffness/mass),equilibrium=constantForce/stiffness;
        const double scale=std::max(std::abs(initialQ[1]),std::abs(equilibrium));double maximumError=0;
        bool exactFinite=std::isfinite(stiffness)&&stiffness>0&&std::isfinite(mass)&&mass>0&&std::isfinite(omega)&&std::isfinite(equilibrium);
        for(int step=0;step<data.records.size();++step) {
            const double phase=omega*data.records[step].timeS;
            const double reference=initialQ[1]*std::cos(phase)+equilibrium*2*std::pow(std::sin(phase/2),2);
            exactFinite &= std::isfinite(phase)&&std::isfinite(reference);
            maximumError=std::max(maximumError,std::abs(states[step][1]-reference));
        }
        const double scaledError=scale==0?(maximumError==0?0:std::numeric_limits<double>::infinity()):maximumError/scale;
        metric("sdofAnalyticDisplacement",exactFinite&&std::isfinite(scaledError)&&scaledError<=1e-4?"pass":"fail","Every accepted SDOF displacement compared with the exact undamped consistent-mass rod oscillator; scale=max(|initial displacement|,|F/k|).",
            {{"scaledMaximumAbsoluteError",std::isfinite(scaledError)?QJsonValue(scaledError):QJsonValue()},{"fixedLimit",1e-4},{"massKg",mass},{"stiffnessNPerM",stiffness},{"omegaRadPerS",omega}});
    } else metric("sdofAnalyticDisplacement","notApplicable","Requires one fixed-free axial element, zero damping/initial velocity/transverse force, and either a nonzero constant tip step force from rest or nonzero zero-load initial displacement; zero excitation is excluded.");
    auto integrate=[&](int subdivisions,QVector<Vec> &trajectory)->bool {
        Vec q=reduced(initialQ,system.free),v=reduced(initialV,system.free),a;
        Vec rhs=reduced(forceAt(system,s,0,revision),system.free);
        for(size_t i=0;i<n;++i) for(size_t j=0;j<n;++j) rhs[i]-=k[i][j]*q[j]+c[i][j]*v[j];
        if(!solve(m,rhs,a)) return false;
        const double h=dt/subdivisions;
        Mat effective=matrix(n);
        for(size_t i=0;i<n;++i) for(size_t j=0;j<n;++j) effective[i][j]=m[i][j]+h/2*c[i][j]+h*h/4*k[i][j];
        for(int step=0;step<data.records.size();++step) {
            for(int sub=1;sub<=subdivisions;++sub) {
                Vec qp(n),vp(n),nextA;
                for(size_t i=0;i<n;++i) {qp[i]=q[i]+h*v[i]+h*h/4*a[i];vp[i]=v[i]+h/2*a[i];}
                rhs=reduced(forceAt(system,s,(step+double(sub)/subdivisions)*dt,revision),system.free);
                for(size_t i=0;i<n;++i) for(size_t j=0;j<n;++j) rhs[i]-=k[i][j]*qp[j]+c[i][j]*vp[j];
                if(!solve(effective,rhs,nextA)) return false;
                for(size_t i=0;i<n;++i) {q[i]=qp[i]+h*h/4*nextA[i];v[i]=vp[i]+h/2*nextA[i];} a=nextA;
            }
            trajectory.append(q);
        }
        return true;
    };
    QVector<Vec> coarse,fine;
    if(!integrate(16,coarse)||!integrate(32,fine)) {metric("displacement","notComputed","Independent transient reference could not be solved finitely.");return finish();}
    double convergence=0,error=0,balance=0;
    // Global trajectory norm per DOF; includes every record and avoids singular
    // instantaneous relative errors at a response zero crossing.
    for(size_t d=0;d<n;++d) {
        Vec actual,ref,other;
        for(int step=0;step<data.records.size();++step) {actual.push_back(states[step][system.free[d]]);ref.push_back(fine[step][d]);other.push_back(coarse[step][d]);}
        error=std::max(error,relativeError(actual,ref));convergence=std::max(convergence,relativeError(other,ref));
    }
    for(int step=0;step<data.records.size();++step) {
        const auto f=forceAt(system,s,data.records[step].timeS,revision);
        for(int i:system.free) {
            double residual=-f[i],scale=std::abs(f[i]);
            for(size_t j=0;j<system.k.size();++j) {
                const double inertial=system.m[i][j]*accelerations[step][j],damping=system.c[i][j]*velocities[step][j],elastic=system.k[i][j]*states[step][j];
                residual+=inertial+damping+elastic;scale+=std::abs(inertial)+std::abs(damping)+std::abs(elastic);
            }
            balance=std::max(balance,scale==0?(residual==0?0:std::numeric_limits<double>::infinity()):std::abs(residual)/scale);
        }
    }
    Vec previousQ=initialQ,previousV=initialV,previousA(system.k.size(),0),initialRhs=reduced(forceAt(system,s,0,revision),system.free),freeA;
    for(size_t i=0;i<n;++i) for(size_t j=0;j<system.k.size();++j) initialRhs[i]-=system.k[system.free[i]][j]*initialQ[j]+system.c[system.free[i]][j]*initialV[j];
    if(!solve(m,initialRhs,freeA)) {metric("kinematicConsistency","notComputed","Initial independent acceleration solve failed.");return finish();}
    for(size_t i=0;i<n;++i) previousA[system.free[i]]=freeA[i];
    auto energy=[&](const Vec &q,const Vec &v) {double total=0;for(size_t i=0;i<q.size();++i) for(size_t j=0;j<q.size();++j) total+=0.5*(q[i]*system.k[i][j]*q[j]+v[i]*system.m[i][j]*v[j]);return total;};
    const double initialEnergy=energy(initialQ,initialV);double externalWork=0,dissipation=0,energyError=0,kinematic=0;
    Vec previousForce=forceAt(system,s,0,revision);
    for(int step=0;step<data.records.size();++step) {
        const auto &q=states[step],&v=velocities[step],&a=accelerations[step];const auto f=forceAt(system,s,data.records[step].timeS,revision);
        for(int i:system.free) {
            const double dq=q[i]-previousQ[i],dv=v[i]-previousV[i];
            const double expectedQ=dt/2*(v[i]+previousV[i]),expectedV=dt/2*(a[i]+previousA[i]);
            for(const auto &pair:{std::make_pair(dq,expectedQ),std::make_pair(dv,expectedV)}) {
                const double scale=std::max(std::abs(pair.first),std::abs(pair.second));
                kinematic=std::max(kinematic,scale==0?0:std::abs(pair.first-pair.second)/scale);
            }
            externalWork+=0.5*(previousForce[i]+f[i])*dq;
            for(int j:system.free) dissipation+=dt*0.25*(v[i]+previousV[i])*system.c[i][j]*(v[j]+previousV[j]);
        }
        const double currentEnergy=energy(q,v),scale=std::abs(initialEnergy)+std::abs(currentEnergy)+std::abs(externalWork)+std::abs(dissipation);
        const double residual=currentEnergy-initialEnergy-externalWork+dissipation;
        energyError=std::max(energyError,scale==0?(residual==0?0:std::numeric_limits<double>::infinity()):std::abs(residual)/scale);
        previousQ=q;previousV=v;previousA=a;previousForce=f;
    }
    metric("kinematicConsistency",std::isfinite(kinematic)&&kinematic<=1e-6?"pass":"fail","Every consecutive accepted state must satisfy average-acceleration Newmark displacement and velocity identities, starting from the declared initial state.",{{"maximumRelativeResidual",std::isfinite(kinematic)?QJsonValue(kinematic):QJsonValue()}});
    metric("energyBalance",std::isfinite(energyError)&&energyError<=1e-6?"pass":"fail","At every accepted record, kinetic plus strain energy equals initial energy plus midpoint external work minus Rayleigh dissipation (the supported Newmark method).",{{"maximumNormalizedResidual",std::isfinite(energyError)?QJsonValue(energyError):QJsonValue()}});
    metric("referenceConvergence",std::isfinite(convergence)&&convergence<=0.001?"pass":"fail","Independent average-acceleration reference at dt/16 and dt/32 must agree within 0.1% on every trajectory DOF.",{{"relativeDifference",std::isfinite(convergence)?QJsonValue(convergence):QJsonValue()}});
    metric("displacement",std::isfinite(error)&&error<=effectiveDisplacement?"pass":"fail","All accepted full nodal trajectories compared with independent consistent-mass 1D SI reference; requested precision is capped at the fixed 1% bound.",{{"maximumRelativeError",std::isfinite(error)?QJsonValue(error):QJsonValue()},{"effectiveRelativeLimit",effectiveDisplacement},{"reference","axial / Euler-Bernoulli semi-discrete mesh; Newmark dt/32"}});
    metric("dynamicEquilibrium",std::isfinite(balance)&&balance<=1e-6?"pass":"fail","Every accepted record must satisfy M*a+C*v+K*u=f at every unconstrained DOF.",{{"maximumNormalizedResidual",std::isfinite(balance)?QJsonValue(balance):QJsonValue()}});
    if (includeConvergenceMeasurements) {
        convergenceMeasurements = {{QStringLiteral("analysisKind"), kind},
            {QStringLiteral("expectedDisplacementCount"), expectedDisplacementCount},
            {QStringLiteral("displacements"), selectedDisplacementMeasurements()}};
    }
    return finish();
}
