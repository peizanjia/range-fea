#include <QtTest>
#include <rmesh_candidate_service.h>
#include <rgeometry_candidate_service.h>
#include <atomic>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <cmath>
#include <limits>
#include <set>

namespace
{
using XYZ=std::array<double,3>;
QJsonObject options(double volume=0.08)
{
    return {{"unit","m"},{"volumeUnit","m^3"},{"maxVolume",volume},{"radiusEdgeRatio",1.6}};
}
QString objectHash(const QJsonObject &o)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
RModel surface(const std::vector<XYZ> &nodes,const std::vector<std::array<uint,3>> &faces,uint groups=1)
{
    RModel m;
    m.setNNodes(uint(nodes.size())); m.setNSurfaces(groups);
    RMaterial material=RMaterial::generateDefault(); material.setName("WF14 uniform solid");
    for (uint g=0;g<groups;++g)
    {
        m.getSurface(g).setName(QString("outer-%1").arg(g));
        m.getSurface(g).setMaterial(material);
    }
    for (uint i=0;i<nodes.size();++i)
    {
        m.getNode(i).set(nodes[i][0],nodes[i][1],nodes[i][2]);
        m.setNodePhysicalId(i,101+10*i);
    }
    for (uint i=0;i<faces.size();++i)
    {
        RElement e(R_ELEMENT_TRI1);
        for (uint j=0;j<3;++j) e.setNodeId(j,faces[i][j]);
        m.addElement(e,true,groups==1 ? 0 : i/2);
        m.setElementPhysicalId(i,501+10*i);
    }
    return m;
}
RModel tetra()
{
    return surface({{0,0,0},{1,0,0},{0,1,0},{0,0,1}},
                   {{0,2,1},{0,1,3},{0,3,2},{1,2,3}});
}
RModel cube()
{
    // Independently stated outward faces of the unit cube, two facets per named side.
    return surface({{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
                   {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                    {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}},6);
}
double determinant(const RModel &m,const RElement &e)
{
    XYZ a{},b{},c{};
    const RNode &o=m.getNode(e.getNodeId(0)), &x=m.getNode(e.getNodeId(1)),
                &y=m.getNode(e.getNodeId(2)), &z=m.getNode(e.getNodeId(3));
    a={x.getX()-o.getX(),x.getY()-o.getY(),x.getZ()-o.getZ()};
    b={y.getX()-o.getX(),y.getY()-o.getY(),y.getZ()-o.getZ()};
    c={z.getX()-o.getX(),z.getY()-o.getY(),z.getZ()-o.getZ()};
    return a[0]*(b[1]*c[2]-b[2]*c[1])-a[1]*(b[0]*c[2]-b[2]*c[0])+a[2]*(b[0]*c[1]-b[1]*c[0]);
}
void near(double actual,double expected,double tolerance=1e-8)
{
    QVERIFY2(std::abs(actual-expected)<=tolerance,qPrintable(QString("%1 != %2").arg(actual,0,'g',17).arg(expected,0,'g',17)));
}
void identitiesAndSource(const RModel &source,const RMeshCandidateService::MeshCandidate &r,const QString &pre)
{
    QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre);
    QCOMPARE(r.sourceGeometrySha256,pre);
    QVERIFY(r.postGeometrySha256!=pre);
    QCOMPARE(r.postGeometrySha256,RGeometryCandidateService::geometryFingerprint(r.candidate));
    std::set<uint> sourceNodes,sourceElements,newNodes,newElements;
    for (uint i=0;i<source.getNNodes();++i) sourceNodes.insert(source.getNodePhysicalId(i));
    for (uint i=0;i<source.getNElements();++i) sourceElements.insert(source.getElementPhysicalId(i));
    for (uint i=0;i<r.candidate.getNNodes();++i)
    {
        const uint id=r.candidate.getNodePhysicalId(i);
        QVERIFY(id>0 && id<std::numeric_limits<uint>::max());
        QVERIFY(!sourceNodes.contains(id)); QVERIFY(newNodes.insert(id).second);
    }
    for (uint i=0;i<r.candidate.getNElements();++i)
    {
        const uint id=r.candidate.getElementPhysicalId(i);
        QVERIFY(!sourceElements.contains(id)); QVERIFY(newElements.insert(id).second);
    }
    QCOMPARE(r.candidate.getNSurfaces(),source.getNSurfaces());
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        QCOMPARE(r.candidate.getSurface(g).getName(),source.getSurface(g).getName());
        QCOMPARE(r.candidate.getSurface(g).getPhysicalId(),source.getSurface(g).getPhysicalId());
        QCOMPARE(r.candidate.getSurface(g).getMaterial().toJson(),source.getSurface(g).getMaterial().toJson());
    }
    QCOMPARE(r.candidate.getNVolumes(),1u);
    QCOMPARE(r.candidate.getVolume(0).getName(),QString("WF14 single region"));
    QCOMPARE(r.candidate.getVolume(0).getMaterial().toJson(),source.getSurface(0).getMaterial().toJson());
}
}

