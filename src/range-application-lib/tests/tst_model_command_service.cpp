#include <QtTest>

#include <limits>
#include <utility>

#include <rmodel_command_service.h>
#include <rmodel_command_session.h>
#include <rml_structural_control_setup.h>

namespace
{
RModel stressLineModel()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.setNNodes(2);
    model.getNode(0).set(0.0, 0.0, 0.0);
    model.getNode(1).set(1.0, 0.0, 0.0);
    model.setNLines(1);
    model.getLine(0).setName(QStringLiteral("Truss"));
    RElement truss(R_ELEMENT_TRUSS1);
    truss.setNodeId(0, 0);
    truss.setNodeId(1, 1);
    model.addElement(truss, true, 0);
    return model;
}

RModel stressPointModel()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.setNNodes(1);
    model.getNode(0).set(0.0, 0.0, 0.0);
    model.setNPoints(1);
    model.getPoint(0).setName(QStringLiteral("Point"));
    RElement point(R_ELEMENT_POINT);
    point.setNodeId(0, 0);
    model.addElement(point, true, 0);
    return model;
}

QJsonObject materialArguments(uint physicalId)
{
    return {{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(physicalId)},
            {QStringLiteral("name"), QStringLiteral("Steel ")},
            {QStringLiteral("youngModulusPa"), 2.0e11},
            {QStringLiteral("densityKgM3"), 8000.0},
            {QStringLiteral("poissonRatio"), 0.3},
            {QStringLiteral("thermalExpansionPerK"), 1.2e-5}};
}

QJsonObject boundaryArguments(uint physicalId, const QString &kind = QStringLiteral("displacement"))
{
    return {{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(physicalId)},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("values"), QJsonArray{0.0, QJsonValue(QJsonValue::Null), 0.0}},
            {QStringLiteral("unit"), kind == QStringLiteral("force") ? QStringLiteral("N") : QStringLiteral("m")}};
}

QJsonObject dynamicsArguments()
{
    return {{QStringLiteral("enabled"), true},
            {QStringLiteral("method"), QStringLiteral("newmark")},
            {QStringLiteral("timeStep"), 0.01}, {QStringLiteral("unitSI"), true},
            {QStringLiteral("steps"), 4}, {QStringLiteral("outputFrequency"), 2},
            {QStringLiteral("rayleighAlpha"), 0.0}, {QStringLiteral("rayleighBeta"), 0.0},
            {QStringLiteral("rhoInfinity"), 1.0}, {QStringLiteral("newmarkGamma"), 0.5},
            {QStringLiteral("newmarkBeta"), 0.25}, {QStringLiteral("modalDampingRatios"), QJsonArray{}}};
}

QJsonObject commandRequest(const RModelCommandSession &session, const QString &command,
                           const QJsonObject &parameters, bool dryRun = false)
{
    const QJsonObject query = session.query();
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
            {QStringLiteral("commandId"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("modelId"), query.value(QStringLiteral("modelId"))},
            {QStringLiteral("expectedRevision"), query.value(QStringLiteral("revision"))},
            {QStringLiteral("requestId"), QUuid::createUuid().toString()},
            {QStringLiteral("idempotencyKey"), QUuid::createUuid().toString()},
            {QStringLiteral("parameters"), parameters}, {QStringLiteral("dryRun"), dryRun}};
}

class TestPointWithPhysicalId : public RPoint
{
public:
    void setPhysicalIdForTest(uint value)
    {
        physicalId = value;
    }
};

class CorruptGroupIdsModel : public RModel
{
public:
    void duplicateCrossTypeGroupId()
    {
        setNPoints(1);
        setNLines(1);
        TestPointWithPhysicalId duplicate;
        duplicate.setPhysicalIdForTest(getLine(0).getPhysicalId());
        getPoint(0) = duplicate;
    }
};
}

class TestModelCommandService : public QObject
{
    Q_OBJECT
private slots:
    void modalPreviewKeepsLegacyShape();
    void structuralDynamicsUsesSecondOrderSetupAndSyncsTimeGrid();
    void materialSetPreservesOtherPropertiesAndUsesPhysicalId();
    void materialRejectsTabulatedReplacementWithoutMutation();
    void boundarySetUsesGlobalXYZAndStrictUnits();
    void targetPagesBindCursorsAndDisambiguateLabels();
    void contextReportsStructuralControlAndPaginatesConditioning();
    void contextMarksInvalidStructuralControlUnavailable();
    void duplicatePhysicalIdsRejectTargetQueries();
    void sessionAcceptUndoRedoAndIdempotencyUseSharedService();
};

