#include <QtTest>
#include <rgeometry_candidate_service.h>
#include <cmath>
#include <limits>

namespace
{
RModel fixture(RElementType type = R_ELEMENT_TRUSS1)
{
    RModel m;
    m.setNNodes(3);
    m.getNode(0).set(1,0,0);
    m.getNode(1).set(2,0,0);
    m.getNode(2).set(0,0,1);
    m.setNodePhysicalId(0,41);
    m.setNodePhysicalId(1,97);
    m.setNodePhysicalId(2,103);
    m.setNLines(1);
    m.getLine(0).setLocalYDirection(RR3Vector(0,1,0));
    RElement e(type);
    e.setNodeId(0,0);
    e.setNodeId(1,1);
    m.addElement(e,true,0);
    m.setElementPhysicalId(0,501);
    return m;
}
QJsonObject transform(double angle = 90.0)
{
    return {{"unit","m"},{"angleUnit","deg"},{"directionPolicy","preserve_global"},
            {"center",QJsonArray{0,0,0}},{"axis",QJsonArray{0,0,1}},
            {"angle",angle},{"translation",QJsonArray{3,4,5}}};
}
QJsonObject setNodes(const QJsonArray &nodes)
{
    return {{"unit","m"},{"directionPolicy","preserve_global"},{"nodes",nodes}};
}
QJsonObject node(const QJsonValue &id,const QJsonArray &coordinates)
{
    return {{"physicalNodeId",id},{"coordinates",coordinates}};
}
auto preview(const RModel &m,const QJsonObject &a)
{
    return RGeometryCandidateService::preview(m,"geometry.rigid_transform",a);
}
void near(double actual,double expected)
{
    QVERIFY2(std::abs(actual-expected)<1e-12,qPrintable(QString("%1 != %2").arg(actual,0,'g',17).arg(expected,0,'g',17)));
}
}