class MeshCandidateTest : public QObject
{
    Q_OBJECT
private slots:
    void strictOptionsAndPurePreview()
    {
        const RModel source=tetra(); const QString pre=RGeometryCandidateService::geometryFingerprint(source);
        const auto p=RMeshCandidateService::preview(source,options());
        QVERIFY2(p.valid,qPrintable(p.message)); QVERIFY(p.candidate.isEmpty()); QVERIFY(p.postGeometrySha256.isEmpty());
        near(p.report.value("sourceVolume").toDouble(),1.0/6);
        QCOMPARE(p.report.value("sourceCentroid").toArray(),QJsonArray({0.25,0.25,0.25}));
        QCOMPARE(p.report.value("bindingState").toString(),QString("REQUIRES_REBINDING"));
        QCOMPARE(p.report.value("requestedMaxVolume").toDouble(),0.08);
        QCOMPARE(p.report.value("backendMaxVolume").toDouble(),0.04);
        QCOMPARE(p.report.value("backendVolumeSafetyFactor").toDouble(),0.5);
        QCOMPARE(p.report.value("estimatedTetrahedra").toDouble(),25.0);
        QCOMPARE(p.report.value("normalizedArgumentsSha256").toString(),objectHash(p.normalizedArguments));
        for (const QString &field:{QString("tetGenInputParams"),QString("algorithm"),QString("callback"),QString("sizeFunction")})
        {
            QJsonObject a=options(); a.insert(field,"arbitrary");
            QCOMPARE(RMeshCandidateService::preview(source,a).code,QString("invalid_arguments"));
        }
        for (const QJsonObject &a:{QJsonObject{{"unit","m"}},QJsonObject{{"unit","mm"},{"volumeUnit","m^3"},{"maxVolume",0.1},{"radiusEdgeRatio",1.6}},
            options(0),options(-1),options(1e19),options(1e-16)})
            QVERIFY(!RMeshCandidateService::preview(source,a).valid);
        for (const QJsonValue &ratio:{QJsonValue("1.6"),QJsonValue(1.19),QJsonValue(2.01),QJsonValue()})
        {
            QJsonObject a=options(); a.insert("radiusEdgeRatio",ratio); QVERIFY(!RMeshCandidateService::preview(source,a).valid);
        }
        QJsonObject precise=options(0.123456789);
        QCOMPARE(RMeshCandidateService::preview(source,precise).normalizedArguments.value("maxVolume").toDouble(),0.123457);
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre);
    }
    void analyticTetrahedronAndFreshIdentity()
    {
        const RModel source=tetra(); const QString pre=RGeometryCandidateService::geometryFingerprint(source);
        const auto r=RMeshCandidateService::generate(source,options(0.04));
        QVERIFY2(r.valid,qPrintable(r.code+": "+r.message));
        identitiesAndSource(source,r,pre); near(r.report.value("totalVolume").toDouble(),1.0/6);
        const QJsonArray center=r.report.value("centroid").toArray(); for (int i=0;i<3;++i) near(center[i].toDouble(),0.25);
        double volume=0;
        for (uint i=0;i<r.candidate.getNElements();++i)
        {
            const RElement &e=r.candidate.getElement(i);
            QVERIFY(e.getType()==R_ELEMENT_TRI1 || e.getType()==R_ELEMENT_TETRA1);
            if (e.getType()!=R_ELEMENT_TETRA1) continue;
            const double d=determinant(r.candidate,e); QVERIFY(std::isfinite(d) && d>0);
            QVERIFY(d/6<=0.04*(1+1e-8)); volume+=d/6;
        }
        near(volume,1.0/6); QVERIFY(r.report.value("boundaryFacetCoverageVerified").toBool());
        QVERIFY(r.report.value("qualityVolumeSha256").toString().size()==64);
        // Stable allocator must advance beyond every new ID; old resize slots cannot reappear.
        RModel next(r.candidate); const uint n=next.getNNodes(); next.setNNodes(n+1);
        uint maximum=0; for (uint i=0;i<n;++i) maximum=std::max(maximum,next.getNodePhysicalId(i));
        QVERIFY(next.getNodePhysicalId(n)>maximum);
        const uint e=next.getNElements(); next.setNElements(e+1);
        maximum=0; for (uint i=0;i<e;++i) maximum=std::max(maximum,next.getElementPhysicalId(i));
        QVERIFY(next.getElementPhysicalId(e)>maximum);
    }
    void analyticCubeMarkersAndVolumeConstraint()
    {
        const RModel source=cube(); const QString pre=RGeometryCandidateService::geometryFingerprint(source);
        const auto r=RMeshCandidateService::generate(source,options(0.05));
        if (!r.valid) qWarning().noquote()<<QJsonDocument(r.report).toJson(QJsonDocument::Compact);
        QVERIFY2(r.valid,qPrintable(r.code+": "+r.message)); identitiesAndSource(source,r,pre);
        QCOMPARE(r.normalizedArguments,options(0.05));
        QCOMPARE(r.report.value("requestedMaxVolume").toDouble(),0.05);
        QCOMPARE(r.report.value("backendMaxVolume").toDouble(),0.025);
        QCOMPARE(r.report.value("backendVolumeSafetyFactor").toDouble(),0.5);
        QCOMPARE(r.report.value("estimatedTetrahedra").toDouble(),40.0);
        QCOMPARE(r.report.value("backendParameters").toString(),QString("npAq1.6a0.025T1e-10"));
        QCOMPARE(r.report.value("normalizedArgumentsSha256").toString(),objectHash(options(0.05)));
        QCOMPARE(r.report.value("backendParametersSha256").toString(),objectHash(QJsonObject{{"parameters",r.report.value("backendParameters")}}));
        QJsonObject reportForHash=r.report; reportForHash.remove("reportSha256");
        QCOMPARE(r.report.value("reportSha256").toString(),objectHash(reportForHash));
        near(r.report.value("totalVolume").toDouble(),1);
        const QJsonArray center=r.report.value("centroid").toArray(); for (int i=0;i<3;++i) near(center[i].toDouble(),0.5);
        QCOMPARE(r.report.value("surfaceLabels").toArray().size(),6);
        for (uint g=0;g<6;++g)
        {
            double area=0;
            const RSurface &s=r.candidate.getSurface(g);
            for (uint j=0;j<s.size();++j)
            {
                const RElement &e=r.candidate.getElement(s.get(j));
                const RNode &a=r.candidate.getNode(e.getNodeId(0)), &b=r.candidate.getNode(e.getNodeId(1)), &c=r.candidate.getNode(e.getNodeId(2));
                const XYZ u{b.getX()-a.getX(),b.getY()-a.getY(),b.getZ()-a.getZ()},v{c.getX()-a.getX(),c.getY()-a.getY(),c.getZ()-a.getZ()};
                area+=std::hypot(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])/2;
                const int axis=g<2 ? 2 : (g==2 || g==4 ? 1 : 0);
                const double value=(g==1 || g==3 || g==4) ? 1 : 0;
                for (uint k=0;k<3;++k)
                {
                    const RNode &n=r.candidate.getNode(e.getNodeId(k)); const XYZ p{n.getX(),n.getY(),n.getZ()}; near(p[axis],value);
                }
            }
            near(area,1);
        }
        QVERIFY(r.report.value("maximumTetVolume").toDouble()<=0.05*(1+1e-8));
    }
    void outwardBoundaryOrientationAndPureArtifactRejection()
    {
        for (const RModel &source:{tetra(),cube()})
        {
            const auto generated=RMeshCandidateService::generate(source,options()); QVERIFY2(generated.valid,qPrintable(generated.message));
            QVERIFY(generated.report.value("outwardBoundaryOrientationVerified").toBool());
            QVERIFY(generated.report.value("oppositeInteriorFaceOrientationVerified").toBool());
            QCOMPARE(generated.report.value("boundaryOrientationNormalizationAllowedDuringVerification"),QJsonValue(false));
            const auto verified=RMeshCandidateService::verify(source,generated.candidate,options()); QVERIFY2(verified.valid,qPrintable(verified.message));
            QCOMPARE(verified.report,generated.report);
            // Independent geometric normal test using the adjacent tetrahedron's
            // centroid, rather than the verifier's combinatorial orientation rule.
            for (uint i=0;i<generated.candidate.getNElements();++i)
            {
                const auto &triangle=generated.candidate.getElement(i);
                if (triangle.getType()!=R_ELEMENT_TRI1) continue;
                const auto &a=generated.candidate.getNode(triangle.getNodeId(0));
                const auto &b=generated.candidate.getNode(triangle.getNodeId(1));
                const auto &c=generated.candidate.getNode(triangle.getNodeId(2));
                const XYZ u{b.getX()-a.getX(),b.getY()-a.getY(),b.getZ()-a.getZ()};
                const XYZ v{c.getX()-a.getX(),c.getY()-a.getY(),c.getZ()-a.getZ()};
                const XYZ normal{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
                uint adjacent=0;
                for (uint j=0;j<generated.candidate.getNElements();++j)
                {
                    const auto &tet=generated.candidate.getElement(j);
                    if (tet.getType()!=R_ELEMENT_TETRA1) continue;
                    uint shared=0;
                    for (uint k=0;k<3;++k) for (uint l=0;l<4;++l) if (triangle.getNodeId(k)==tet.getNodeId(l)) ++shared;
                    if (shared!=3) continue;
                    ++adjacent; XYZ center{};
                    for (uint k=0;k<4;++k)
                    {
                        const auto &node=generated.candidate.getNode(tet.getNodeId(k));
                        center[0]+=node.getX()/4; center[1]+=node.getY()/4; center[2]+=node.getZ()/4;
                    }
                    QVERIFY(normal[0]*(center[0]-a.getX())+normal[1]*(center[1]-a.getY())+normal[2]*(center[2]-a.getZ())<0);
                }
                QCOMPARE(adjacent,1u);
            }
            RModel reversed=generated.candidate;
            auto &triangle=reversed.getElement(reversed.getSurface(0).get(0));
            const uint node=triangle.getNodeId(1); triangle.setNodeId(1,triangle.getNodeId(2)); triangle.setNodeId(2,node);
            const QString before=RGeometryCandidateService::geometryFingerprint(reversed);
            const auto rejected=RMeshCandidateService::verify(source,reversed,options());
            QCOMPARE(rejected.code,QString("invalid_candidate_mesh")); QVERIFY(rejected.message.contains("orientation is inward"));
            QCOMPARE(RGeometryCandidateService::geometryFingerprint(reversed),before);
        }
    }
    void sameSidePositiveInteriorFaceRejected()
    {
        const RModel source=tetra(); RModel candidate(source); candidate.setNNodes(5); candidate.getNode(4).set(0.2,0.2,0.2);
        for (uint i=0;i<5;++i) candidate.setNodePhysicalId(i,2001+i);
        candidate.setNVolumes(1); candidate.getVolume(0).setName("WF14 single region");
        candidate.getVolume(0).setMaterial(source.getSurface(0).getMaterial());
        for (const std::array<uint,4> &nodes:{std::array<uint,4>{0,1,2,3},std::array<uint,4>{0,1,2,4}})
        {
            RElement element(R_ELEMENT_TETRA1); for (uint i=0;i<4;++i) element.setNodeId(i,nodes[i]);
            candidate.addElement(element,true,0); QVERIFY(determinant(candidate,element)>0);
        }
        for (uint i=0;i<candidate.getNElements();++i) candidate.setElementPhysicalId(i,3001+i);
        const auto rejected=RMeshCandidateService::verify(source,candidate,options(0.2));
        QCOMPARE(rejected.code,QString("invalid_candidate_mesh")); QVERIFY(rejected.message.contains("same orientation"));
    }
    void invalidShellsRejectBeforeGeneration()
    {
        RModel hole=cube(); hole.setNElements(11); hole.getSurface(5).resize(1);
        QVERIFY(!RMeshCandidateService::preview(hole,options()).valid);
        RModel reversed=tetra();
        for (uint i=0;i<reversed.getNElements();++i)
        {
            RElement &e=reversed.getElement(i); const uint a=e.getNodeId(1); e.setNodeId(1,e.getNodeId(2)); e.setNodeId(2,a);
        }
        QVERIFY(!RMeshCandidateService::preview(reversed,options()).valid);
        RModel mixed=tetra(); mixed.getElement(0).setNodeId(1,1); mixed.getElement(0).setNodeId(2,2);
        QVERIFY(!RMeshCandidateService::preview(mixed,options()).valid);
        RModel duplicate=tetra(); duplicate.setElement(3,duplicate.getElement(0),false);
        QVERIFY(!RMeshCandidateService::preview(duplicate,options()).valid);
        RModel degenerate=tetra(); degenerate.getNode(2).set(0.5,0,0);
        QVERIFY(!RMeshCandidateService::preview(degenerate,options()).valid);
        RModel nan=tetra(); nan.getNode(0).set(std::numeric_limits<double>::quiet_NaN(),0,0);
        QVERIFY(!RMeshCandidateService::preview(nan,options()).valid);
        RModel orphan=tetra(); orphan.setNNodes(5); orphan.getNode(4).set(3,3,3);
        QVERIFY(!RMeshCandidateService::preview(orphan,options()).valid);
        RModel concave=cube(); concave.getNode(6).set(0.3,0.3,0.3);
        QVERIFY(!RMeshCandidateService::preview(concave,options()).valid);
        RModel wrongType=tetra(); wrongType.getElement(0).setType(R_ELEMENT_TRI2);
        QVERIFY(!RMeshCandidateService::preview(wrongType,options()).valid);
    }
    void resourceIdentityAndAmbiguousLabelsReject()
    {
        const RModel source=tetra(); QCOMPARE(RMeshCandidateService::preview(source,options(1e-12)).code,QString("resource_limit"));
        // Bbox/requested is 1667; bbox/backend is 3333 and must reject before generation.
        QCOMPARE(RMeshCandidateService::preview(source,options(0.0006)).code,QString("resource_limit"));
        RModel overflow=tetra(); overflow.setNodePhysicalId(0,std::numeric_limits<uint>::max()-1);
        QCOMPARE(RMeshCandidateService::preview(overflow,options()).code,QString("physical_id_exhausted"));
        RModel badLabels=cube(); badLabels.getSurface(1).setName(badLabels.getSurface(0).getName());
        QVERIFY(!RMeshCandidateService::preview(badLabels,options()).valid);
        RModel overlap=cube(); overlap.getSurface(1).add(0);
        QVERIFY(!RMeshCandidateService::preview(overlap,options()).valid);
        RModel mixedMaterial=cube(); RMaterial other=mixedMaterial.getSurface(1).getMaterial(); other.setName("another region"); mixedMaterial.getSurface(1).setMaterial(other);
        QVERIFY(!RMeshCandidateService::preview(mixedMaterial,options()).valid);
        RModel twoRegions=tetra(); twoRegions.setNVolumes(2);
        QVERIFY(!RMeshCandidateService::preview(twoRegions,options()).valid);
    }
    void cancellationAndFailureNeverReturnCandidate()
    {
        const RModel source=tetra(); const QString pre=RGeometryCandidateService::geometryFingerprint(source);
        std::atomic_bool stop{true}; const auto r=RMeshCandidateService::generate(source,options(),&stop);
        QVERIFY(!r.valid); QCOMPARE(r.code,QString("cancelled")); QVERIFY(r.candidate.isEmpty()); QVERIFY(r.postGeometrySha256.isEmpty());
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre);
        const auto invalid=RMeshCandidateService::generate(source,options(-1)); QVERIFY(!invalid.valid); QVERIFY(invalid.candidate.isEmpty());
    }
    void oldMappingsAreClearedOnlyInCandidate()
    {
        RModel source=tetra();
        source.getProblemSetup().getStructuralControlSetup().enabled=true;
        source.getProblemSetup().getStructuralControlSetup().controllerId="old-controller";
        source.getProblemSetup().setRestart(true);
        RStructuralSensor sensor; sensor.id="old-sensor"; source.getProblemSetup().getStructuralInteractionSetup().getSensors().push_back(sensor);
        RStructuralActuator actuator; actuator.id="old-actuator"; source.getProblemSetup().getStructuralInteractionSetup().getActuators().push_back(actuator);
        RBoundaryCondition condition(R_BOUNDARY_CONDITION_DISPLACEMENT); source.getSurface(0).addBoundaryCondition(condition);
        source.setNVariables(1);
        const QString pre=RGeometryCandidateService::geometryFingerprint(source);
        const auto r=RMeshCandidateService::generate(source,options()); QVERIFY2(r.valid,qPrintable(r.message));
        identitiesAndSource(source,r,pre);
        QCOMPARE(source.getNVariables(),1u); QVERIFY(source.getProblemSetup().getStructuralControlSetup().enabled);
        QCOMPARE(source.getSurface(0).getNBoundaryConditions(),1u); QVERIFY(source.getProblemSetup().getRestart());
        QCOMPARE(source.getProblemSetup().getStructuralInteractionSetup().getSensors().size(),size_t(1));
        QCOMPARE(r.candidate.getNVariables(),0u); QVERIFY(!r.candidate.getProblemSetup().getStructuralControlSetup().enabled);
        QVERIFY(!r.candidate.getProblemSetup().getRestart()); QCOMPARE(r.candidate.getSurface(0).getNBoundaryConditions(),0u);
        QVERIFY(r.candidate.getProblemSetup().getStructuralInteractionSetup().getSensors().empty());
        QVERIFY(r.candidate.getProblemSetup().getStructuralInteractionSetup().getActuators().empty());
        QVERIFY(r.report.value("invalidatedBindingKinds").toArray().contains("ROM")); QVERIFY(r.report.value("rebindingRequired").toBool());
    }
};

QTEST_APPLESS_MAIN(MeshCandidateTest)
#include "tst_mesh_candidate.moc"