void TestModelCommandService::modalPreviewKeepsLegacyShape()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    const uint sourceModes = model.getProblemSetup().getModalSetup().getNModesToExtract();
    QJsonObject arguments{{QStringLiteral("method"), QStringLiteral("multiple")},
                          {QStringLiteral("iterations"), 250}, {QStringLiteral("modes"), 4},
                          {QStringLiteral("tolerance"), 1e-7}, {QStringLiteral("unit"), QStringLiteral("1")}};
    const auto preview = RModelCommandService::preview(model, QStringLiteral("analysis.modal.configure"), arguments,
                                                       QStringLiteral("document-A"), QStringLiteral("r0"));
    QVERIFY2(preview.valid, qPrintable(preview.message));
    QCOMPARE(preview.after.value(QStringLiteral("modes")).toInt(), 4);
    const QJsonArray expectedChanges{QStringLiteral("/problemSetup/modal")};
    QCOMPARE(preview.changedPaths, expectedChanges);
    QVERIFY(!preview.fingerprint.isEmpty());
    QCOMPARE(model.getProblemSetup().getModalSetup().getNModesToExtract(), sourceModes);
    QCOMPARE(preview.candidate.getProblemSetup().getModalSetup().getNModesToExtract(), 4u);
}

void TestModelCommandService::structuralDynamicsUsesSecondOrderSetupAndSyncsTimeGrid()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    RTimeSolver legacy = model.getTimeSolver();
    legacy.setEnabled(true);
    legacy.setInputStartTime(3.0);
    legacy.setInputNTimeSteps(2);
    legacy.setInputTimeStepSize(0.25);
    legacy.setTimes({3.25, 3.5});
    model.setTimeSolver(legacy);

    const auto preview = RModelCommandService::preview(model,
        QStringLiteral("analysis.structural_dynamics.configure"), dynamicsArguments());
    QVERIFY2(preview.valid, qPrintable(preview.message));
    QVERIFY(RModelCommandService::stateFingerprint(model)
            != RModelCommandService::stateFingerprint(preview.candidate));
    QCOMPARE(preview.normalizedArguments.value(QStringLiteral("unitSI")).toBool(), true);
    const QJsonArray expectedChanges{QStringLiteral("/problemSetup/structuralDynamics"), QStringLiteral("/timeSolver")};
    QCOMPARE(preview.changedPaths, expectedChanges);
    QVERIFY(preview.candidate.getProblemSetup().getStructuralDynamicsSetup().getEnabled());
    QCOMPARE(preview.candidate.getProblemSetup().getStructuralDynamicsSetup().getMethod(),
             RStructuralTimeIntegrationMethod::Newmark);
    const RTimeSolver &time = preview.candidate.getTimeSolver();
    QVERIFY(time.getEnabled());
    QCOMPARE(time.getInputStartTime(), 0.0);
    QCOMPARE(time.getInputNTimeSteps(), 4u);
    QCOMPARE(time.getInputTimeStepSize(), 0.01);
    QCOMPARE(time.getTimes().size(), size_t(4));
    QCOMPARE(time.getTimes().back(), 0.04);
    QCOMPARE(model.getProblemSetup().getStructuralDynamicsSetup().getEnabled(), false);
    QCOMPARE(model.getTimeSolver().getInputTimeStepSize(), 0.25);

    QJsonObject invalid = dynamicsArguments();
    invalid.insert(QStringLiteral("unitSI"), false);
    const auto rejected = RModelCommandService::preview(model,
        QStringLiteral("analysis.structural_dynamics.configure"), invalid);
    QVERIFY(!rejected.valid);
    QCOMPARE(rejected.code, QStringLiteral("invalid_parameters"));
    QCOMPARE(model.getTimeSolver().getInputTimeStepSize(), 0.25);

    QJsonObject overflowingGrid = dynamicsArguments();
    overflowingGrid.insert(QStringLiteral("timeStep"), 1.0e308);
    overflowingGrid.insert(QStringLiteral("steps"), 2);
    const QString sourceFingerprint = RModelCommandService::stateFingerprint(model);
    const auto overflowRejected = RModelCommandService::preview(model,
        QStringLiteral("analysis.structural_dynamics.configure"), overflowingGrid);
    QVERIFY(!overflowRejected.valid);
    QCOMPARE(overflowRejected.code, QStringLiteral("unsupported_parameters"));
    QCOMPARE(RModelCommandService::stateFingerprint(model), sourceFingerprint);
    QCOMPARE(model.getProblemSetup().getStructuralDynamicsSetup().getTimeStep(), 0.01);
    QCOMPARE(model.getTimeSolver().getTimes().size(), legacy.getTimes().size());
    QCOMPARE(model.getTimeSolver().getTimes().back(), legacy.getTimes().back());
}