class GeometryCandidateTest : public QObject
{
    Q_OBJECT
private slots:
    void rightHandedRotationTranslationAndInverse()
    {
        const RModel m = fixture();
        const QString hash = RGeometryCandidateService::geometryFingerprint(m);
        const auto r = preview(m,transform());
        QVERIFY2(r.valid,qPrintable(r.message));
        near(r.candidate.getNode(0).getX(),3);
        near(r.candidate.getNode(0).getY(),5);
        near(r.candidate.getNode(0).getZ(),5);
        near(r.candidate.getNode(1).getX(),3);
        near(r.candidate.getNode(1).getY(),6);
        QCOMPARE(r.delta.preGeometrySha256,hash);
        QVERIFY(r.delta.postGeometrySha256 != hash);
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(m),hash);
        QCOMPARE(m.getNode(0).getX(),1.0);
        QCOMPARE(r.candidate.getNNodes(),m.getNNodes());
        QCOMPARE(r.candidate.getNElements(),m.getNElements());
        for (uint i=0;i<m.getNNodes();++i) QCOMPARE(r.candidate.getNodePhysicalId(i),m.getNodePhysicalId(i));
        QCOMPARE(r.candidate.getElementPhysicalId(0),501u);
        QCOMPARE(r.candidate.getElement(0).getNodeId(0),0u);
        QCOMPARE(r.delta.nodes.size(),size_t(3));
        QCOMPARE(r.delta.nodes[0].physicalNodeId,41u);
        QCOMPARE(r.delta.nodes[1].physicalNodeId,97u);
        QCOMPARE(r.delta.nodes[2].physicalNodeId,103u);
        QJsonObject inverse = transform(-90);
        // R^-1(x-t) has translation -R^-1 t = (-4,3,-5).
        inverse.insert("translation",QJsonArray{-4,3,-5});
        const auto restored = preview(r.candidate,inverse);
        QVERIFY(restored.valid);
        for (uint i=0;i<m.getNNodes();++i)
        {
            near(restored.candidate.getNode(i).getX(),m.getNode(i).getX());
            near(restored.candidate.getNode(i).getY(),m.getNode(i).getY());
            near(restored.candidate.getNode(i).getZ(),m.getNode(i).getZ());
        }
    }
    void centeredRotationAndNormalizedAxis()
    {
        QJsonObject a = transform();
        a.insert("center",QJsonArray{1,0,0});
        a.insert("translation",QJsonArray{0,0,0});
        a.insert("axis",QJsonArray{0,0,1e300});
        const auto r = preview(fixture(),a);
        QVERIFY(r.valid);
        near(r.candidate.getNode(0).getX(),1);
        near(r.candidate.getNode(0).getY(),0);
        near(r.candidate.getNode(1).getX(),1);
        near(r.candidate.getNode(1).getY(),1);
        QCOMPARE(r.normalizedArguments.value("axis").toArray(),QJsonArray({0,0,1e300}));
        a.insert("axis",QJsonArray{3,4,0});
        const RModel m = fixture();
        const auto irregular = preview(m,a);
        QVERIFY(irregular.valid);
        const auto replay = preview(m,irregular.normalizedArguments);
        QVERIFY(replay.valid);
        QCOMPARE(replay.delta.postGeometrySha256,irregular.delta.postGeometrySha256);
    }
    void explicitCoordinatesAndNoOp()
    {
        const RModel m = fixture();
        auto r = RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",
            setNodes({node(97,{4,2,0}),node(41,{2,1,0})}));
        QVERIFY(r.valid);
        QCOMPARE(r.delta.nodes[0].physicalNodeId,41u);
        QCOMPARE(r.delta.nodes[1].physicalNodeId,97u);
        QCOMPARE(r.candidate.getNode(2).getZ(),1.0);
        r = RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",setNodes({node(41,{1,0,0})}));
        QVERIFY(r.valid);
        QVERIFY(r.delta.noOp);
        QCOMPARE(r.delta.preGeometrySha256,r.delta.postGeometrySha256);
        QVERIFY(r.delta.nodes.empty());
        QJsonObject a = transform(360);
        a.insert("translation",QJsonArray{0,0,0});
        QVERIFY(preview(m,a).delta.noOp);
        // QJsonValue canonicalizes an integral -0.0 to zero. Command semantics
        // follow its decoded value; the native geometry hash still binds the
        // actual IEEE coordinates of an independently loaded model.
        r = RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",setNodes({node(41,{1,-0.0,0})}));
        QVERIFY(r.valid);
        QVERIFY(r.delta.noOp);
        QCOMPARE(r.delta.preGeometrySha256,r.delta.postGeometrySha256);
        RModel signedZeroModel = m;
        signedZeroModel.getNode(0).set(1,-0.0,0);
        QVERIFY(RGeometryCandidateService::geometryFingerprint(signedZeroModel) !=
                RGeometryCandidateService::geometryFingerprint(m));
    }
    void rejectClosedSchemaAndInvalidIds()
    {
        const RModel m = fixture();
        const QList<QJsonValue> ids{true,0,-1,1.5,4294967295.0,4294967296.0,999,"41",QJsonValue()};
        for (const QJsonValue &id : ids)
            QVERIFY(!RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",setNodes({node(id,{1,0,0})})).valid);
        QVERIFY(!RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",setNodes({node(41,{1,0,0}),node(41,{2,0,0})})).valid);
        QJsonObject a = transform();
        a.insert("unknown",true);
        QVERIFY(!preview(m,a).valid);
        a = transform(); a.remove("directionPolicy");
        QVERIFY(!preview(m,a).valid);
        a = transform(); a.insert("directionPolicy","rotate_with_geometry");
        QVERIFY(!preview(m,a).valid);
        QVERIFY(!RGeometryCandidateService::preview(m,"geometry.scale",{}).valid);
    }
    void rejectInvalidNumbersAndCollapsedLine()
    {
        const RModel m = fixture();
        for (const QJsonArray &v : QList<QJsonArray>{{1000001,0,0},{true,0,0},{0,0},
                 {std::numeric_limits<double>::infinity(),0,0},{std::numeric_limits<double>::quiet_NaN(),0,0}})
            QVERIFY(!RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",setNodes({node(41,v)})).valid);
        QVERIFY(!RGeometryCandidateService::preview(m,"geometry.node_coordinates.set",setNodes({node(41,{2,0,0})})).valid);
        QJsonObject a = transform(); a.insert("axis",QJsonArray{0,0,0});
        QVERIFY(!preview(m,a).valid);
        a = transform(361); QVERIFY(!preview(m,a).valid);
        a = transform(); a.insert("angle",true); QVERIFY(!preview(m,a).valid);
        a = transform(); a.insert("translation",QJsonArray{1e6,1e6,0}); QVERIFY(!preview(m,a).valid);
        RModel invalid = m; invalid.getNode(0).setX(std::numeric_limits<double>::quiet_NaN());
        QVERIFY(!preview(invalid,transform()).valid);
    }
    void beamDirectionAndGlobalData()
    {
        RModel m = fixture(R_ELEMENT_BEAM1);
        RBoundaryCondition displacement(R_BOUNDARY_CONDITION_DISPLACEMENT);
        RConditionComponent dx(R_VARIABLE_DISPLACEMENT_X);
        dx.add(0,0.0123); displacement.addComponent(dx);
        m.getLine(0).addBoundaryCondition(displacement);
        RBoundaryCondition force(R_BOUNDARY_CONDITION_FORCE);
        RConditionComponent fx(R_VARIABLE_FORCE_X);
        fx.add(0,123.4); fx.add(1,567.8); force.addComponent(fx);
        m.getLine(0).addBoundaryCondition(force);
        RInitialCondition velocity(R_INITIAL_CONDITION_VELOCITY);
        RConditionComponent vx(R_VARIABLE_VELOCITY_X);
        vx.add(0,4.5); velocity.addComponent(vx);
        m.getLine(0).addInitialCondition(velocity);
        m.getProblemSetup().getStructuralControlSetup().controllerId = "global-controller";
        const auto parallel = preview(m,transform());
        QVERIFY(!parallel.valid);
        QJsonObject a = transform(180);
        const auto r = preview(m,a);
        QVERIFY2(r.valid,qPrintable(r.message));
        QCOMPARE(r.candidate.getLine(0).getLocalYDirection()[1],1.0);
        QCOMPARE(r.candidate.getLine(0).getNBoundaryConditions(),2u);
        QVERIFY(r.candidate.getLine(0).hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
        QVERIFY(r.candidate.getLine(0).hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE));
        const auto &afterForce = r.candidate.getLine(0).getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        QVERIFY(afterForce.getComponent(afterForce.findComponentPosition(R_VARIABLE_FORCE_X)) == fx);
        const auto &afterDisplacement = r.candidate.getLine(0).getBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT);
        QVERIFY(afterDisplacement.getComponent(afterDisplacement.findComponentPosition(R_VARIABLE_DISPLACEMENT_X)) == dx);
        const auto &afterVelocity = r.candidate.getLine(0).getInitialCondition(R_INITIAL_CONDITION_VELOCITY);
        QVERIFY(afterVelocity.getComponent(afterVelocity.findComponentPosition(R_VARIABLE_VELOCITY_X)) == vx);
        QCOMPARE(r.candidate.getProblemSetup().getStructuralControlSetup().controllerId,QString("global-controller"));
        RBoundaryCondition roller(R_BOUNDARY_CONDITION_DISPLACEMENT_ROLLER);
        m.getLine(0).addBoundaryCondition(roller);
        QVERIFY(!preview(m,a).valid);
        roller.setExplicitLocalDirection(true);
        roller.setLocalDirection(RR3Vector(0,0,1));
        m.getLine(0).addBoundaryCondition(roller);
        const auto explicitResult = preview(m,a);
        QVERIFY(explicitResult.valid);
        QCOMPARE(explicitResult.candidate.getLine(0).getBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT_ROLLER).getLocalDirection()[2],1.0);
        m.getLine(0).setLocalYDirection(RR3Vector(1,0,0));
        QVERIFY(!preview(m,a).valid);
    }
    void hashBindsAllGeometryAndStableOrdering()
    {
        RModel m = fixture();
        const QString initial = RGeometryCandidateService::geometryFingerprint(m);
        QVERIFY(!initial.isEmpty());
        RModel changed = m; changed.getElement(0).setNodeId(1,2);
        QVERIFY(RGeometryCandidateService::geometryFingerprint(changed) != initial);
        changed = m; changed.getElement(0).setType(R_ELEMENT_BEAM1);
        QVERIFY(RGeometryCandidateService::geometryFingerprint(changed) != initial);
        changed = m; changed.setElementPhysicalId(0,900);
        QVERIFY(RGeometryCandidateService::geometryFingerprint(changed) != initial);
        changed = m; changed.getNode(2).setZ(2);
        QVERIFY(RGeometryCandidateService::geometryFingerprint(changed) != initial);
        changed = m;
        RNode first = changed.getNode(0);
        changed.getNode(0) = changed.getNode(1);
        changed.getNode(1) = first;
        changed.getElement(0).setNodeId(0,1);
        changed.getElement(0).setNodeId(1,0);
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(changed),initial);
        changed = m; changed.getElement(0).setNodeId(1,1234);
        QVERIFY(RGeometryCandidateService::geometryFingerprint(changed).isEmpty());
    }
    void explicitSupportedElementBoundary()
    {
        RModel point;
        point.setNNodes(1);
        point.setNElements(1);
        RElement p(R_ELEMENT_POINT); p.setNodeId(0,0); point.setElement(0,p);
        QVERIFY(preview(point,transform()).valid);
        RModel unsupported = fixture();
        unsupported.getElement(0).setType(R_ELEMENT_TRUSS2);
        QVERIFY(!preview(unsupported,transform()).valid);
    }
};

QTEST_APPLESS_MAIN(GeometryCandidateTest)
#include "tst_geometry_candidate.moc"
