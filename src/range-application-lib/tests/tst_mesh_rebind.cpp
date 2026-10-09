#include <QtTest>
#include <rmesh_rebind_service.h>
#include <rgeometry_candidate_service.h>
#include <rmodel_command_service.h>
#include <rml_file_io.h>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <vector>

namespace
{
class AllocatorFixture : public RModel
{
public:
    explicit AllocatorFixture(const RModel &source) : RModel(source) {}
    void exhaustGroupAllocator() { nextElementGroupPhysicalId=RConstants::eod; }
    void leaveOneGroupId() { nextElementGroupPhysicalId=RConstants::eod-1; }
    uint elementHighWater() const { return nextElementPhysicalId; }
    uint groupHighWater() const { return nextElementGroupPhysicalId; }
};
RModel tetra(double scale=1,const std::array<double,3> &offset={0,0,0})
{
    RModel m; m.setNNodes(4); m.setNSurfaces(1); m.setNVolumes(1);
    const double p[4][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    for (uint i=0;i<4;++i)
    {
        m.getNode(i).set(offset[0]+scale*p[i][0],offset[1]+scale*p[i][1],offset[2]+scale*p[i][2]);
        m.setNodePhysicalId(i,101+10*i);
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
RModel volumeFixture(const std::vector<std::array<double,3>> &nodes,const std::vector<std::array<uint,4>> &tets)
{
    RModel m; m.setNNodes(uint(nodes.size())); m.setNSurfaces(1); m.setNVolumes(1);
    for (uint n=0;n<nodes.size();++n)
    {
        m.getNode(n).set(nodes[n][0],nodes[n][1],nodes[n][2]); m.setNodePhysicalId(n,101+10*n);
    }
    std::map<std::array<uint,3>,std::pair<uint,std::array<uint,3>>> incidence;
    const uint localFaces[4][3]={{1,2,3},{0,3,2},{0,1,3},{0,2,1}};
    for (const auto &tet:tets)
    {
        RElement e(R_ELEMENT_TETRA1); for (uint j=0;j<4;++j) e.setNodeId(j,tet[j]); m.addElement(e,true,0);
        for (const auto &local:localFaces)
        {
            std::array<uint,3> face{tet[local[0]],tet[local[1]],tet[local[2]]},key=face;
            std::sort(key.begin(),key.end()); auto &entry=incidence[key]; ++entry.first; entry.second=face;
        }
    }
    for (const auto &entry:incidence) if (entry.second.first==1)
    {
        RElement e(R_ELEMENT_TRI1); for (uint j=0;j<3;++j) e.setNodeId(j,entry.second.second[j]); m.addElement(e,true,0);
    }
    m.getSurface(0).setName("fixture outer"); m.getVolume(0).setName("fixture body");
    m.setProblemTaskTree(RProblemTaskItem()); m.clearResults(); m.clearBoundaryConditions(); m.clearInitialConditions(); m.clearEntityVariableData();
    for (uint g=0;g<m.getNElementGroups();++g)
        while (m.getElementGroupPtr(g)->getNEnvironmentConditions()) m.getElementGroupPtr(g)->removeEnvironmentCondition(0u);
    return m;
}
QJsonObject options()
{
    return {{"unitSI",true},{"staticStress",true},
        {"material",QJsonObject{{"youngModulusPa",1000},{"poissonRatio",0.25},{"densityKgM3",1}}},
        {"constraints",QJsonArray{
            QJsonObject{{"nodePhysicalId",101},{"values",QJsonArray{0,0,0}}},
            QJsonObject{{"nodePhysicalId",111},{"values",QJsonArray{QJsonValue(QJsonValue::Null),0,0}}},
            QJsonObject{{"nodePhysicalId",121},{"values",QJsonArray{QJsonValue(QJsonValue::Null),QJsonValue(QJsonValue::Null),0}}}}},
        {"loads",QJsonArray{QJsonObject{{"nodePhysicalId",131},{"forceN",QJsonArray{0,0,-1}}}}}};
}
void near(double actual,double expected,double tolerance=1e-12)
{
    QVERIFY2(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,
             qPrintable(QString("%1 != %2").arg(actual,0,'g',17).arg(expected,0,'g',17)));
}
}

class MeshRebindTest : public QObject
{
    Q_OBJECT
private slots:
    void convexScopeRejectsDoubleWrappedPositiveTetrahedra()
    {
        // Around A--B the ring turns through two complete revolutions with
        // different radii. Every local tetrahedron has positive determinant,
        // interior faces cancel and the abstract boundary is a sphere, yet
        // their physical interiors overlap. Local incidence is insufficient.
        const std::vector<std::array<double,3>> nodes{{0,0,-1},{0,0,1},
            {1,0,0},{0,1,0},{-1,0,0},{0,-1,0},{2,0,0},{0,2,0},{-2,0,0},{0,-2,0}};
        std::vector<std::array<uint,4>> tets;
        for (uint i=0;i<8;++i) tets.push_back({0,1,2+i,2+(i+1)%8});
        for (const auto &tet:tets)
        {
            const auto &c=nodes[tet[2]],&d=nodes[tet[3]];
            QVERIFY(2*(c[0]*d[1]-c[1]*d[0])>0); // Exact determinant about A--B.
        }
        const RModel source=volumeFixture(nodes,tets); const QString pre=RGeometryCandidateService::geometryFingerprint(source);
        const auto rejected=RMeshRebindService::preview(source,options());
        QCOMPARE(rejected.code,QString("unsupported_mesh")); QVERIFY(!rejected.valid); QVERIFY(rejected.candidate.isEmpty());
        QVERIFY2(rejected.message.contains("WF14 convex boundary preview"),qPrintable(rejected.message));
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre); QCOMPARE(source.getNPoints(),0u);
    }
    void convexTetrahedronInteriorNodeAndCubeRemainEligible()
    {
        const RModel inside=volumeFixture({{0,0,0},{1,0,0},{0,1,0},{0,0,1},{0.25,0.25,0.25}},
            {{4,1,2,3},{4,0,3,2},{4,0,1,3},{4,0,2,1}});
        const RModel cube=volumeFixture({{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
            {{0,1,2,6},{0,2,3,6},{0,3,7,6},{0,7,4,6},{0,4,5,6},{0,5,1,6}});
        for (const RModel &source:{tetra(),inside,cube})
        {
            const QString pre=RGeometryCandidateService::geometryFingerprint(source);
            const auto r=RMeshRebindService::preview(source,options()); QVERIFY2(r.valid,qPrintable(r.message));
            const QJsonObject eligibility=r.report.value("meshEligibility").toObject();
            QCOMPARE(eligibility.value("scope").toString(),QString("WF14_convex_single_region"));
            QVERIFY(eligibility.value("boundaryShellPreviewVerified").toBool());
            QVERIFY(eligibility.value("volumeNodesInsideConvexHalfSpacesVerified").toBool());
            QVERIFY(eligibility.value("tetrahedronAndConvexShellVolumeAgree").toBool());
            QVERIFY(!eligibility.value("generatorInvoked").toBool(true));
            QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre); QCOMPARE(source.getNPoints(),0u);
        }
        const auto r=RMeshRebindService::preview(inside,options()); QVERIFY(r.valid);
        const auto eligibility=r.report.value("meshEligibility").toObject();
        QCOMPARE(eligibility.value("boundaryNodeCount").toInt(),4); QCOMPARE(eligibility.value("halfSpacePairCount").toInt(),20);
        QCOMPARE(r.candidate.getNNodes(),5u); // Interior node was retained, never remapped or dropped.
    }
    void defaultSourceAndStaticCandidateSurviveNativeRoundTrip()
    {
        // Exercises the real serializer lifetime both on successful previews
        // and default-setup rejection, then checks persisted node bindings.
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const RModel source=tetra();
        const QString sourcePath=source.write(directory.filePath("unbound.rbm"),false);
        RModel reopened; reopened.read(sourcePath);
        const auto r=RMeshRebindService::preview(reopened,options()); QVERIFY2(r.valid,qPrintable(r.message));
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(reopened),RGeometryCandidateService::geometryFingerprint(source));
        const QString candidatePath=r.candidate.write(directory.filePath("bound.rbm"),false);
        RModel bound; bound.read(candidatePath);
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(bound),r.postGeometrySha256);
        QCOMPARE(RModelCommandService::stateFingerprint(bound),r.postStateSha256);
        QCOMPARE(bound.getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_STRESS); QCOMPARE(bound.getNPoints(),4u);
        reopened.getProblemSetup().getModalSetup().setNModesToExtract(3);
        const auto stale=RMeshRebindService::preview(reopened,options());
        QVERIFY(!stale.valid); QVERIFY(stale.candidate.isEmpty());
        reopened=tetra(); reopened.getTimeSolver().setInputTimeStepSize(0.125);
        const auto staleTime=RMeshRebindService::preview(reopened,options());
        QVERIFY(!staleTime.valid); QVERIFY(staleTime.candidate.isEmpty());
    }
    void exactBboxSizingSeedsSurviveNativeLoadAndRejectRetainedSettings()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        RModel source=tetra(2,{7,-3,11});
        // This translated tetra has a 2 x 2 x 2 bbox. The GUI sizing seed is
        // derived from bbox volume/1000, not shortest or longest mesh edges.
        const double maxSeed=std::cbrt(12.0*(8.0/1000.0)/std::sqrt(2.0)),minSeed=maxSeed*0.1;
        source.getProblemSetup().getMeshSetup().setMinEdgeLength(minSeed);
        source.getProblemSetup().getMeshSetup().setMaxEdgeLength(maxSeed);
        RModel loaded;
        for (const QString &fileName:{QString("seeded-unbound.ram"),QString("seeded-unbound.rbm")})
        {
            const QString path=source.write(directory.filePath(fileName),false); loaded.read(path);
            QCOMPARE(loaded.getProblemSetup().getMeshSetup().getMinEdgeLength(),minSeed);
            QCOMPARE(loaded.getProblemSetup().getMeshSetup().getMaxEdgeLength(),maxSeed);
            const auto roundTrip=RMeshRebindService::preview(loaded,options());
            QVERIFY2(roundTrip.valid,qPrintable(fileName+": "+roundTrip.message));
        }
        const QString before=RModelCommandService::stateFingerprint(loaded);
        const auto accepted=RMeshRebindService::preview(loaded,options());
        QVERIFY2(accepted.valid,qPrintable(accepted.message));
        const auto proof=accepted.report.value("meshEligibility").toObject().value("sourceSetupProof").toObject();
        QCOMPARE(proof.value("profile").toString(),QString("default_reset_with_bbox_sizing_seeds"));
        QCOMPARE(proof.value("sizingSeedOrigin").toString(),QString("Model::initializeMeshInput"));
        QVERIFY(proof.value("fullDefaultRemainderVerified").toBool());
        QVERIFY(proof.value("bboxSizingSeedsVerified").toBool());
        QCOMPARE(proof.value("minimumSizingSeedM").toDouble(),minSeed);
        QCOMPARE(proof.value("maximumSizingSeedM").toDouble(),maxSeed);
        QCOMPARE(accepted.candidate.getProblemSetup().getMeshSetup().getMinEdgeLength(),0.0);
        QCOMPARE(accepted.candidate.getProblemSetup().getMeshSetup().getMaxEdgeLength(),0.0);
        QCOMPARE(RModelCommandService::stateFingerprint(loaded),before);
        const auto rejected=[&](const RModel &retained) {
            const QString state=RModelCommandService::stateFingerprint(retained);
            const auto result=RMeshRebindService::preview(retained,options());
            QVERIFY(!result.valid); QVERIFY(result.candidate.isEmpty());
            QCOMPARE(result.code,QString("unsupported_mesh"));
            QCOMPARE(RModelCommandService::stateFingerprint(retained),state);
        };
        RModel forged(loaded);
        forged.getProblemSetup().getMeshSetup().setMinEdgeLength(std::nextafter(minSeed,1.0)); rejected(forged);
        forged=loaded;
        forged.getProblemSetup().getMeshSetup().setMaxEdgeLength(std::nextafter(maxSeed,1.0)); rejected(forged);
        forged=loaded; forged.getProblemSetup().getMeshSetup().setMinEdgeLength(0); rejected(forged);
        // Legacy RAM seeds rounded to default six-digit text precision are
        // still rejected. Historical retained values gain no tolerance.
        forged=loaded;
        forged.getProblemSetup().getMeshSetup().setMinEdgeLength(QString::number(minSeed,'g',6).toDouble());
        forged.getProblemSetup().getMeshSetup().setMaxEdgeLength(QString::number(maxSeed,'g',6).toDouble()); rejected(forged);
        forged=loaded; forged.getProblemSetup().getMeshSetup().getVariables().insert(R_VARIABLE_DISPLACEMENT_X); rejected(forged);
        forged=loaded; forged.getProblemSetup().getModalSetup().setNModesToExtract(3); rejected(forged);
        forged=loaded; forged.getTimeSolver().setInputTimeStepSize(0.125); rejected(forged);
        // Geometry edits invalidate old seeds even when the mesh remains valid.
        forged=loaded; forged.getNode(3).set(7,-3,14); rejected(forged);
    }
    void atomicPublicationPreservesSeedHistoryAndAllocatorHighWater()
    {
        RModel source=tetra();
        const double maxSeed=std::cbrt(12.0*(1.0/1000.0)/std::sqrt(2.0));
        source.getProblemSetup().getMeshSetup().setMinEdgeLength(maxSeed*0.1);
        source.getProblemSetup().getMeshSetup().setMaxEdgeLength(maxSeed);
        AllocatorFixture live(source);
        const auto forward=RModelCommandService::preview(live,"mesh.rebind.static",options());
        QVERIFY2(forward.valid,qPrintable(forward.message));
        QString error;
        const QString original=RModelCommandService::stateFingerprint(live);
        const uint initialElements=live.elementHighWater(),initialGroups=live.groupHighWater();
        QVERIFY(!live.applyStaticMeshBindingCandidate(forward.candidate,[](const RModel &) { return false; },&error));
        QVERIFY(!live.applyStaticMeshBindingCandidate(forward.candidate,[](const RModel &) -> bool { throw std::bad_alloc(); },&error));
        QCOMPARE(RModelCommandService::stateFingerprint(live),original);
        QCOMPARE(live.getNPoints(),0u); QCOMPARE(live.getNElements(),5u);
        QCOMPARE(live.getProblemSetup().getMeshSetup().getMaxEdgeLength(),maxSeed);
        QCOMPARE(live.elementHighWater(),initialElements); QCOMPARE(live.groupHighWater(),initialGroups);
        AllocatorFixture retained(source);
        retained.getProblemSetup().getMeshSetup().setMinEdgeLength(std::nextafter(maxSeed*0.1,1.0));
        QVERIFY(!retained.applyStaticMeshBindingCandidate(forward.candidate,[](const RModel &) { return true; },&error));
        retained=AllocatorFixture(source); retained.getProblemSetup().getModalSetup().setNModesToExtract(3);
        QVERIFY(!retained.applyStaticMeshBindingCandidate(forward.candidate,[](const RModel &) { return true; },&error));
        retained=AllocatorFixture(source); retained.getProblemSetup().getStructuralDynamicsRestartData().q.push_back(1.0);
        QVERIFY(!retained.applyStaticMeshBindingCandidate(forward.candidate,[](const RModel &) { return true; },&error));
        const auto publish=[&](const RModel &expected) {
            const QString state=RModelCommandService::stateFingerprint(expected);
            const QString geometry=RGeometryCandidateService::geometryFingerprint(expected);
            return live.applyStaticMeshBindingCandidate(expected,[&](const RModel &prepared) {
                return RModelCommandService::stateFingerprint(prepared)==state
                    && RGeometryCandidateService::geometryFingerprint(prepared)==geometry
                    && prepared.getProblemSetup().getMeshSetup().getMinEdgeLength()==expected.getProblemSetup().getMeshSetup().getMinEdgeLength()
                    && prepared.getProblemSetup().getMeshSetup().getMaxEdgeLength()==expected.getProblemSetup().getMeshSetup().getMaxEdgeLength();
            },&error);
        };
        QVERIFY2(publish(forward.candidate),qPrintable(error));
        QCOMPARE(live.getProblemSetup().getMeshSetup().getMaxEdgeLength(),0.0);
        QVector<uint> pointIds,elementIds;
        for (uint p=0;p<live.getNPoints();++p)
        { pointIds.append(live.getPoint(p).getPhysicalId()); elementIds.append(live.getElementPhysicalId(live.getPoint(p).get(0))); }
        const uint boundElements=live.elementHighWater(),boundGroups=live.groupHighWater();
        const auto undo=RModelCommandService::applyDelta(live,forward.delta,false);
        QVERIFY2(undo.valid,qPrintable(undo.message));
        const QString boundState=RModelCommandService::stateFingerprint(live);
        QVERIFY(!live.applyStaticMeshBindingCandidate(undo.candidate,[](const RModel &) { return false; },&error));
        QVERIFY(!live.applyStaticMeshBindingCandidate(undo.candidate,[](const RModel &) -> bool { throw std::bad_alloc(); },&error));
        QCOMPARE(RModelCommandService::stateFingerprint(live),boundState);
        QCOMPARE(live.getNPoints(),uint(pointIds.size()));
        QCOMPARE(live.getProblemSetup().getMeshSetup().getMaxEdgeLength(),0.0);
        QCOMPARE(live.elementHighWater(),boundElements); QCOMPARE(live.groupHighWater(),boundGroups);
        QVERIFY2(publish(undo.candidate),qPrintable(error));
        QCOMPARE(live.getNPoints(),0u); QCOMPARE(live.getNElements(),5u);
        QCOMPARE(live.getProblemSetup().getMeshSetup().getMaxEdgeLength(),maxSeed);
        QVERIFY(live.elementHighWater()>=boundElements); QVERIFY(live.groupHighWater()>=boundGroups);
        // New edits after Undo allocate above the old owned targets, while
        // Redo restores captured IDs only through the authoritative delta.
        AllocatorFixture allocationProof(live); allocationProof.addPoint(RPoint());
        QVERIFY(allocationProof.getPoint(0).getPhysicalId()>=boundGroups);
        RElement element(R_ELEMENT_POINT); element.setNodeId(0,0);
        allocationProof.addElement(element,true,0);
        QVERIFY(allocationProof.getElementPhysicalId(allocationProof.getNElements()-1)>=boundElements);
        const uint undoneElements=live.elementHighWater(),undoneGroups=live.groupHighWater();
        const auto redo=RModelCommandService::applyDelta(live,forward.delta,true);
        QVERIFY2(redo.valid,qPrintable(redo.message)); QVERIFY2(publish(redo.candidate),qPrintable(error));
        QVERIFY(live.elementHighWater()>=undoneElements); QVERIFY(live.groupHighWater()>=undoneGroups);
        QCOMPARE(live.getNPoints(),uint(pointIds.size()));
        for (uint p=0;p<live.getNPoints();++p)
        { QCOMPARE(live.getPoint(p).getPhysicalId(),pointIds[int(p)]); QCOMPARE(live.getElementPhysicalId(live.getPoint(p).get(0)),elementIds[int(p)]); }
    }
    void sizingSeedAsciiWriterPreservesCallerPrecision()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path=directory.filePath("mesh-setup.txt");
        const double seed=std::cbrt(12.0/1000.0/std::sqrt(2.0));
        RMeshSetup setup; setup.setMinEdgeLength(seed*0.1); setup.setMaxEdgeLength(seed);
        RSaveFile output(path,RSaveFile::ASCII); output.setVersion(RModel::version);
        QVERIFY(output.open(QIODevice::WriteOnly|QIODevice::Text));
        output.getTextStream().setRealNumberPrecision(4);
        RFileIO::writeAscii(output,setup,true);
        QCOMPARE(output.getTextStream().realNumberPrecision(),4);
        QVERIFY(output.commit());
        RFile input(path,RFile::ASCII); input.setVersion(RModel::version);
        QVERIFY(input.open(QIODevice::ReadOnly|QIODevice::Text));
        RMeshSetup loaded; RFileIO::readAscii(input,loaded);
        QCOMPARE(loaded.getMinEdgeLength(),setup.getMinEdgeLength());
        QCOMPARE(loaded.getMaxEdgeLength(),setup.getMaxEdgeLength());
    }
    void completeAtomicCandidate()
    {
        const RModel source=tetra(); const QString pre=RGeometryCandidateService::geometryFingerprint(source),state=RModelCommandService::stateFingerprint(source);
        const auto r=RMeshRebindService::preview(source,options());
        QVERIFY2(r.valid,qPrintable(r.code+": "+r.message));
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre); QCOMPARE(RModelCommandService::stateFingerprint(source),state);
        QCOMPARE(source.getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_NONE); QCOMPARE(source.getNPoints(),0u);
        QCOMPARE(r.candidate.getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_STRESS);
        QCOMPARE(r.candidate.getNNodes(),4u); QCOMPARE(r.candidate.getNElements(),9u); QCOMPARE(r.candidate.getNPoints(),4u);
        QCOMPARE(r.report.value("physicsState").toString(),QString("BOUND_STATIC")); QVERIFY(!r.report.value("requiresRebinding").toBool(true));
        QCOMPARE(r.report.value("rigidBodyConstraintRank").toInt(),6); QCOMPARE(r.candidate.getNVariables(),0u);
        QVERIFY(!r.candidate.getProblemSetup().getStructuralControlSetup().enabled);
        QVERIFY(!r.candidate.getProblemSetup().getStructuralDynamicsSetup().getEnabled());
        QVERIFY(!r.candidate.getProblemSetup().getStructuralDynamicsRestartData().available);
        QCOMPARE(r.sourceGeometrySha256,pre); QVERIFY(r.postGeometrySha256!=pre);
        QCOMPARE(r.postGeometrySha256,RGeometryCandidateService::geometryFingerprint(r.candidate));
        QCOMPARE(r.postStateSha256,RModelCommandService::stateFingerprint(r.candidate));
        std::set<uint> ids;
        for (uint e=0;e<source.getNElements();++e) ids.insert(source.getElementPhysicalId(e));
        for (uint e=source.getNElements();e<r.candidate.getNElements();++e) QVERIFY(ids.insert(r.candidate.getElementPhysicalId(e)).second);
        ids.clear(); for (uint g=0;g<source.getNElementGroups();++g) ids.insert(source.getElementGroupPtr(g)->getPhysicalId());
        for (uint p=0;p<r.candidate.getNPoints();++p)
        {
            const auto &group=r.candidate.getPoint(p); QVERIFY(ids.insert(group.getPhysicalId()).second);
            QCOMPARE(group.size(),1u); const auto &e=r.candidate.getElement(group.get(0)); QCOMPARE(e.getType(),R_ELEMENT_POINT); QCOMPARE(e.size(),1u);
            QCOMPARE(group.getNInitialConditions(),0u); QCOMPARE(group.getNEnvironmentConditions(),0u);
            QCOMPARE(group.getMaterial().size(),0u);
        }
        const auto &material=r.candidate.getVolume(0).getMaterial(); QCOMPARE(material.getState(),RMaterial::Solid); QCOMPARE(material.size(),4u);
        near(material.get(material.findPosition(RMaterialProperty::ModulusOfElasiticity)).get(293.15),1000);
        near(material.get(material.findPosition(RMaterialProperty::PoissonRatio)).get(293.15),0.25);
        near(material.get(material.findPosition(RMaterialProperty::Density)).get(293.15),1);
        near(material.get(material.findPosition(RMaterialProperty::ThermalExpansionCoefficient)).get(293.15),0);
        QCOMPARE(r.candidate.getSurface(0).getMaterial().size(),0u);
        QCOMPARE(r.candidate.getSurface(0).getNEnvironmentConditions(),0u); QCOMPARE(r.candidate.getVolume(0).getNEnvironmentConditions(),0u);
        QCOMPARE(r.report.value("forceResultantN").toArray(),QJsonArray({0,0,-1}));
        QCOMPARE(r.report.value("forceMomentAboutGlobalOriginNm").toArray(),QJsonArray({0,0,0}));
        // Deterministic normalized bindings and material IDs bind repeated preview.
        const auto second=RMeshRebindService::preview(source,options()); QVERIFY(second.valid);
        QCOMPARE(second.postStateSha256,r.postStateSha256); QCOMPARE(second.postGeometrySha256,r.postGeometrySha256);
    }
    void strictSchemaAndFailureDoesNotMutateSource()
    {
        const RModel source=tetra(); const QString pre=RModelCommandService::stateFingerprint(source);
        for (const QString &field:{QString("callback"),QString("unit"),QString("nearestNode")})
        {
            QJsonObject a=options(); a.insert(field,"arbitrary"); const auto r=RMeshRebindService::preview(source,a);
            QVERIFY(!r.valid); QVERIFY(r.candidate.isEmpty()); QVERIFY(r.postGeometrySha256.isEmpty());
        }
        for (const QString &field:{QString("unitSI"),QString("staticStress"),QString("material"),QString("constraints"),QString("loads")})
        {
            QJsonObject a=options(); a.remove(field); QVERIFY(!RMeshRebindService::preview(source,a).valid);
        }
        for (const QJsonValue &value:{QJsonValue(false),QJsonValue("true"),QJsonValue(1)})
        {
            QJsonObject a=options(); a.insert("unitSI",value); QVERIFY(!RMeshRebindService::preview(source,a).valid);
        }
        for (const QJsonValue &id:{QJsonValue(0),QJsonValue(999),QJsonValue(101.5),QJsonValue("131"),QJsonValue(double(RConstants::eod))})
        {
            QJsonObject a=options(); a.insert("loads",QJsonArray{QJsonObject{{"nodePhysicalId",id},{"forceN",QJsonArray{0,0,-1}}}});
            QVERIFY(!RMeshRebindService::preview(source,a).valid);
        }
        for (const QJsonArray &force:{QJsonArray{0,0,0},QJsonArray{0,0},QJsonArray{0,0,"-1"},QJsonArray{0,0,1e13},QJsonArray{0,0,QJsonValue(QJsonValue::Null)}})
        {
            QJsonObject a=options(); a.insert("loads",QJsonArray{QJsonObject{{"nodePhysicalId",131},{"forceN",force}}});
            QVERIFY(!RMeshRebindService::preview(source,a).valid);
        }
        QJsonObject a=options(); auto loads=a.value("loads").toArray(); loads.append(loads[0]); a.insert("loads",loads);
        QVERIFY(!RMeshRebindService::preview(source,a).valid);
        a=options(); auto constraints=a.value("constraints").toArray(); constraints.append(constraints[0]); a.insert("constraints",constraints);
        QVERIFY(!RMeshRebindService::preview(source,a).valid);
        a=options(); auto material=a.value("material").toObject(); material.insert("thermalExpansionPerK",1e-5); a.insert("material",material);
        QVERIFY(!RMeshRebindService::preview(source,a).valid);
        for (double nu:{-1.0,0.5,1.0})
        {
            a=options(); material=a.value("material").toObject(); material.insert("poissonRatio",nu); a.insert("material",material);
            QCOMPARE(RMeshRebindService::preview(source,a).code,QString("invalid_material"));
        }
        QCOMPARE(RModelCommandService::stateFingerprint(source),pre);
    }
    void rigidModeRankAndGeometricInvariance()
    {
        // Six prescribed rows concentrated on two nodes leave rotation around
        // their connecting line free: a simple component-count check is wrong.
        QJsonObject a=options(); a.insert("constraints",QJsonArray{
            QJsonObject{{"nodePhysicalId",101},{"values",QJsonArray{0,0,0}}},
            QJsonObject{{"nodePhysicalId",111},{"values",QJsonArray{0,0,0}}}});
        QCOMPARE(RMeshRebindService::preview(tetra(),a).code,QString("insufficient_constraints"));
        for (double scale:{1e-6,1.0,1e3})
        {
            const auto r=RMeshRebindService::preview(tetra(scale,{5,-2,7}),options());
            QVERIFY2(r.valid,qPrintable(r.message)); QCOMPARE(r.report.value("rigidBodyConstraintRank").toInt(),6);
            QCOMPARE(r.report.value("forceMomentAboutGlobalOriginNm").toArray(),QJsonArray({2,5,0}));
        }
        a=options(); auto constraints=a.value("constraints").toArray(); auto one=constraints[2].toObject();
        one.insert("values",QJsonArray{QJsonValue(QJsonValue::Null),QJsonValue(QJsonValue::Null),QJsonValue(QJsonValue::Null)});
        constraints[2]=one; a.insert("constraints",constraints); QVERIFY(!RMeshRebindService::preview(tetra(),a).valid);
    }
    void dirtySourceAndOverflowRejectAtomically()
    {
        const auto reject=[](const RModel &source) {
            const auto r=RMeshRebindService::preview(source,options()); QVERIFY(!r.valid); QVERIFY(r.candidate.isEmpty());
        };
        RModel source=tetra(); source.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS)); reject(source);
        source=tetra(); source.getProblemSetup().getStructuralControlSetup().controllerId="old-control"; reject(source);
        source=tetra(); source.getProblemSetup().getStructuralControlSetup().controllerId=QString(QChar(0))+"hidden-control"; reject(source);
        source=tetra(); source.getProblemSetup().getStructuralDynamicsRestartData().q={1}; reject(source);
        source=tetra(); source.getProblemSetup().setRestart(true); reject(source);
        source=tetra(); source.getSurface(0).addBoundaryCondition(RBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT)); reject(source);
        source=tetra(); source.setNVariables(1); reject(source);
        source=tetra(); source.getVolume(0).getData().getVariableData()[R_VARIABLE_DISPLACEMENT_X]=0; reject(source);
        // Last usable element ID advances the current allocator to exhausted.
        source=tetra(); source.setElementPhysicalId(0,RConstants::eod-1);
        const QString pre=RGeometryCandidateService::geometryFingerprint(source); reject(source);
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),pre); QCOMPARE(source.getNPoints(),0u);
        AllocatorFixture exhausted(tetra()); exhausted.exhaustGroupAllocator(); reject(exhausted); QCOMPARE(exhausted.getNPoints(),0u);
        AllocatorFixture partial(tetra()); partial.leaveOneGroupId(); reject(partial); QCOMPARE(partial.getNPoints(),0u);
        source=tetra(); source.getElement(0).setNodeId(1,3); reject(source); // Missing exterior face.
    }
    void constraintAndLoadShareOneFreshPoint()
    {
        QJsonObject a=options(); a.insert("loads",QJsonArray{QJsonObject{{"nodePhysicalId",101},{"forceN",QJsonArray{0,0,-1}}}});
        const auto r=RMeshRebindService::preview(tetra(),a); QVERIFY2(r.valid,qPrintable(r.message)); QCOMPARE(r.candidate.getNPoints(),3u);
        const auto &p=r.candidate.getPoint(0); QCOMPARE(p.getNBoundaryConditions(),2u);
        const auto &bc=p.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        QVERIFY(!bc.getHasLocalDirection());
        const auto &component=bc.getComponent(bc.findComponentPosition(R_VARIABLE_FORCE_Z));
        QVERIFY(component.getEnabled()); QCOMPARE(component.size(),1u); near(component.getKey(0),0); near(component.getValue(0),-1);
    }
    void independentConstantStrainTetraReference()
    {
        // Independent textbook CST derivation, not Range element/assembly APIs.
        // N={1-x-y-z,x,y,z}, V=1/6, engineering-strain order
        // [xx,yy,zz,xy,yz,xz]. K=V B^T D B. Reactions use K*u-f.
        const double gradients[4][3]={{-1,-1,-1},{1,0,0},{0,1,0},{0,0,1}};
        double b[6][12]{},d[6][6]{},k[12][12]{};
        for (int n=0;n<4;++n)
        {
            const auto &g=gradients[n]; b[0][3*n]=g[0]; b[1][3*n+1]=g[1]; b[2][3*n+2]=g[2];
            b[3][3*n]=g[1]; b[3][3*n+1]=g[0]; b[4][3*n+1]=g[2]; b[4][3*n+2]=g[1];
            b[5][3*n]=g[2]; b[5][3*n+2]=g[0];
        }
        const double lambda=400,mu=400;
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) d[i][j]=lambda+(i==j ? 2*mu : 0);
        for (int i=3;i<6;++i) d[i][i]=mu;
        for (int i=0;i<12;++i) for (int j=0;j<12;++j)
            for (int p=0;p<6;++p) for (int q=0;q<6;++q) k[i][j]+=b[p][i]*d[p][q]*b[q][j]/6;
        const double u[12]={0,0,0,0.0015,0,0,0,0.0015,0,0,0,-0.006};
        double strain[6]{},stress[6]{},internal[12]{},energy=0;
        for (int i=0;i<6;++i) for (int j=0;j<12;++j) strain[i]+=b[i][j]*u[j];
        for (int i=0;i<6;++i) for (int j=0;j<6;++j) stress[i]+=d[i][j]*strain[j];
        for (int i=0;i<12;++i) for (int j=0;j<12;++j) internal[i]+=k[i][j]*u[j];
        for (int i=0;i<12;++i) energy+=u[i]*internal[i]/2;
        near(strain[0],0.0015); near(strain[1],0.0015); near(strain[2],-0.006);
        for (int i=3;i<6;++i) near(strain[i],0);
        for (int i=0;i<6;++i) near(stress[i],i==2 ? -6 : 0);
        for (int i=0;i<12;++i) near(internal[i],i==2 ? 1 : i==11 ? -1 : 0);
        near(energy,0.003); near(internal[2]+internal[11],0);
        // Root/native-solver integration must compare its actual displacement,
        // strain/stress/energy and reactions against these independent values.
    }
};

QTEST_APPLESS_MAIN(MeshRebindTest)
#include "tst_mesh_rebind.moc"