void TestModelCommandService::materialSetPreservesOtherPropertiesAndUsesPhysicalId()
{
    RModel model = stressLineModel();
    RMaterial existing(RMaterial::Solid);
    existing.setName(QStringLiteral("Old"));
    RMaterialProperty damping(RMaterialProperty::AcousticDampingFactor);
    damping.add(293.15, 0.02);
    existing.add(damping);
    model.getLine(0).setMaterial(existing);
    const uint physicalId = model.getLine(0).getPhysicalId();

    const auto preview = RModelCommandService::preview(model, QStringLiteral("entity.material.set"),
                                                        materialArguments(physicalId));
    QVERIFY2(preview.valid, qPrintable(preview.message));
    QVERIFY(RModelCommandService::stateFingerprint(model)
            != RModelCommandService::stateFingerprint(preview.candidate));
    QCOMPARE(preview.normalizedArguments.value(QStringLiteral("name")).toString(), QStringLiteral("Steel"));
    QCOMPARE(preview.normalizedArguments.value(QStringLiteral("referenceTemperatureK")).toDouble(), 293.15);
    QCOMPARE(preview.after.value(QStringLiteral("properties")).toArray().size(), 5);
    QCOMPARE(model.getLine(0).getMaterial().getName(), QStringLiteral("Old"));
    const RMaterial &material = preview.candidate.getLine(0).getMaterial();
    QCOMPARE(material.getName(), QStringLiteral("Steel"));
    QCOMPARE(material.getState(), RMaterial::Solid);
    const uint modulus = material.findPosition(RMaterialProperty::ModulusOfElasiticity);
    QCOMPARE(modulus < material.size(), true);
    QCOMPARE(material.get(modulus).size(), 1u);
    QCOMPARE(material.get(modulus).getKey(0), 293.15);
    QCOMPARE(material.get(modulus).getValue(0), 2.0e11);
    QVERIFY(material.findPosition(RMaterialProperty::AcousticDampingFactor) < material.size());

    const auto staleTarget = RModelCommandService::preview(model, QStringLiteral("entity.material.set"),
                                                            materialArguments(999));
    QVERIFY(!staleTarget.valid);
    QCOMPARE(staleTarget.code, QStringLiteral("stale_target"));

    // The stable target continues to resolve after the group list is reordered.
    model.setNLines(2);
    model.getLine(1).setName(QStringLiteral("Truss"));
    const uint oldFirstId = model.getLine(0).getPhysicalId();
    std::swap(model.getLine(0), model.getLine(1));
    QCOMPARE(model.getLine(1).getPhysicalId(), oldFirstId);
    const auto reordered = RModelCommandService::preview(model, QStringLiteral("entity.material.set"),
                                                          materialArguments(oldFirstId));
    QVERIFY(reordered.valid);
    QCOMPARE(reordered.candidate.getLine(1).getMaterial().getName(), QStringLiteral("Steel"));
}

void TestModelCommandService::materialRejectsTabulatedReplacementWithoutMutation()
{
    RModel model = stressLineModel();
    RMaterial existing(RMaterial::Solid);
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(273.15, 1.9e11);
    modulus.add(373.15, 1.8e11);
    existing.add(modulus);
    model.getLine(0).setMaterial(existing);
    const QJsonObject before = model.getLine(0).getMaterial().toJson();
    const auto preview = RModelCommandService::preview(model, QStringLiteral("entity.material.set"),
                                                        materialArguments(model.getLine(0).getPhysicalId()));
    QVERIFY(!preview.valid);
    QCOMPARE(preview.code, QStringLiteral("tabulated_material"));
    QCOMPARE(model.getLine(0).getMaterial().toJson(), before);
}

void TestModelCommandService::boundarySetUsesGlobalXYZAndStrictUnits()
{
    RModel model = stressPointModel();
    const uint physicalId = model.getPoint(0).getPhysicalId();
    QJsonObject force = boundaryArguments(physicalId, QStringLiteral("force"));
    force.insert(QStringLiteral("values"), QJsonArray{1.0, QJsonValue(QJsonValue::Null), -2.0});
    force.insert(QStringLiteral("timeS"), 0.25);
    const auto preview = RModelCommandService::preview(model, QStringLiteral("entity.boundary.set"), force);
    QVERIFY2(preview.valid, qPrintable(preview.message));
    QVERIFY(RModelCommandService::stateFingerprint(model)
            != RModelCommandService::stateFingerprint(preview.candidate));
    QCOMPARE(preview.normalizedArguments.value(QStringLiteral("unit")).toString(), QStringLiteral("N"));
    QCOMPARE(preview.normalizedArguments.value(QStringLiteral("timeS")).toDouble(), 0.25);
    QCOMPARE(model.getPoint(0).getNBoundaryConditions(), 0u);
    const RBoundaryCondition &condition = preview.candidate.getPoint(0).getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
    const uint forceX = condition.findComponentPosition(R_VARIABLE_FORCE_X);
    const uint forceY = condition.findComponentPosition(R_VARIABLE_FORCE_Y);
    const uint forceZ = condition.findComponentPosition(R_VARIABLE_FORCE_Z);
    QVERIFY(condition.getComponent(forceX).getEnabled());
    QCOMPARE(condition.getComponent(forceX).getKey(0), 0.25);
    QCOMPARE(condition.getComponent(forceX).getValue(0), 1.0);
    QVERIFY(!condition.getComponent(forceY).getEnabled());
    QVERIFY(condition.getComponent(forceZ).getEnabled());
    QCOMPARE(condition.getComponent(forceZ).getValue(0), -2.0);

    QJsonObject badUnit = force;
    badUnit.insert(QStringLiteral("unit"), QStringLiteral("kN"));
    const auto wrongUnit = RModelCommandService::preview(model, QStringLiteral("entity.boundary.set"), badUnit);
    QVERIFY(!wrongUnit.valid);
    QCOMPARE(wrongUnit.code, QStringLiteral("invalid_units"));
    QJsonObject allDisabled = force;
    allDisabled.insert(QStringLiteral("values"), QJsonArray{QJsonValue(QJsonValue::Null),
        QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null)});
    QVERIFY(!RModelCommandService::preview(model, QStringLiteral("entity.boundary.set"), allDisabled).valid);

    RModel lineModel = stressLineModel();
    const auto distributed = RModelCommandService::preview(lineModel, QStringLiteral("entity.boundary.set"),
        boundaryArguments(lineModel.getLine(0).getPhysicalId(), QStringLiteral("force")));
    QVERIFY(!distributed.valid);
    QCOMPARE(distributed.code, QStringLiteral("unsupported_target"));
}

void TestModelCommandService::targetPagesBindCursorsAndDisambiguateLabels()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.setNPoints(3);
    for (uint i = 0; i < 3; ++i) model.getPoint(i).setName(QStringLiteral("Repeated"));
    auto first = RModelCommandService::targets(model, QStringLiteral("doc-A"), QStringLiteral("r1"),
                                                {{QStringLiteral("limit"), 2}});
    QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(first.value(QStringLiteral("targets")).toArray().size(), 2);
    QVERIFY(first.value(QStringLiteral("hasMore")).toBool());
    const QString cursor = first.value(QStringLiteral("nextCursor")).toString();
    QVERIFY(!cursor.isEmpty());
    const auto second = RModelCommandService::targets(model, QStringLiteral("doc-A"), QStringLiteral("r1"),
        {{QStringLiteral("cursor"), cursor}});
    QCOMPARE(second.value(QStringLiteral("targets")).toArray().size(), 1);
    QCOMPARE(second.value(QStringLiteral("targets")).toArray().at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("g:3"));
    QCOMPARE(RModelCommandService::targets(model, QStringLiteral("doc-A"), QStringLiteral("r2"),
        {{QStringLiteral("cursor"), cursor}}).value(QStringLiteral("code")).toString(), QStringLiteral("stale_revision"));
    QCOMPARE(RModelCommandService::targets(model, QStringLiteral("doc-B"), QStringLiteral("r1"),
        {{QStringLiteral("cursor"), cursor}}).value(QStringLiteral("code")).toString(), QStringLiteral("stale_target_cursor"));
    const auto maximumOffset = RModelCommandService::targets(model, QStringLiteral("doc-A"), QStringLiteral("r1"),
        {{QStringLiteral("offset"), std::numeric_limits<int>::max()}, {QStringLiteral("limit"), 200}});
    QCOMPARE(maximumOffset.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(maximumOffset.value(QStringLiteral("offset")).toInt(), std::numeric_limits<int>::max());
    QCOMPARE(maximumOffset.value(QStringLiteral("targets")).toArray().size(), 0);
    QVERIFY(!maximumOffset.value(QStringLiteral("hasMore")).toBool());
    QVERIFY(!maximumOffset.contains(QStringLiteral("nextCursor")));

    const auto duplicates = RModelCommandService::targets(model, QStringLiteral("doc-A"), QStringLiteral("r1"),
        {{QStringLiteral("labelQuery"), QStringLiteral("Repeated")}});
    QVERIFY(duplicates.value(QStringLiteral("ambiguous")).toBool());
    QCOMPARE(duplicates.value(QStringLiteral("candidateIds")).toArray().size(), 3);
    const uint id = model.getPoint(0).getPhysicalId();
    std::swap(model.getPoint(0), model.getPoint(2));
    const auto stable = RModelCommandService::preview(model, QStringLiteral("entity.material.set"), materialArguments(id));
    QVERIFY(stable.valid);
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    QVERIFY(stable.candidate.findElementGroupByPhysicalId(id, &type, &index));
    QCOMPARE(type, R_ENTITY_GROUP_POINT);
    QCOMPARE(stable.candidate.getPoint(index).getMaterial().getName(), QStringLiteral("Steel"));
}

void TestModelCommandService::contextReportsStructuralControlAndPaginatesConditioning()
{
    RModel model = stressLineModel();
    RStructuralControlSetup &control = model.getProblemSetup().getStructuralControlSetup();
    control.enabled = true;
    control.controllerId = QStringLiteral("pid-controller-07");
    control.deterministicSeed = std::numeric_limits<quint64>::max();
    for (uint i = 0; i < 205; ++i)
    {
        RStructuralControlChannelConditioning channel;
        channel.actuatorId = QStringLiteral("actuator-%1").arg(i, 4, 10, QLatin1Char('0'));
        channel.delaySteps = i == 0 ? 7u : i % 7u;
        channel.holdSteps = i == 0 ? 5u : i + 1u;
        channel.noiseAmplitude = i == 0 ? 0.25 : static_cast<double>(i) * 0.001;
        control.channelConditioning.push_back(channel);
    }
    const RStructuralControlSetup expected = control;

    const QJsonObject first = RModelCommandService::context(model, QStringLiteral("doc-A"),
        QStringLiteral("r1"), {{QStringLiteral("limit"), 200}});
    QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject configuration = first.value(QStringLiteral("configuration")).toObject();
    const QJsonObject reported = configuration.value(QStringLiteral("structuralControl")).toObject();
    QVERIFY(reported.value(QStringLiteral("available")).toBool());
    QCOMPARE(reported.value(QStringLiteral("enabled")).toBool(), expected.enabled);
    QCOMPARE(reported.value(QStringLiteral("controlProtocolVersion")).toInt(),
             static_cast<int>(expected.controlProtocolVersion));
    QCOMPARE(reported.value(QStringLiteral("controllerId")).toString(), expected.controllerId);
    QCOMPARE(reported.value(QStringLiteral("deterministicSeed")).toString(),
             QString::number(static_cast<qulonglong>(expected.deterministicSeed)));

    const QJsonObject firstPage = reported.value(QStringLiteral("conditioningPage")).toObject();
    QCOMPARE(firstPage.value(QStringLiteral("total")).toInt(), 205);
    QCOMPARE(firstPage.value(QStringLiteral("offset")).toInt(), 0);
    QCOMPARE(firstPage.value(QStringLiteral("limit")).toInt(), 200);
    QVERIFY(firstPage.value(QStringLiteral("hasMore")).toBool());
    const QJsonArray firstChannels = firstPage.value(QStringLiteral("channels")).toArray();
    QCOMPARE(firstChannels.size(), 200);
    const QJsonObject firstChannel = firstChannels.first().toObject();
    QCOMPARE(firstChannel.value(QStringLiteral("actuatorId")).toString(), QStringLiteral("actuator-0000"));
    QCOMPARE(firstChannel.value(QStringLiteral("delaySteps")).toInt(), 7);
    QCOMPARE(firstChannel.value(QStringLiteral("holdSteps")).toInt(), 5);
    QCOMPARE(firstChannel.value(QStringLiteral("noiseAmplitudeN")).toDouble(), 0.25);
    QVERIFY(firstPage.value(QStringLiteral("nextCursor")).isString());
    QCOMPARE(first.value(QStringLiteral("targetPage")).toObject().value(QStringLiteral("total")).toInt(), 1);

    const QString cursor = firstPage.value(QStringLiteral("nextCursor")).toString();
    const QJsonObject second = RModelCommandService::context(model, QStringLiteral("doc-A"),
        QStringLiteral("r1"), {{QStringLiteral("cursor"), cursor}});
    const QJsonObject secondControl = second.value(QStringLiteral("configuration")).toObject()
        .value(QStringLiteral("structuralControl")).toObject();
    const QJsonObject secondPage = secondControl.value(QStringLiteral("conditioningPage")).toObject();
    QCOMPARE(secondPage.value(QStringLiteral("offset")).toInt(), 200);
    QCOMPARE(secondPage.value(QStringLiteral("total")).toInt(), 205);
    QCOMPARE(secondPage.value(QStringLiteral("channels")).toArray().size(), 5);
    QVERIFY(!secondPage.value(QStringLiteral("hasMore")).toBool());
    QVERIFY(!secondPage.contains(QStringLiteral("nextCursor")));
    QCOMPARE(secondPage.value(QStringLiteral("channels")).toArray().last().toObject()
                 .value(QStringLiteral("actuatorId")).toString(), QStringLiteral("actuator-0204"));
    QCOMPARE(second.value(QStringLiteral("targetPage")).toObject().value(QStringLiteral("targets")).toArray().size(), 0);

    QCOMPARE(RModelCommandService::context(model, QStringLiteral("doc-A"), QStringLiteral("r2"),
        {{QStringLiteral("cursor"), cursor}}).value(QStringLiteral("code")).toString(),
        QStringLiteral("stale_revision"));
    QCOMPARE(RModelCommandService::context(model, QStringLiteral("doc-B"), QStringLiteral("r1"),
        {{QStringLiteral("cursor"), cursor}}).value(QStringLiteral("code")).toString(),
        QStringLiteral("stale_target_cursor"));

    const QJsonObject actuatorQuery = RModelCommandService::context(model, QStringLiteral("doc-A"),
        QStringLiteral("r1"), {{QStringLiteral("labelQuery"), QStringLiteral("actuator-020")}});
    const QJsonObject actuatorControl = actuatorQuery.value(QStringLiteral("configuration")).toObject()
        .value(QStringLiteral("structuralControl")).toObject();
    QCOMPARE(actuatorQuery.value(QStringLiteral("targetPage")).toObject().value(QStringLiteral("total")).toInt(), 0);
    QCOMPARE(actuatorControl.value(QStringLiteral("conditioningPage")).toObject()
                 .value(QStringLiteral("total")).toInt(), 5);
    const QJsonObject groupQuery = RModelCommandService::context(model, QStringLiteral("doc-A"),
        QStringLiteral("r1"), {{QStringLiteral("labelQuery"), QStringLiteral("Truss")}});
    QCOMPARE(groupQuery.value(QStringLiteral("targetPage")).toObject().value(QStringLiteral("total")).toInt(), 1);
    QCOMPARE(groupQuery.value(QStringLiteral("configuration")).toObject()
                 .value(QStringLiteral("structuralControl")).toObject()
                 .value(QStringLiteral("conditioningPage")).toObject()
                 .value(QStringLiteral("total")).toInt(), 0);

    const QJsonObject maximumOffset = RModelCommandService::context(model, QStringLiteral("doc-A"),
        QStringLiteral("r1"), {{QStringLiteral("offset"), std::numeric_limits<int>::max()},
                                {QStringLiteral("limit"), 200}});
    const QJsonObject maximumOffsetPage = maximumOffset.value(QStringLiteral("configuration")).toObject()
        .value(QStringLiteral("structuralControl")).toObject()
        .value(QStringLiteral("conditioningPage")).toObject();
    QCOMPARE(maximumOffsetPage.value(QStringLiteral("offset")).toInt(), std::numeric_limits<int>::max());
    QCOMPARE(maximumOffsetPage.value(QStringLiteral("channels")).toArray().size(), 0);
    QVERIFY(!maximumOffsetPage.value(QStringLiteral("hasMore")).toBool());
    QVERIFY(!maximumOffsetPage.contains(QStringLiteral("nextCursor")));

    const RStructuralControlSetup &preserved = model.getProblemSetup().getStructuralControlSetup();
    QCOMPARE(preserved.enabled, expected.enabled);
    QCOMPARE(preserved.controlProtocolVersion, expected.controlProtocolVersion);
    QCOMPARE(preserved.controllerId, expected.controllerId);
    QCOMPARE(preserved.deterministicSeed, expected.deterministicSeed);
    QCOMPARE(preserved.channelConditioning.size(), expected.channelConditioning.size());
    for (std::size_t i = 0; i < expected.channelConditioning.size(); ++i)
    {
        QCOMPARE(preserved.channelConditioning[i].actuatorId, expected.channelConditioning[i].actuatorId);
        QCOMPARE(preserved.channelConditioning[i].delaySteps, expected.channelConditioning[i].delaySteps);
        QCOMPARE(preserved.channelConditioning[i].holdSteps, expected.channelConditioning[i].holdSteps);
        QCOMPARE(preserved.channelConditioning[i].noiseAmplitude,
                 expected.channelConditioning[i].noiseAmplitude);
    }
}

void TestModelCommandService::contextMarksInvalidStructuralControlUnavailable()
{
    RModel model = stressPointModel();
    RStructuralControlSetup &control = model.getProblemSetup().getStructuralControlSetup();
    control.enabled = true;
    control.controllerId = QStringLiteral("native-controller-09");
    control.deterministicSeed = quint64(9007199254740993ULL);
    control.channelConditioning.resize(4097);

    const QJsonObject context = RModelCommandService::context(model, QStringLiteral("doc-A"), QStringLiteral("r1"));
    QCOMPARE(context.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject reported = context.value(QStringLiteral("configuration")).toObject()
        .value(QStringLiteral("structuralControl")).toObject();
    QVERIFY(!reported.value(QStringLiteral("available")).toBool());
    QCOMPARE(reported.value(QStringLiteral("enabled")).toBool(), true);
    QCOMPARE(reported.value(QStringLiteral("controllerId")).toString(), control.controllerId);
    QCOMPARE(reported.value(QStringLiteral("deterministicSeed")).toString(), QStringLiteral("9007199254740993"));
    QVERIFY(!reported.contains(QStringLiteral("conditioningPage")));
    const QJsonArray diagnostics = reported.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("code")).toString(),
             QStringLiteral("invalid_structural_control_setup"));
    QVERIFY(diagnostics.first().toObject().value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("4096")));

    const RStructuralControlSetup &preserved = model.getProblemSetup().getStructuralControlSetup();
    QCOMPARE(preserved.enabled, true);
    QCOMPARE(preserved.controllerId, QStringLiteral("native-controller-09"));
    QCOMPARE(preserved.deterministicSeed, quint64(9007199254740993ULL));
    QCOMPARE(preserved.channelConditioning.size(), size_t(4097));

    RModel oversizedIdModel = stressPointModel();
    RStructuralControlSetup &oversizedIdControl =
        oversizedIdModel.getProblemSetup().getStructuralControlSetup();
    oversizedIdControl.enabled = true;
    oversizedIdControl.controlProtocolVersion = 7;
    oversizedIdControl.controllerId = QString(129, QLatin1Char('x'));
    oversizedIdControl.deterministicSeed = quint64(9007199254740993ULL);
    const QJsonObject oversizedIdContext = RModelCommandService::context(
        oversizedIdModel, QStringLiteral("doc-A"), QStringLiteral("r1"));
    const QJsonObject oversizedIdReported = oversizedIdContext.value(QStringLiteral("configuration")).toObject()
        .value(QStringLiteral("structuralControl")).toObject();
    QVERIFY(!oversizedIdReported.value(QStringLiteral("available")).toBool());
    QVERIFY(!oversizedIdReported.contains(QStringLiteral("controllerId")));
    QCOMPARE(oversizedIdReported.value(QStringLiteral("enabled")).toBool(), true);
    QCOMPARE(oversizedIdReported.value(QStringLiteral("controlProtocolVersion")).toInt(), 7);
    QCOMPARE(oversizedIdReported.value(QStringLiteral("deterministicSeed")).toString(),
             QStringLiteral("9007199254740993"));
    QVERIFY(oversizedIdReported.value(QStringLiteral("diagnostics")).toArray().first().toObject()
                .value(QStringLiteral("message")).toString().contains(QStringLiteral("128")));
    const RStructuralControlSetup &oversizedIdPreserved =
        oversizedIdModel.getProblemSetup().getStructuralControlSetup();
    QCOMPARE(oversizedIdPreserved.controllerId, QString(129, QLatin1Char('x')));
    QCOMPARE(oversizedIdPreserved.deterministicSeed, quint64(9007199254740993ULL));
}

void TestModelCommandService::duplicatePhysicalIdsRejectTargetQueries()
{
    CorruptGroupIdsModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.duplicateCrossTypeGroupId();
    const auto page = RModelCommandService::targets(model, QStringLiteral("doc-A"), QStringLiteral("r1"));
    QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    QCOMPARE(page.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_model"));
    const auto preview = RModelCommandService::preview(model, QStringLiteral("entity.material.set"), materialArguments(1));
    QVERIFY(!preview.valid);
    QCOMPARE(preview.code, QStringLiteral("invalid_model"));
}

void TestModelCommandService::sessionAcceptUndoRedoAndIdempotencyUseSharedService()
{
    RModelCommandSession session(stressLineModel());
    const uint physicalId = session.model().getLine(0).getPhysicalId();
    QJsonObject command = commandRequest(session, QStringLiteral("entity.material.set"), materialArguments(physicalId), true);
    const QJsonObject before = session.query();
    QCOMPARE(session.execute(command).value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
    QCOMPARE(session.query(), before);
    command.insert(QStringLiteral("dryRun"), false);
    const QJsonObject accepted = session.execute(command);
    QCOMPARE(accepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QVERIFY(accepted.contains(QStringLiteral("normalizedParameters")));
    QCOMPARE(accepted.value(QStringLiteral("normalizedParameters")).toObject()
                 .value(QStringLiteral("referenceTemperatureK")).toDouble(), 293.15);
    QCOMPARE(session.model().getLine(0).getMaterial().getName(), QStringLiteral("Steel"));
    QCOMPARE(session.execute(command), accepted);
    const QJsonObject boundary = session.execute(commandRequest(session, QStringLiteral("entity.boundary.set"),
        boundaryArguments(physicalId)));
    QCOMPARE(boundary.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QVERIFY(session.model().getLine(0).hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
    QCOMPARE(session.execute(commandRequest(session, QStringLiteral("model.undo"), {}))
                 .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QVERIFY(!session.model().getLine(0).hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
    QCOMPARE(session.execute(commandRequest(session, QStringLiteral("model.undo"), {}))
                 .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QCOMPARE(session.model().getLine(0).getMaterial().getName(), QString());
    QCOMPARE(session.execute(commandRequest(session, QStringLiteral("model.redo"), {}))
                 .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QCOMPARE(session.model().getLine(0).getMaterial().getName(), QStringLiteral("Steel"));
    QCOMPARE(session.execute(commandRequest(session, QStringLiteral("model.redo"), {}))
                 .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QVERIFY(session.model().getLine(0).hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
}

QTEST_GUILESS_MAIN(TestModelCommandService)
#include "tst_model_command_service.moc"
