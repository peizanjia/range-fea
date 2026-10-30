#include <QtTest>

#include <limits>

#include <rtrusted_mechanics_service.h>

#include <rml_element.h>
#include <rml_boundary_condition.h>
#include <rml_condition_component.h>
#include <rml_element_descriptor.h>
#include <rml_environment_condition.h>
#include <rml_material.h>
#include <rml_material_property.h>
#include <rml_modal_setup.h>
#include <rml_model.h>
#include <rml_point.h>
#include <rml_structural_dynamics_setup.h>
#include <rml_surface.h>

namespace
{
QJsonObject q(double value, const QString &unit)
{
    return {{"value", value}, {"unit", unit}};
}

QJsonArray force(double x, double y, double z, const QString &unit = QStringLiteral("N"))
{
    return QJsonArray{q(x, unit), q(y, unit), q(z, unit)};
}

QJsonObject rodSpec()
{
    return {
        {"schemaVersion", 1}, {"modelType", "axialRod"}, {"name", "1 m rod"},
        {"geometry", QJsonObject{{"length", q(1000.0, "mm")}, {"segments", 4}}},
        {"material", QJsonObject{{"name", "Steel"}, {"source", "test fixture"},
            {"youngModulus", q(210.0, "GPa")}, {"poissonRatio", 0.3},
            {"density", q(7850.0, "kg/m3")}}},
        {"section", QJsonObject{{"area", q(100.0, "mm2")}}},
        {"analysis", QJsonObject{{"kind", "static"}}},
        {"supports", QJsonArray{
            QJsonObject{{"selector", QJsonObject{{"kind", "fixedEnd"}}}, {"dofs", QJsonArray{"ux", "uy", "uz"}}},
            QJsonObject{{"selector", QJsonObject{{"kind", "allNodes"}}}, {"dofs", QJsonArray{"uy", "uz"}}}}},
        {"loads", QJsonArray{QJsonObject{{"kind", "pointForce"},
            {"selector", QJsonObject{{"kind", "freeEnd"}}}, {"components", force(1000.0, 0.0, 0.0)}}}},
        {"initialConditions", QJsonArray{}},
        {"observables", QJsonArray{QJsonObject{{"selector", QJsonObject{{"kind", "freeEnd"}}},
            {"quantity", "displacement"}, {"component", "ux"}}}},
        {"precision", QJsonObject{{"displacementRelative", 0.01}}}
    };
}

QJsonObject beamSpec()
{
    QJsonObject spec = rodSpec();
    spec.insert("modelType", "cantileverBeam");
    spec.insert("name", "1 m cantilever");
    spec.insert("section", QJsonObject{{"area", q(100.0, "mm2")},
        {"iy", q(8.333333333333333e-10, "m4")}, {"iz", q(8.333333333333333e-10, "m4")},
        {"torsionalConstant", q(1.666666666666667e-9, "m4")},
        {"wy", q(1.666666666666667e-7, "m3")}, {"wz", q(1.666666666666667e-7, "m3")},
        {"wt", q(1.666666666666667e-7, "m3")}, {"localY", QJsonArray{0.0, 1.0, 0.0}}});
    spec.insert("supports", QJsonArray{QJsonObject{{"selector", QJsonObject{{"kind", "fixedEnd"}}},
        {"dofs", QJsonArray{"ux", "uy", "uz", "rx", "ry", "rz"}}}});
    QJsonArray loads{QJsonObject{{"kind", "pointForce"}, {"selector", QJsonObject{{"kind", "freeEnd"}}},
        {"components", force(0.0, 0.0, -100.0)}}};
    spec.insert("loads", loads);
    spec.insert("observables", QJsonArray{QJsonObject{{"selector", QJsonObject{{"kind", "freeEnd"}}},
        {"quantity", "displacement"}, {"component", "uz"}}});
    return spec;
}

bool hasCode(const QJsonArray &issues, const QString &code)
{
    for (const QJsonValue &value : issues)
        if (value.toObject().value("code").toString() == code) return true;
    return false;
}

bool hasIssueAtPath(const QJsonArray &issues, const QString &code, const QString &path)
{
    for (const QJsonValue &value : issues) {
        const QJsonObject issue = value.toObject();
        if (issue.value(QStringLiteral("code")).toString() == code
            && issue.value(QStringLiteral("path")).toString() == path)
            return true;
    }
    return false;
}

QJsonObject issueByCode(const QJsonArray &issues, const QString &code)
{
    for (const QJsonValue &value : issues)
        if (value.toObject().value("code").toString() == code) return value.toObject();
    return {};
}

RModel degenerateUnmaterialedRod()
{
    RModel model;
    model.setNNodes(2);
    model.getNode(0).set(0.0, 0.0, 0.0);
    model.getNode(1).set(0.0, 0.0, 0.0);
    RElement rod(R_ELEMENT_TRUSS1);
    rod.setNodeId(0, 0);
    rod.setNodeId(1, 1);
    model.addElement(rod, true, 0);
    return model;
}
}

class TrustedMechanicsServiceTest : public QObject
{
    Q_OBJECT
private slots:
    void validatesAndNormalizesSI();
    void rejectsWrongUnitsAndMissingMaterial();
    void rejectsWrongTypeProblemSpecArrays();
    void createsRepeatableRodAndBeamCandidates();
    void rejectsUnresolvedAndNonsinglePointLoads();
    void rejectsTransverseAxialRodInputs();
    void separatesPhysicalAndNumericalSummaries();
    void resolvesSemanticSelectorsAgainstCurrentRevision();
    void reportsUnderconstraintWithoutChangingPhysics();
    void diagnosesZeroExcitationAndInvalidSettings();
    void diagnosesDegenerateElementsAndMissingMaterial();
    void reconcilesModelStateWithProblemSpec();
    void readsLiveBoundaryConditionLocalDirection();
    void readsLiveDynamicsAndModalSettings();
    void readsLivePointAndSurfaceProperties();
    void diagnosesDisconnectedFreeBeamComponent();
    void diagnosesDisconnectedFreeRodComponent();
    void diagnosesLineGroupMembershipMismatch();
    void rejectsUnsupportedModelElementsAndBoundaryConditions();
    void diagnosesUnsupportedEnvironmentConditions();
    void acceptsEquivalentEndpointSelectorAndRejectsRodRotations();
    void bindsStaleResultsAndUnknownFailureEvidence();
};

void TrustedMechanicsServiceTest::validatesAndNormalizesSI()
{
    const auto result = RTrustedMechanicsService::validate(rodSpec());
    QVERIFY(result.valid);
    QCOMPARE(result.normalizedSpec.value("geometry").toObject().value("length").toDouble(), 1.0);
    QCOMPARE(result.normalizedSpec.value("material").toObject().value("youngModulus").toDouble(), 210.0e9);
    QCOMPARE(result.normalizedSpec.value("section").toObject().value("area").toDouble(), 1.0e-4);
    QCOMPARE(result.normalizedSpec.value("loads").toArray().first().toObject()
                 .value("components").toArray().first().toDouble(), 1000.0);
}

void TrustedMechanicsServiceTest::rejectsWrongUnitsAndMissingMaterial()
{
    QJsonObject badUnit = rodSpec();
    QJsonObject material = badUnit.value("material").toObject();
    material.insert("youngModulus", q(210.0, "m"));
    badUnit.insert("material", material);
    const auto wrongUnitResult = RTrustedMechanicsService::validate(badUnit);
    QVERIFY(!wrongUnitResult.valid);
    QVERIFY(hasCode(wrongUnitResult.issues, QStringLiteral("UNIT_DIMENSION_MISMATCH")));

    QJsonObject missingMaterial = rodSpec();
    material = missingMaterial.value("material").toObject();
    material.remove("density");
    missingMaterial.insert("material", material);
    const auto missingResult = RTrustedMechanicsService::validate(missingMaterial);
    QVERIFY(!missingResult.valid);
    QVERIFY(hasCode(missingResult.issues, QStringLiteral("SPEC_MISSING_QUANTITY")));

    QJsonObject unsupportedPhysics = rodSpec();
    material = unsupportedPhysics.value("material").toObject();
    material.insert("plasticity", true);
    unsupportedPhysics.insert("material", material);
    const auto unsupportedResult = RTrustedMechanicsService::validate(unsupportedPhysics);
    QVERIFY(!unsupportedResult.valid);
    QVERIFY(hasCode(unsupportedResult.issues, QStringLiteral("SPEC_FIELD_UNSUPPORTED")));
}

void TrustedMechanicsServiceTest::rejectsWrongTypeProblemSpecArrays()
{
    QJsonObject transient = rodSpec();
    transient.insert(QStringLiteral("analysis"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("linearTransient")},
        {QStringLiteral("timeStep"), q(0.1, QStringLiteral("s"))},
        {QStringLiteral("steps"), 1},
        {QStringLiteral("damping"), QJsonObject{
            {QStringLiteral("rayleighAlpha"), q(0.0, QStringLiteral("1/s"))},
            {QStringLiteral("rayleighBeta"), q(0.0, QStringLiteral("s"))}}}});
    transient.insert(QStringLiteral("initialConditions"), QJsonArray{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("displacement")},
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("components"), force(0.0, 0.0, 0.0, QStringLiteral("m"))}}});
    QJsonArray loads = transient.value(QStringLiteral("loads")).toArray();
    QJsonObject load = loads.first().toObject();
    load.insert(QStringLiteral("history"), QJsonObject{{QStringLiteral("time"), 0.0}});
    loads[0] = load;
    transient.insert(QStringLiteral("loads"), loads);
    transient.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("responseRelative"), 0.01}});
    const auto badHistory = RTrustedMechanicsService::validate(transient);
    QVERIFY(!badHistory.valid);
    QVERIFY(hasIssueAtPath(badHistory.issues, QStringLiteral("SPEC_ARRAY_TYPE_INVALID"),
                           QStringLiteral("loads[0].history")));
    QVERIFY(badHistory.normalizedSpec.isEmpty());

    QJsonObject modal = rodSpec();
    modal.insert(QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("modal")},
                                                          {QStringLiteral("modes"), 1}});
    modal.insert(QStringLiteral("loads"), QJsonObject{{QStringLiteral("not"), QStringLiteral("an array")}});
    modal.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("frequencyRelative"), 0.02}});
    const auto badLoads = RTrustedMechanicsService::validate(modal);
    QVERIFY(!badLoads.valid);
    QVERIFY(hasIssueAtPath(badLoads.issues, QStringLiteral("SPEC_ARRAY_TYPE_INVALID"), QStringLiteral("loads")));
    QVERIFY(badLoads.normalizedSpec.isEmpty());
}

void TrustedMechanicsServiceTest::createsRepeatableRodAndBeamCandidates()
{
    const auto rodA = RTrustedMechanicsService::createCandidate(rodSpec(), "source-A", "rev-1");
    const auto rodB = RTrustedMechanicsService::createCandidate(rodSpec(), "source-A", "rev-1");
    QVERIFY2(rodA.valid, qPrintable(rodA.message));
    QVERIFY(rodB.valid);
    QCOMPARE(rodA.model.getNNodes(), 5u);
    QCOMPARE(rodA.model.getNElements(), 6u); // four line elements plus the support and load point elements
    QCOMPARE(rodA.inspection.value("model"), rodB.inspection.value("model"));
    QCOMPARE(rodA.inspection.value("source").toObject().value("modelFingerprint"),
             rodB.inspection.value("source").toObject().value("modelFingerprint"));
    QCOMPARE(rodA.inspection.value("model").toObject().value("lines").toArray().first().toObject()
                 .value("material").toObject().value("properties").toArray().size(), 4);
    const QJsonObject rodLoadGroup = rodA.inspection.value("model").toObject().value("points").toArray().at(1).toObject();
    const QJsonObject rodForceX = rodLoadGroup.value("boundaryConditions").toArray().first().toObject()
        .value("components").toArray().first().toObject();
    QCOMPARE(rodForceX.value("unit").toString(), QStringLiteral("N"));
    QCOMPARE(rodForceX.value("samples").toArray().first().toObject().value("value").toDouble(), 1000.0);

    const auto beam = RTrustedMechanicsService::createCandidate(beamSpec(), "source-B", "rev-3");
    QVERIFY2(beam.valid, qPrintable(beam.message));
    QCOMPARE(beam.model.getNNodes(), 5u);
    QCOMPARE(beam.model.getElement(0).getType(), R_ELEMENT_BEAM1);
    QCOMPARE(beam.model.getLine(0).getSecondMomentY(), 8.333333333333333e-10);
    QCOMPARE(beam.inspection.value("model").toObject().value("points").toArray().size(), 2);
    QCOMPARE(beam.inspection.value("model").toObject().value("points").toArray().first().toObject()
                 .value("boundaryConditions").toArray().size(), 2);

    QJsonObject modalSpec = rodSpec();
    modalSpec.insert("analysis", QJsonObject{{"kind", "modal"}, {"modes", 3}});
    modalSpec.insert("loads", QJsonArray{});
    modalSpec.insert("precision", QJsonObject{{"frequencyRelative", 0.02}});
    const auto modal = RTrustedMechanicsService::createCandidate(modalSpec, "source-M", "rev-4");
    QVERIFY2(modal.valid, qPrintable(modal.message));
    QCOMPARE(modal.model.getProblemTaskTree().getProblemType(), R_PROBLEM_STRESS_MODAL);
    QCOMPARE(modal.model.getProblemSetup().getModalSetup().getNModesToExtract(), 3u);
    const QJsonObject modalReport = RTrustedMechanicsService::diagnose(
        modalSpec, &modal.model, "source-M", "rev-4");
    QVERIFY(!hasCode(modalReport.value("issues").toArray(), "MODEL_ANALYSIS_SPEC_MISMATCH"));

    QJsonObject beamModalSpec = beamSpec();
    beamModalSpec.insert("analysis", QJsonObject{{"kind", "modal"}, {"modes", 3}});
    beamModalSpec.insert("loads", QJsonArray{});
    beamModalSpec.insert("precision", QJsonObject{{"frequencyRelative", 0.02}});
    const auto beamModal = RTrustedMechanicsService::createCandidate(beamModalSpec, "source-BM", "rev-5");
    QVERIFY2(beamModal.valid, qPrintable(beamModal.message));
    const uint alphaPosition = beamModal.model.getLine(0).getMaterial().findPosition(
        RMaterialProperty::ThermalExpansionCoefficient);
    QVERIFY(alphaPosition < beamModal.model.getLine(0).getMaterial().size());
    const RMaterialProperty &alpha = beamModal.model.getLine(0).getMaterial().get(alphaPosition);
    QCOMPARE(alpha.size(), 1u);
    QCOMPARE(alpha.getKey(0), 293.15);
    QCOMPARE(alpha.getValue(0), 0.0);
    const QJsonObject beamModalReport = RTrustedMechanicsService::diagnose(
        beamModalSpec, &beamModal.model, "source-BM", "rev-5");
    QVERIFY(!hasCode(beamModalReport.value("issues").toArray(), "MATERIAL_SPEC_MISMATCH"));
    QVERIFY(beamModal.inspection.value("model").toObject().value("solverCompatibilityDefaults")
                .toArray().first().toObject().value("property").toString()
            == QStringLiteral("thermalExpansionCoefficient"));

    RModel nonzeroAlpha = beamModal.model;
    const uint nonzeroAlphaPosition = nonzeroAlpha.getLine(0).getMaterial().findPosition(
        RMaterialProperty::ThermalExpansionCoefficient);
    RMaterialProperty &nonzeroAlphaProperty = nonzeroAlpha.getLine(0).getMaterial().get(nonzeroAlphaPosition);
    nonzeroAlphaProperty.clear();
    nonzeroAlphaProperty.add(293.15, 1.0e-5);
    const QJsonObject nonzeroAlphaReport = RTrustedMechanicsService::diagnose(
        beamModalSpec, &nonzeroAlpha, "source-BM", "rev-5");
    QVERIFY(hasCode(nonzeroAlphaReport.value("issues").toArray(), "MATERIAL_SPEC_MISMATCH"));
}

void TrustedMechanicsServiceTest::separatesPhysicalAndNumericalSummaries()
{
    QJsonObject coarse = rodSpec();
    QJsonObject geometry = coarse.value("geometry").toObject();
    geometry.insert("segments", 4);
    coarse.insert("geometry", geometry);
    QJsonObject transient = coarse.value("analysis").toObject();
    transient.insert("kind", "linearTransient");
    transient.insert("timeStep", q(10.0, "ms"));
    transient.insert("steps", 100);
    transient.insert("damping", QJsonObject{{"rayleighAlpha", q(0.0, "1/s")},
                                              {"rayleighBeta", q(0.0, "s")}});
    coarse.insert("analysis", transient);
    coarse.insert("precision", QJsonObject{{"responseRelative", 0.01}});
    coarse.insert("initialConditions", QJsonArray{QJsonObject{{"type", "displacement"},
        {"selector", QJsonObject{{"kind", "allNodes"}}}, {"components", force(0.0, 0.0, 0.0, "m")}}});

    QJsonObject fine = coarse;
    geometry = fine.value("geometry").toObject();
    geometry.insert("segments", 8);
    fine.insert("geometry", geometry);
    transient = fine.value("analysis").toObject();
    transient.insert("timeStep", q(5.0, "ms"));
    transient.insert("steps", 200);
    fine.insert("analysis", transient);

    const auto coarseResult = RTrustedMechanicsService::validate(coarse);
    const auto fineResult = RTrustedMechanicsService::validate(fine);
    QVERIFY2(coarseResult.valid, qPrintable(QJsonDocument(coarseResult.issues).toJson()));
    QVERIFY2(fineResult.valid, qPrintable(QJsonDocument(fineResult.issues).toJson()));
    QCOMPARE(RTrustedMechanicsService::summaryDigest(
                 RTrustedMechanicsService::physicalSummary(coarseResult.normalizedSpec)),
             RTrustedMechanicsService::summaryDigest(
                 RTrustedMechanicsService::physicalSummary(fineResult.normalizedSpec)));
    QVERIFY(RTrustedMechanicsService::summaryDigest(
                RTrustedMechanicsService::numericalSummary(coarseResult.normalizedSpec))
            != RTrustedMechanicsService::summaryDigest(
                RTrustedMechanicsService::numericalSummary(fineResult.normalizedSpec)));

    QJsonObject damped = coarse;
    transient = damped.value("analysis").toObject();
    QJsonObject damping = transient.value("damping").toObject();
    damping.insert("rayleighAlpha", q(0.1, "1/s"));
    transient.insert("damping", damping);
    damped.insert("analysis", transient);
    const auto dampedResult = RTrustedMechanicsService::validate(damped);
    QVERIFY(dampedResult.valid);
    QVERIFY(RTrustedMechanicsService::summaryDigest(
                RTrustedMechanicsService::physicalSummary(coarseResult.normalizedSpec))
            != RTrustedMechanicsService::summaryDigest(
                RTrustedMechanicsService::physicalSummary(dampedResult.normalizedSpec)));

    const auto transientCandidate = RTrustedMechanicsService::createCandidate(coarse, "source-T", "rev-4");
    QVERIFY2(transientCandidate.valid, qPrintable(transientCandidate.message));
    QCOMPARE(transientCandidate.model.getProblemSetup().getStructuralDynamicsSetup().getRayleighAlpha(), 0.0);
    QCOMPARE(transientCandidate.inspection.value("model").toObject().value("analysis").toObject()
                 .value("structuralDynamics").toObject().value("rayleighAlpha_1_s").toDouble(), 0.0);
    const QJsonArray transientPoints = transientCandidate.inspection.value("model").toObject().value("points").toArray();
    QCOMPARE(transientPoints.size(), 7);
    const QJsonArray initialConditions = transientPoints.last().toObject().value("initialConditions").toArray();
    QCOMPARE(initialConditions.size(), 1);
    QCOMPARE(initialConditions.first().toObject().value("components").toArray().first().toObject()
                 .value("samples").toArray().first().toObject().value("value").toDouble(), 0.0);
}

void TrustedMechanicsServiceTest::resolvesSemanticSelectorsAgainstCurrentRevision()
{
    const auto coarse = RTrustedMechanicsService::createCandidate(rodSpec(), "source-A", "rev-1");
    QVERIFY(coarse.valid);
    const QJsonObject freeEnd{{"kind", "freeEnd"}};
    const auto first = RTrustedMechanicsService::resolveSelector(coarse.model, freeEnd, "rev-1", "rev-1");
    QVERIFY(first.value("valid").toBool());
    QCOMPARE(first.value("nodePositions").toArray().first().toInt(), 4);
    QVERIFY(!RTrustedMechanicsService::resolveSelector(coarse.model, freeEnd, "rev-0", "rev-1")
                 .value("valid").toBool());

    QJsonObject refined = rodSpec();
    QJsonObject geometry = refined.value("geometry").toObject();
    geometry.insert("segments", 8);
    refined.insert("geometry", geometry);
    const auto fine = RTrustedMechanicsService::createCandidate(refined, "source-A", "rev-1");
    QVERIFY(fine.valid);
    const auto rebound = RTrustedMechanicsService::resolveSelector(fine.model, freeEnd, "rev-1", "rev-1");
    QVERIFY(rebound.value("valid").toBool());
    QCOMPARE(rebound.value("nodePositions").toArray().first().toInt(), 8);
    QCOMPARE(rebound.value("physicalIds").toArray().first(),
             QJsonValue(double(fine.model.getNodePhysicalId(8))));
}

void TrustedMechanicsServiceTest::reportsUnderconstraintWithoutChangingPhysics()
{
    QJsonObject spec = rodSpec();
    QJsonArray supports = spec.value("supports").toArray();
    QJsonObject distributed = supports.at(1).toObject();
    distributed.insert("dofs", QJsonArray{"uz"});
    supports[1] = distributed;
    spec.insert("supports", supports);
    const QJsonObject before = RTrustedMechanicsService::physicalSummary(
        RTrustedMechanicsService::validate(rodSpec()).normalizedSpec);
    const QJsonObject report = RTrustedMechanicsService::diagnose(spec, nullptr, "source-A", "rev-1");
    const QJsonObject issue = issueByCode(report.value("issues").toArray(), "BC_UNDERCONSTRAINED");
    QVERIFY(!issue.isEmpty());
    QVERIFY(issue.value("requiresUserConfirmation").toBool());
    QCOMPARE(issue.value("evidence").toObject().value("automaticPhysicalEdit").toBool(), false);
    QVERIFY(issue.value("evidence").toObject().value("missingDirections").toArray().contains("uy"));
    QVERIFY(issue.value("allowedOperations").toArray().contains("proposeSupportChange"));
    QCOMPARE(RTrustedMechanicsService::summaryDigest(before), RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::physicalSummary(RTrustedMechanicsService::validate(rodSpec()).normalizedSpec)));

    const auto validRod = RTrustedMechanicsService::createCandidate(rodSpec(), "source-A", "rev-1");
    QVERIFY(validRod.valid);
    RModel unsupportedModel = validRod.model;
    unsupportedModel.getLine(0).removeBoundaryCondition(0);
    const QJsonObject modelReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &unsupportedModel, "source-A", "rev-1");
    const QJsonObject modelIssue = issueByCode(modelReport.value("issues").toArray(), "BC_UNDERCONSTRAINED");
    QVERIFY(!modelIssue.isEmpty());
    QCOMPARE(modelIssue.value("evidence").toObject().value("automaticPhysicalEdit").toBool(), false);
    const QJsonObject rodEvidence = modelIssue.value("evidence").toObject().value("modelSupport").toObject();
    QCOMPARE(rodEvidence.value("method").toString(), QStringLiteral("axial-rod-direction-coverage"));
    QVERIFY(rodEvidence.value("missingDirections").toArray().contains("uy"));
    QVERIFY(rodEvidence.value("missingDirections").toArray().contains("uz"));

    const auto validBeam = RTrustedMechanicsService::createCandidate(beamSpec(), "source-B", "rev-3");
    QVERIFY(validBeam.valid);
    RModel unsupportedBeam = validBeam.model;
    unsupportedBeam.getPoint(0).removeBoundaryCondition(1);
    const QJsonObject beamReport = RTrustedMechanicsService::diagnose(
        beamSpec(), &unsupportedBeam, "source-B", "rev-3");
    const QJsonObject beamIssue = issueByCode(beamReport.value("issues").toArray(), "BC_UNDERCONSTRAINED");
    QVERIFY(!beamIssue.isEmpty());
    const QJsonObject beamEvidence = beamIssue.value("evidence").toObject().value("modelSupport").toObject();
    QCOMPARE(beamEvidence.value("method").toString(), QStringLiteral("scaled-six-rigid-mode-constraint-rank"));
    QVERIFY(beamEvidence.value("observedConstraintRank").toInt() < 6);
    QCOMPARE(beamEvidence.value("hasUnrestrainedRigidMode").toBool(), true);
}

void TrustedMechanicsServiceTest::diagnosesZeroExcitationAndInvalidSettings()
{
    QJsonObject noLoad = rodSpec();
    QJsonArray loads = noLoad.value("loads").toArray();
    QJsonObject load = loads.first().toObject();
    load.insert("components", force(0.0, 0.0, 0.0));
    loads[0] = load;
    noLoad.insert("loads", loads);
    const QJsonObject report = RTrustedMechanicsService::diagnose(noLoad, nullptr, "s", "r");
    const QJsonObject zero = issueByCode(report.value("issues").toArray(), "ZERO_EXCITATION");
    QVERIFY(!zero.isEmpty());
    QCOMPARE(zero.value("severity").toString(), QStringLiteral("warning"));
    QVERIFY(zero.value("requiresUserConfirmation").toBool());
    QVERIFY(zero.value("allowedOperations").toArray().contains("requestLoadClarification"));

    QJsonObject invalid = rodSpec();
    QJsonObject analysis = invalid.value("analysis").toObject();
    analysis.insert("kind", "linearTransient");
    analysis.insert("timeStep", q(0.0, "s"));
    analysis.insert("steps", 0);
    analysis.insert("damping", QJsonObject{{"rayleighAlpha", q(0.0, "1/s")},
                                             {"rayleighBeta", q(0.0, "s")}});
    invalid.insert("analysis", analysis);
    invalid.insert("precision", QJsonObject{{"responseRelative", 0.01}});
    invalid.insert("initialConditions", QJsonArray{QJsonObject{{"type", "displacement"},
        {"selector", QJsonObject{{"kind", "allNodes"}}}, {"components", force(0.0, 0.0, 0.0, "m")}}});
    const QJsonObject badReport = RTrustedMechanicsService::diagnose(invalid, nullptr, "s", "r");
    QVERIFY(hasCode(badReport.value("issues").toArray(), "ANALYSIS_TIME_STEP_INVALID"));
    QVERIFY(hasCode(badReport.value("issues").toArray(), "ANALYSIS_STEP_COUNT_INVALID"));

    QJsonObject transientZero = rodSpec();
    analysis = transientZero.value("analysis").toObject();
    analysis.insert("kind", "linearTransient");
    analysis.insert("timeStep", q(0.01, "s"));
    analysis.insert("steps", 10);
    analysis.insert("damping", QJsonObject{{"rayleighAlpha", q(0.0, "1/s")},
                                             {"rayleighBeta", q(0.0, "s")}});
    transientZero.insert("analysis", analysis);
    transientZero.insert("precision", QJsonObject{{"responseRelative", 0.01}});
    transientZero.insert("initialConditions", QJsonArray{QJsonObject{{"type", "displacement"},
        {"selector", QJsonObject{{"kind", "allNodes"}}}, {"components", force(0.0, 0.0, 0.0, "m")}}});
    QJsonArray zeroLoads = transientZero.value("loads").toArray();
    QJsonObject zeroLoad = zeroLoads.first().toObject();
    zeroLoad.insert("components", force(0.0, 0.0, 0.0));
    zeroLoads[0] = zeroLoad;
    transientZero.insert("loads", zeroLoads);
    const QJsonObject transientReport = RTrustedMechanicsService::diagnose(transientZero, nullptr, "s", "r");
    const QJsonObject transientZeroIssue = issueByCode(transientReport.value("issues").toArray(), "ZERO_EXCITATION");
    QVERIFY(!transientZeroIssue.isEmpty());
    QCOMPARE(transientZeroIssue.value("evidence").toObject().value("hasNonzeroInitialCondition").toBool(), false);

    QJsonObject invalidHistory = transientZero;
    zeroLoads = invalidHistory.value("loads").toArray();
    zeroLoad = zeroLoads.first().toObject();
    zeroLoad.insert("history", QJsonArray{
        QJsonObject{{"time", q(0.0, "s")}, {"components", force(0.0, 0.0, 0.0)}},
        QJsonObject{{"time", q(0.0, "s")}, {"components", force(0.0, 0.0, 0.0)}}});
    zeroLoads[0] = zeroLoad;
    invalidHistory.insert("loads", zeroLoads);
    const auto historyResult = RTrustedMechanicsService::validate(invalidHistory);
    QVERIFY(!historyResult.valid);
    QVERIFY(hasCode(historyResult.issues, QStringLiteral("LOAD_HISTORY_TIME_ORDER_INVALID")));
}

void TrustedMechanicsServiceTest::rejectsUnresolvedAndNonsinglePointLoads()
{
    QJsonObject offGrid = rodSpec();
    QJsonArray loads = offGrid.value("loads").toArray();
    QJsonObject load = loads.first().toObject();
    load.insert("selector", QJsonObject{{"kind", "position"}, {"x", q(0.3, "m")}});
    loads[0] = load;
    offGrid.insert("loads", loads);
    const auto unresolved = RTrustedMechanicsService::createCandidate(offGrid, "source", "revision");
    QVERIFY(!unresolved.valid);
    QCOMPARE(unresolved.code, QStringLiteral("SELECTOR_NOT_FOUND"));

    QJsonObject withTolerance = offGrid;
    loads = withTolerance.value("loads").toArray();
    load = loads.first().toObject();
    load.insert("selector", QJsonObject{{"kind", "position"}, {"x", q(0.3, "m")},
                                        {"tolerance", q(0.1, "m")}});
    loads[0] = load;
    withTolerance.insert("loads", loads);
    const auto resolved = RTrustedMechanicsService::createCandidate(withTolerance, "source", "revision");
    QVERIFY2(resolved.valid, qPrintable(resolved.message));
    const QJsonObject loadBinding = resolved.inspection.value("selectorBindings").toArray().at(2).toObject();
    QVERIFY(loadBinding.value("valid").toBool());
    QCOMPARE(loadBinding.value("physicalIds").toArray().size(), 1);
    QCOMPARE(loadBinding.value("nodePositions").toArray().first().toInt(), 1);

    QJsonObject refinedSpec = withTolerance;
    QJsonObject geometry = refinedSpec.value("geometry").toObject();
    geometry.insert("segments", 8);
    refinedSpec.insert("geometry", geometry);
    const auto refined = RTrustedMechanicsService::createCandidate(refinedSpec, "source", "revision");
    QVERIFY(refined.valid);
    const QJsonObject refinedLoadBinding = refined.inspection.value("selectorBindings").toArray().at(2).toObject();
    QVERIFY(refinedLoadBinding.value("valid").toBool());
    QCOMPARE(refinedLoadBinding.value("modelRevision").toString(), QStringLiteral("revision"));
    QCOMPARE(refinedLoadBinding.value("nodePositions").toArray().first().toInt(), 2);
    QCOMPARE(refinedLoadBinding.value("physicalIds").toArray().first(),
             QJsonValue(double(refined.model.getNodePhysicalId(2))));
    const QJsonObject refinedLoadGroup = refined.inspection.value("model").toObject().value("points").toArray().at(1).toObject();
    const QJsonObject refinedForceX = refinedLoadGroup.value("boundaryConditions").toArray().first().toObject()
        .value("components").toArray().first().toObject();
    QCOMPARE(refinedForceX.value("unit").toString(), QStringLiteral("N"));
    QCOMPARE(refinedForceX.value("samples").toArray().first().toObject().value("value").toDouble(), 1000.0);

    QJsonObject allNodesLoad = rodSpec();
    loads = allNodesLoad.value("loads").toArray();
    load = loads.first().toObject();
    load.insert("selector", QJsonObject{{"kind", "allNodes"}});
    loads[0] = load;
    allNodesLoad.insert("loads", loads);
    const auto rejected = RTrustedMechanicsService::validate(allNodesLoad);
    QVERIFY(!rejected.valid);
    QVERIFY(hasCode(rejected.issues, QStringLiteral("LOAD_SELECTOR_NOT_SINGLE_NODE")));
}

void TrustedMechanicsServiceTest::rejectsTransverseAxialRodInputs()
{
    QJsonObject staticLoad = rodSpec();
    auto loads = staticLoad.value("loads").toArray();
    auto load = loads.first().toObject();
    load.insert("components", force(1000.0, 1.0, 0.0));
    loads[0] = load;
    staticLoad.insert("loads", loads);
    const auto staticResult = RTrustedMechanicsService::validate(staticLoad);
    QVERIFY(!staticResult.valid);
    QVERIFY(hasCode(staticResult.issues, QStringLiteral("AXIAL_ROD_TRANSVERSE_INPUT_UNSUPPORTED")));
    const auto staticCandidate = RTrustedMechanicsService::createCandidate(staticLoad, "transverse-static", "1");
    QVERIFY(!staticCandidate.valid);
    QCOMPARE(staticCandidate.code, QStringLiteral("AXIAL_ROD_TRANSVERSE_INPUT_UNSUPPORTED"));

    QJsonObject transientLoad = rodSpec();
    transientLoad["analysis"] = QJsonObject{{"kind", "linearTransient"},
        {"timeStep", q(0.01, "s")}, {"steps", 10},
        {"damping", QJsonObject{{"rayleighAlpha", q(0.0, "1/s")}, {"rayleighBeta", q(0.0, "s")}}}};
    auto transientLoads = transientLoad.value("loads").toArray();
    auto historyLoad = transientLoads.first().toObject();
    historyLoad.insert("history", QJsonArray{
        QJsonObject{{"time", q(0.0, "s")}, {"components", force(0.0, 0.0, 0.0)}},
        QJsonObject{{"time", q(0.01, "s")}, {"components", force(1.0, 0.0, 1.0)}}});
    historyLoad.insert("components", force(0.0, 0.0, 0.0));
    transientLoads[0] = historyLoad;
    transientLoad.insert("loads", transientLoads);
    transientLoad.insert("initialConditions", QJsonArray{
        QJsonObject{{"type", "displacement"}, {"selector", QJsonObject{{"kind", "freeEnd"}}},
                    {"components", force(0.0, 1.0, 0.0, "m")}},
        QJsonObject{{"type", "velocity"}, {"selector", QJsonObject{{"kind", "freeEnd"}}},
                    {"components", force(0.0, 0.0, 1.0, "m/s")}}});
    const auto transientResult = RTrustedMechanicsService::validate(transientLoad);
    QVERIFY(!transientResult.valid);
    QVERIFY(hasCode(transientResult.issues, QStringLiteral("AXIAL_ROD_TRANSVERSE_INPUT_UNSUPPORTED")));
    const auto transientCandidate = RTrustedMechanicsService::createCandidate(transientLoad, "transverse-transient", "1");
    QVERIFY(!transientCandidate.valid);
    QCOMPARE(transientCandidate.code, QStringLiteral("AXIAL_ROD_TRANSVERSE_INPUT_UNSUPPORTED"));
}

void TrustedMechanicsServiceTest::diagnosesDegenerateElementsAndMissingMaterial()
{
    const RModel model = degenerateUnmaterialedRod();
    const QJsonObject report = RTrustedMechanicsService::diagnose(rodSpec(), &model, "s", "r");
    QVERIFY(hasCode(report.value("issues").toArray(), "ELEMENT_DEGENERATE"));
    QVERIFY(hasCode(report.value("issues").toArray(), "MATERIAL_MISSING"));
    const QJsonObject degenerate = issueByCode(report.value("issues").toArray(), "ELEMENT_DEGENERATE");
    QVERIFY(degenerate.value("evidence").toObject().contains("length_m"));
    QVERIFY(degenerate.value("objects").toArray().size() == 1);

    auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    RModel altered = candidate.model;
    RMaterialProperty wrongModulus(RMaterialProperty::ModulusOfElasiticity);
    wrongModulus.add(293.15, 100.0e9);
    altered.getLine(0).getMaterial().add(wrongModulus);
    const QJsonObject mismatchReport = RTrustedMechanicsService::diagnose(rodSpec(), &altered, "source", "revision");
    const QJsonObject materialMismatch = issueByCode(mismatchReport.value("issues").toArray(), "MATERIAL_SPEC_MISMATCH");
    QVERIFY(!materialMismatch.isEmpty());
    QVERIFY(materialMismatch.value("evidence").toObject().value("mismatches").toArray().size() == 1);
    QVERIFY(materialMismatch.value("requiresUserConfirmation").toBool());

    RModel wrongMaterialState = candidate.model;
    wrongMaterialState.getLine(0).getMaterial().setState(RMaterial::Gas);
    const QJsonObject wrongStateReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &wrongMaterialState, "source", "revision");
    QVERIFY(hasCode(wrongStateReport.value("issues").toArray(), "MATERIAL_SPEC_MISMATCH"));
}

void TrustedMechanicsServiceTest::reconcilesModelStateWithProblemSpec()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    const QJsonObject baselineReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &candidate.model, "source", "revision");
    QVERIFY(!hasCode(baselineReport.value("issues").toArray(), "MODEL_GEOMETRY_SPEC_MISMATCH"));
    QVERIFY(!hasCode(baselineReport.value("issues").toArray(), "MODEL_SUPPORT_SPEC_MISMATCH"));
    QVERIFY(!hasCode(baselineReport.value("issues").toArray(), "MODEL_LOAD_SPEC_MISMATCH"));

    RModel changedForce = candidate.model;
    bool forceChanged = false;
    for (uint i = 0; i < changedForce.getNPoints(); ++i) {
        RPoint &point = changedForce.getPoint(i);
        if (!point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
        RBoundaryCondition &condition = point.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        RConditionComponent &fx = condition.getComponent(condition.findComponentPosition(R_VARIABLE_FORCE_X));
        fx.clear();
        fx.add(0.0, 1200.0);
        forceChanged = true;
    }
    QVERIFY(forceChanged);
    const QJsonObject forceReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &changedForce, "source", "revision");
    QVERIFY(hasCode(forceReport.value("issues").toArray(), "MODEL_LOAD_SPEC_MISMATCH"));

    QJsonObject zeroLoadSpec = rodSpec();
    QJsonArray zeroLoads = zeroLoadSpec.value("loads").toArray();
    QJsonObject zeroLoad = zeroLoads.at(0).toObject();
    zeroLoad.insert("components", force(0.0, 0.0, 0.0));
    zeroLoads[0] = zeroLoad;
    zeroLoadSpec.insert("loads", zeroLoads);
    const auto zeroLoadCandidate = RTrustedMechanicsService::createCandidate(
        zeroLoadSpec, "source", "revision");
    QVERIFY2(zeroLoadCandidate.valid, qPrintable(zeroLoadCandidate.message));

    RModel nanForce = zeroLoadCandidate.model;
    bool forceNanInjected = false;
    for (uint i = 0; i < nanForce.getNPoints(); ++i) {
        RPoint &point = nanForce.getPoint(i);
        if (!point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
        RBoundaryCondition &condition = point.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        RConditionComponent &fx = condition.getComponent(condition.findComponentPosition(R_VARIABLE_FORCE_X));
        fx.clear();
        fx.add(0.0, std::numeric_limits<double>::quiet_NaN());
        forceNanInjected = true;
    }
    QVERIFY(forceNanInjected);
    const QJsonObject nanForceReport = RTrustedMechanicsService::diagnose(
        zeroLoadSpec, &nanForce, "source", "revision");
    QVERIFY(hasCode(nanForceReport.value("issues").toArray(), "MODEL_LOAD_SPEC_MISMATCH"));

    RModel nanLoadTime = zeroLoadCandidate.model;
    bool timeNanInjected = false;
    for (uint i = 0; i < nanLoadTime.getNPoints(); ++i) {
        RPoint &point = nanLoadTime.getPoint(i);
        if (!point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
        RBoundaryCondition &condition = point.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        RConditionComponent &fx = condition.getComponent(condition.findComponentPosition(R_VARIABLE_FORCE_X));
        fx.clear();
        fx.add(std::numeric_limits<double>::quiet_NaN(), 0.0);
        timeNanInjected = true;
    }
    QVERIFY(timeNanInjected);
    const QJsonObject nanTimeReport = RTrustedMechanicsService::diagnose(
        zeroLoadSpec, &nanLoadTime, "source", "revision");
    QVERIFY(hasCode(nanTimeReport.value("issues").toArray(), "MODEL_LOAD_SPEC_MISMATCH"));

    RModel extraSupport = candidate.model;
    bool supportAdded = false;
    for (uint i = 0; i < extraSupport.getNPoints(); ++i) {
        RPoint &point = extraSupport.getPoint(i);
        if (!point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
        RBoundaryCondition displacement(R_BOUNDARY_CONDITION_DISPLACEMENT);
        const uint position = displacement.findComponentPosition(R_VARIABLE_DISPLACEMENT_X);
        displacement.getComponent(position).clear();
        displacement.getComponent(position).add(0.0, 0.0);
        point.addBoundaryCondition(displacement);
        supportAdded = true;
    }
    QVERIFY(supportAdded);
    const QJsonObject supportReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &extraSupport, "source", "revision");
    QVERIFY(hasCode(supportReport.value("issues").toArray(), "MODEL_SUPPORT_SPEC_MISMATCH"));

    RModel changedGeometry = candidate.model;
    changedGeometry.getNode(2).set(0.51, 0.0, 0.0);
    const QJsonObject geometryReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &changedGeometry, "source", "revision");
    QVERIFY(hasCode(geometryReport.value("issues").toArray(), "MODEL_GEOMETRY_SPEC_MISMATCH"));

    QJsonObject transientSpec = rodSpec();
    QJsonObject analysis{{"kind", "linearTransient"}, {"timeStep", q(0.01, "s")}, {"steps", 10},
        {"damping", QJsonObject{{"rayleighAlpha", q(0.0, "1/s")}, {"rayleighBeta", q(0.0, "s")}}}};
    transientSpec.insert("analysis", analysis);
    transientSpec.insert("precision", QJsonObject{{"responseRelative", 0.01}});
    transientSpec.insert("initialConditions", QJsonArray{QJsonObject{{"type", "displacement"},
        {"selector", QJsonObject{{"kind", "freeEnd"}}}, {"components", force(1.0, 0.0, 0.0, "mm")}}});
    const auto transient = RTrustedMechanicsService::createCandidate(transientSpec, "source", "revision");
    QVERIFY2(transient.valid, qPrintable(transient.message));
    RModel changedInitial = transient.model;
    bool initialChanged = false;
    for (uint i = 0; i < changedInitial.getNPoints(); ++i) {
        RPoint &point = changedInitial.getPoint(i);
        if (point.getNInitialConditions() == 0) continue;
        RInitialCondition &condition = point.getInitialCondition(0);
        RConditionComponent &ux = condition.getComponent(condition.findComponentPosition(R_VARIABLE_DISPLACEMENT_X));
        ux.clear();
        ux.add(0.0, 0.002);
        initialChanged = true;
    }
    QVERIFY(initialChanged);
    const QJsonObject initialReport = RTrustedMechanicsService::diagnose(
        transientSpec, &changedInitial, "source", "revision");
    QVERIFY(hasCode(initialReport.value("issues").toArray(), "MODEL_INITIAL_CONDITION_SPEC_MISMATCH"));

    RModel changedAnalysis = transient.model;
    changedAnalysis.getProblemSetup().getStructuralDynamicsSetup().setTimeStep(0.02);
    const QJsonObject analysisReport = RTrustedMechanicsService::diagnose(
        transientSpec, &changedAnalysis, "source", "revision");
    QVERIFY(hasCode(analysisReport.value("issues").toArray(), "MODEL_ANALYSIS_SPEC_MISMATCH"));

    QVERIFY(candidate.inspection.value("observedPhysicalStateDigest").toString().size() == 64);
    QCOMPARE(candidate.inspection.value("physicalSummaryBasis").toString(),
             QStringLiteral("normalizedProblemSpec"));
}

void TrustedMechanicsServiceTest::readsLiveBoundaryConditionLocalDirection()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    RModel model = candidate.model;
    RBoundaryCondition roller(R_BOUNDARY_CONDITION_DISPLACEMENT_ROLLER);
    roller.setExplicitLocalDirection(true);
    roller.setLocalDirection(RR3Vector(0.0, 1.0, 0.0));
    model.getLine(0).addBoundaryCondition(roller);

    const QJsonObject inspection = RTrustedMechanicsService::inspect(model, "source", "revision");
    const QJsonObject observedLine = inspection.value("observedPhysicalState").toObject()
        .value("lines").toArray().first().toObject();
    const QJsonObject observedRoller = observedLine.value("boundaryConditions").toArray().last().toObject();
    QCOMPARE(observedRoller.value("type").toInt(), int(R_BOUNDARY_CONDITION_DISPLACEMENT_ROLLER));
    QVERIFY(observedRoller.value("hasLocalDirection").toBool());
    QVERIFY(observedRoller.value("explicitLocalDirection").toBool());
    QCOMPARE(observedRoller.value("localDirection").toArray(), (QJsonArray{0.0, 1.0, 0.0}));

    RModel changedDirection = model;
    changedDirection.getLine(0).getBoundaryCondition(
        changedDirection.getLine(0).getNBoundaryConditions() - 1)
        .setLocalDirection(RR3Vector(1.0, 0.0, 0.0));
    const QJsonObject changedInspection = RTrustedMechanicsService::inspect(
        changedDirection, "source", "revision");
    QVERIFY(inspection.value("observedPhysicalStateDigest").toString()
            != changedInspection.value("observedPhysicalStateDigest").toString());
}

void TrustedMechanicsServiceTest::readsLiveDynamicsAndModalSettings()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    RModel model = candidate.model;
    RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setRhoInfinity(0.37);
    dynamics.setNewmarkGamma(0.61);
    dynamics.setNewmarkBeta(0.29);
    dynamics.setModalDampingRatios({0.015, 0.045, 0.08});
    RModalSetup &modal = model.getProblemSetup().getModalSetup();
    modal.setNIterations(321);
    modal.setConvergenceValue(2.5e-7);

    const QJsonObject inspection = RTrustedMechanicsService::inspect(model, "source", "revision");
    const QJsonObject analysis = inspection.value("observedPhysicalState").toObject()
        .value("analysis").toObject();
    const QJsonObject observedDynamics = analysis.value("structuralDynamics").toObject();
    QCOMPARE(observedDynamics.value("rhoInfinity").toDouble(), 0.37);
    QCOMPARE(observedDynamics.value("newmarkGamma").toDouble(), 0.61);
    QCOMPARE(observedDynamics.value("newmarkBeta").toDouble(), 0.29);
    QCOMPARE(observedDynamics.value("modalDampingRatios").toArray(),
             (QJsonArray{0.015, 0.045, 0.08}));
    const QJsonObject observedModal = analysis.value("modal").toObject();
    QCOMPARE(observedModal.value("nIterations").toInt(), 321);
    QCOMPARE(observedModal.value("convergenceValue").toDouble(), 2.5e-7);

    RModel changedDynamics = model;
    changedDynamics.getProblemSetup().getStructuralDynamicsSetup().setRhoInfinity(0.81);
    const QJsonObject changedInspection = RTrustedMechanicsService::inspect(
        changedDynamics, "source", "revision");
    QVERIFY(inspection.value("observedPhysicalStateDigest").toString()
            != changedInspection.value("observedPhysicalStateDigest").toString());
}

void TrustedMechanicsServiceTest::readsLivePointAndSurfaceProperties()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(beamSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    RModel baseline = candidate.model;
    QVERIFY(baseline.getNPoints() > 0);
    baseline.addSurface(RSurface());
    const QJsonObject baselineInspection = RTrustedMechanicsService::inspect(
        baseline, "source", "revision");
    const QString baselineDigest = baselineInspection.value("observedPhysicalStateDigest").toString();

    RModel withPointVolume = baseline;
    withPointVolume.getPoint(0).setVolume(0.0025);
    const QJsonObject volumeInspection = RTrustedMechanicsService::inspect(
        withPointVolume, "source", "revision");
    const QJsonObject observedPoint = volumeInspection.value("observedPhysicalState").toObject()
        .value("points").toArray().first().toObject();
    QCOMPARE(observedPoint.value("volume_m3").toDouble(), 0.0025);
    QVERIFY(volumeInspection.value("observedPhysicalStateDigest").toString() != baselineDigest);

    RModel withSurfaceThickness = baseline;
    withSurfaceThickness.getSurface(0).setThickness(0.125);
    const QJsonObject thicknessInspection = RTrustedMechanicsService::inspect(
        withSurfaceThickness, "source", "revision");
    const QJsonObject observedThicknessSurface = thicknessInspection.value("observedPhysicalState").toObject()
        .value("surfaces").toArray().first().toObject();
    QCOMPARE(observedThicknessSurface.value("thickness_m").toDouble(), 0.125);
    QVERIFY(thicknessInspection.value("observedPhysicalStateDigest").toString() != baselineDigest);

    const RElementFormulation originalFormulation = baseline.getSurface(0).getStructuralFormulation();
    const RElementFormulation changedFormulation = originalFormulation == RElementFormulation::PlaneStress
        ? RElementFormulation::PlaneStrain : RElementFormulation::PlaneStress;
    RModel withChangedFormulation = baseline;
    withChangedFormulation.getSurface(0).setStructuralFormulation(changedFormulation);
    const QJsonObject formulationInspection = RTrustedMechanicsService::inspect(
        withChangedFormulation, "source", "revision");
    const QJsonObject observedFormulationSurface = formulationInspection.value("observedPhysicalState").toObject()
        .value("surfaces").toArray().first().toObject();
    QCOMPARE(observedFormulationSurface.value("structuralFormulation").toInt(),
             int(changedFormulation));
    QCOMPARE(observedFormulationSurface.value("structuralFormulationId").toString(),
             RElementDescriptor::formulationToId(changedFormulation));
    QVERIFY(formulationInspection.value("observedPhysicalStateDigest").toString() != baselineDigest);
}

void TrustedMechanicsServiceTest::diagnosesDisconnectedFreeBeamComponent()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(beamSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    RModel disconnected = candidate.model;
    disconnected.setNNodes(disconnected.getNNodes() + 2);
    disconnected.getNode(5).set(3.0, 0.0, 0.0);
    disconnected.getNode(6).set(4.0, 0.0, 0.0);
    RElement freeBeam(R_ELEMENT_BEAM1);
    freeBeam.setNodeId(0, 5);
    freeBeam.setNodeId(1, 6);
    disconnected.addElement(freeBeam, true, 0);

    const QJsonObject report = RTrustedMechanicsService::diagnose(
        beamSpec(), &disconnected, "source", "revision");
    const QJsonObject underconstrained = issueByCode(report.value("issues").toArray(), "BC_UNDERCONSTRAINED");
    QVERIFY(!underconstrained.isEmpty());
    const QJsonObject support = underconstrained.value("evidence").toObject().value("modelSupport").toObject();
    QCOMPARE(support.value("structuralComponentCount").toInt(), 2);
    QVERIFY(support.value("hasUnrestrainedRigidMode").toBool());
}

void TrustedMechanicsServiceTest::diagnosesDisconnectedFreeRodComponent()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    RModel disconnected = candidate.model;
    disconnected.setNNodes(disconnected.getNNodes() + 2);
    disconnected.getNode(5).set(2.0, 0.0, 0.0);
    disconnected.getNode(6).set(3.0, 0.0, 0.0);
    RElement freeRod(R_ELEMENT_TRUSS1);
    freeRod.setNodeId(0, 5);
    freeRod.setNodeId(1, 6);
    disconnected.addElement(freeRod, true, 0);

    const QJsonObject report = RTrustedMechanicsService::diagnose(
        rodSpec(), &disconnected, "source", "revision");
    const QJsonObject underconstrained = issueByCode(report.value("issues").toArray(), "BC_UNDERCONSTRAINED");
    QVERIFY(!underconstrained.isEmpty());
    const QJsonObject support = underconstrained.value("evidence").toObject().value("modelSupport").toObject();
    QCOMPARE(support.value("structuralComponentCount").toInt(), 2);
    QVERIFY(support.value("hasUnrestrainedRigidMode").toBool());
}

void TrustedMechanicsServiceTest::diagnosesLineGroupMembershipMismatch()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(beamSpec(), "source", "revision");
    QVERIFY(candidate.valid);

    RModel missingMember = candidate.model;
    missingMember.getLine(0).remove(2);
    const QJsonObject missingReport = RTrustedMechanicsService::diagnose(
        beamSpec(), &missingMember, "source", "revision");
    QVERIFY(hasCode(missingReport.value("issues").toArray(), "MODEL_GEOMETRY_SPEC_MISMATCH"));

    RModel duplicatedMember = candidate.model;
    duplicatedMember.getLine(0).set(2, duplicatedMember.getLine(0).get(1));
    const QJsonObject duplicateReport = RTrustedMechanicsService::diagnose(
        beamSpec(), &duplicatedMember, "source", "revision");
    QVERIFY(hasCode(duplicateReport.value("issues").toArray(), "MODEL_GEOMETRY_SPEC_MISMATCH"));
}

void TrustedMechanicsServiceTest::rejectsUnsupportedModelElementsAndBoundaryConditions()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(beamSpec(), "source", "revision");
    QVERIFY(candidate.valid);

    RModel withSurfacePressure = candidate.model;
    withSurfacePressure.setNNodes(withSurfacePressure.getNNodes() + 2);
    withSurfacePressure.getNode(5).set(0.0, 1.0, 0.0);
    withSurfacePressure.getNode(6).set(1.0, 0.0, 0.0);
    RElement surfaceElement(R_ELEMENT_TRI1);
    surfaceElement.setNodeId(0, 0);
    surfaceElement.setNodeId(1, 5);
    surfaceElement.setNodeId(2, 6);
    withSurfacePressure.addElement(surfaceElement);
    RBoundaryCondition pressure(R_BOUNDARY_CONDITION_PRESSURE);
    RConditionComponent &pressureValue = pressure.getComponent(0);
    pressureValue.clear();
    pressureValue.add(0.0, 1000.0);
    withSurfacePressure.getSurface(0).addBoundaryCondition(pressure);
    RBoundaryCondition misplacedDisplacement(R_BOUNDARY_CONDITION_DISPLACEMENT);
    withSurfacePressure.getSurface(0).addBoundaryCondition(misplacedDisplacement);

    const QJsonObject pressureReport = RTrustedMechanicsService::diagnose(
        beamSpec(), &withSurfacePressure, "source", "revision");
    QVERIFY(hasCode(pressureReport.value("issues").toArray(), "MODEL_ELEMENT_TYPE_UNSUPPORTED"));
    QVERIFY(hasCode(pressureReport.value("issues").toArray(), "MODEL_BOUNDARY_CONDITION_UNSUPPORTED"));
    const QJsonObject inspected = RTrustedMechanicsService::inspect(
        withSurfacePressure, "source", "revision", candidate.normalizedSpec);
    const QJsonArray observedSurfaces = inspected.value("observedPhysicalState").toObject()
        .value("surfaces").toArray();
    QCOMPARE(observedSurfaces.size(), 1);
    QCOMPARE(observedSurfaces.first().toObject().value("boundaryConditions").toArray().size(), 2);

    RModel withVolumeSupport = candidate.model;
    withVolumeSupport.setNNodes(withVolumeSupport.getNNodes() + 4);
    withVolumeSupport.getNode(5).set(0.0, 1.0, 0.0);
    withVolumeSupport.getNode(6).set(1.0, 0.0, 0.0);
    withVolumeSupport.getNode(7).set(0.0, 0.0, 1.0);
    withVolumeSupport.getNode(8).set(1.0, 1.0, 1.0);
    RElement volumeElement(R_ELEMENT_TETRA1);
    volumeElement.setNodeId(0, 0);
    volumeElement.setNodeId(1, 5);
    volumeElement.setNodeId(2, 6);
    volumeElement.setNodeId(3, 7);
    withVolumeSupport.addElement(volumeElement);
    RBoundaryCondition volumeDisplacement(R_BOUNDARY_CONDITION_DISPLACEMENT);
    withVolumeSupport.getVolume(0).addBoundaryCondition(volumeDisplacement);
    const QJsonObject volumeReport = RTrustedMechanicsService::diagnose(
        beamSpec(), &withVolumeSupport, "source", "revision");
    QVERIFY(hasCode(volumeReport.value("issues").toArray(), "MODEL_BOUNDARY_CONDITION_UNSUPPORTED"));

    RModel withPointMoment = candidate.model;
    RBoundaryCondition moment(R_BOUNDARY_CONDITION_MOMENT);
    RConditionComponent &momentZ = moment.getComponent(
        moment.findComponentPosition(R_VARIABLE_MOMENT_Z));
    momentZ.clear();
    momentZ.add(0.0, 10.0);
    withPointMoment.getPoint(0).addBoundaryCondition(moment);
    const QJsonObject momentReport = RTrustedMechanicsService::diagnose(
        beamSpec(), &withPointMoment, "source", "revision");
    QVERIFY(hasCode(momentReport.value("issues").toArray(), "MODEL_BOUNDARY_CONDITION_UNSUPPORTED"));
}

void TrustedMechanicsServiceTest::diagnosesUnsupportedEnvironmentConditions()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source", "revision");
    QVERIFY(candidate.valid);
    const QJsonObject baselineReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &candidate.model, "source", "revision");
    QCOMPARE(baselineReport.value("status").toString(), QStringLiteral("ready"));

    RModel withGravity = candidate.model;
    REnvironmentCondition &gravity = withGravity.getLine(0).getEnvironmentCondition(
        R_ENVIRONMENT_CONDITION_G_ACCELERATION);
    RConditionComponent &gravityZ = gravity.getComponent(
        gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Z));
    gravityZ.setEnabled(true);
    gravityZ.clear();
    gravityZ.add(0.0, -9.81);

    const QJsonObject report = RTrustedMechanicsService::diagnose(
        rodSpec(), &withGravity, "source", "revision");
    const QJsonArray issues = report.value("issues").toArray();
    QCOMPARE(report.value("status").toString(), QStringLiteral("error"));
    QVERIFY(hasCode(issues, QStringLiteral("MODEL_ENVIRONMENT_CONDITION_UNSUPPORTED")));
    const QJsonObject unsupported = issueByCode(
        issues, QStringLiteral("MODEL_ENVIRONMENT_CONDITION_UNSUPPORTED"));
    QVERIFY(unsupported.value("requiresUserConfirmation").toBool());
    const QJsonObject evidence = unsupported.value("evidence").toObject();
    const QJsonObject condition = evidence.value("unsupportedConditions").toArray().first().toObject();
    QCOMPARE(condition.value("groupKind").toString(), QStringLiteral("line"));
    QCOMPARE(condition.value("conditionType").toInt(), int(R_ENVIRONMENT_CONDITION_G_ACCELERATION));
    QCOMPARE(condition.value("components").toArray().first().toObject()
                 .value("samples").toArray().first().toObject().value("value").toDouble(), -9.81);

    const QJsonObject baselineInspection = RTrustedMechanicsService::inspect(
        candidate.model, "source", "revision", candidate.normalizedSpec);
    const QJsonObject gravityInspection = RTrustedMechanicsService::inspect(
        withGravity, "source", "revision", candidate.normalizedSpec);
    const QJsonObject observedLine = gravityInspection.value("observedPhysicalState").toObject()
        .value("lines").toArray().first().toObject();
    const QJsonArray environmentConditions = observedLine.value("environmentConditions").toArray();
    QVERIFY(!environmentConditions.isEmpty());
    bool gravityReadBack = false;
    for (const QJsonValue &value : environmentConditions) {
        const QJsonObject environment = value.toObject();
        if (environment.value("type").toInt() != int(R_ENVIRONMENT_CONDITION_G_ACCELERATION)) continue;
        for (const QJsonValue &componentValue : environment.value("components").toArray()) {
            const QJsonObject component = componentValue.toObject();
            if (component.value("variable").toInt() != int(R_VARIABLE_G_ACCELERATION_Z)) continue;
            QCOMPARE(component.value("samples").toArray().first().toObject()
                         .value("value").toDouble(), -9.81);
            gravityReadBack = true;
        }
    }
    QVERIFY(gravityReadBack);
    QVERIFY(baselineInspection.value("observedPhysicalStateDigest").toString()
            != gravityInspection.value("observedPhysicalStateDigest").toString());

    RModel invalidTimeKey = candidate.model;
    REnvironmentCondition &invalidGravity = invalidTimeKey.getLine(0).getEnvironmentCondition(
        R_ENVIRONMENT_CONDITION_G_ACCELERATION);
    RConditionComponent &invalidGravityZ = invalidGravity.getComponent(
        invalidGravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Z));
    invalidGravityZ.setEnabled(true);
    invalidGravityZ.clear();
    invalidGravityZ.add(std::numeric_limits<double>::quiet_NaN(), 0.0);
    const QJsonObject invalidTimeReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &invalidTimeKey, "source", "revision");
    const QJsonObject invalidTimeIssue = issueByCode(invalidTimeReport.value("issues").toArray(),
        QStringLiteral("MODEL_ENVIRONMENT_CONDITION_UNSUPPORTED"));
    QVERIFY(!invalidTimeIssue.isEmpty());
    const QJsonObject invalidTimeSample = invalidTimeIssue.value("evidence").toObject()
        .value("unsupportedConditions").toArray().first().toObject()
        .value("components").toArray().first().toObject()
        .value("samples").toArray().first().toObject();
    QVERIFY(!invalidTimeSample.value("timeKeyFinite").toBool());
    QVERIFY(invalidTimeSample.value("time_s").isNull());
}

void TrustedMechanicsServiceTest::acceptsEquivalentEndpointSelectorAndRejectsRodRotations()
{
    QJsonObject positionFixed = rodSpec();
    QJsonArray supports = positionFixed.value("supports").toArray();
    QJsonObject support = supports.first().toObject();
    support.insert("selector", QJsonObject{{"kind", "position"}, {"x", q(0.0, "m")}});
    supports[0] = support;
    positionFixed.insert("supports", supports);
    const auto valid = RTrustedMechanicsService::validate(positionFixed);
    QVERIFY2(valid.valid, qPrintable(QJsonDocument(valid.issues).toJson()));

    QJsonObject positionFree = rodSpec();
    supports = positionFree.value("supports").toArray();
    support = supports.first().toObject();
    support.insert("selector", QJsonObject{{"kind", "position"}, {"x", q(1.0, "m")},
                                           {"tolerance", q(1.0, "m")}});
    supports[0] = support;
    positionFree.insert("supports", supports);
    const auto wrongEndpoint = RTrustedMechanicsService::validate(positionFree);
    QVERIFY(!wrongEndpoint.valid);
    QVERIFY(hasCode(wrongEndpoint.issues, "SUPPORT_MISSING_DOF"));

    QJsonObject rodRotation = rodSpec();
    supports = rodRotation.value("supports").toArray();
    support = supports.first().toObject();
    support.insert("dofs", QJsonArray{"ux", "uy", "uz", "rx"});
    supports[0] = support;
    rodRotation.insert("supports", supports);
    const auto invalid = RTrustedMechanicsService::validate(rodRotation);
    QVERIFY(!invalid.valid);
    QVERIFY(hasCode(invalid.issues, "ROD_ROTATIONAL_SUPPORT_UNSUPPORTED"));
}

void TrustedMechanicsServiceTest::bindsStaleResultsAndUnknownFailureEvidence()
{
    const auto candidate = RTrustedMechanicsService::createCandidate(rodSpec(), "source-A", "rev-2");
    QVERIFY(candidate.valid);
    const QString fingerprint = candidate.inspection.value("source").toObject().value("modelFingerprint").toString();
    const QJsonObject staleJob{{"jobId", "job-old"}, {"status", "completed"},
        {"sourceId", "source-A"}, {"modelRevision", "rev-1"},
        {"resultSource", QJsonObject{{"path", "old-result.rst"}, {"sourceId", "source-A"},
            {"modelRevision", "rev-1"}, {"modelFingerprint", fingerprint}}}};
    const QJsonObject stale = RTrustedMechanicsService::diagnose(
        rodSpec(), &candidate.model, "source-A", "rev-2", staleJob);
    QVERIFY(hasCode(stale.value("issues").toArray(), "RESULT_SOURCE_STALE"));
    const QJsonObject inspected = RTrustedMechanicsService::inspect(
        candidate.model, "source-A", "rev-2", candidate.normalizedSpec, staleJob);
    QCOMPARE(inspected.value("resultSource").toObject().value("path").toString(), QStringLiteral("old-result.rst"));

    const QJsonObject missingIdentity{{"jobId", "job-unbound"}, {"status", "completed"},
        {"sourceId", "source-A"}, {"modelRevision", "rev-2"},
        {"resultSource", QJsonObject{{"path", "unbound-result.rst"}}}};
    const QJsonObject unbound = RTrustedMechanicsService::diagnose(
        rodSpec(), &candidate.model, "source-A", "rev-2", missingIdentity);
    QVERIFY(hasCode(unbound.value("issues").toArray(), "RESULT_SOURCE_IDENTITY_MISSING"));

    const QJsonObject wrongFingerprint{{"jobId", "job-wrong-fingerprint"}, {"status", "completed"},
        {"sourceId", "source-A"}, {"modelRevision", "rev-2"},
        {"resultSource", QJsonObject{{"path", "wrong-model.rst"}, {"sourceId", "source-A"},
            {"modelRevision", "rev-2"}, {"modelFingerprint", QString(64, QLatin1Char('0'))}}}};
    const QJsonObject wrongFingerprintReport = RTrustedMechanicsService::diagnose(
        rodSpec(), &candidate.model, "source-A", "rev-2", wrongFingerprint);
    QVERIFY(hasCode(wrongFingerprintReport.value("issues").toArray(), "RESULT_SOURCE_STALE"));

    const QJsonObject matchingJob{{"jobId", "job-current"}, {"status", "completed"},
        {"sourceId", "source-A"}, {"modelRevision", "rev-2"},
        {"resultSource", QJsonObject{{"path", "current-result.rst"}, {"sourceId", "source-A"},
            {"modelRevision", "rev-2"}, {"modelFingerprint", fingerprint}}}};
    const QJsonObject currentResult = RTrustedMechanicsService::diagnose(
        rodSpec(), &candidate.model, "source-A", "rev-2", matchingJob);
    QVERIFY(!hasCode(currentResult.value("issues").toArray(), "RESULT_SOURCE_STALE"));
    QVERIFY(!hasCode(currentResult.value("issues").toArray(), "RESULT_SOURCE_IDENTITY_MISSING"));

    const QJsonObject failure = RTrustedMechanicsService::diagnoseFailure(
        rodSpec(), &candidate.model, "source-A", "rev-2", "job-failed", "SOLVER_EXIT_9", "solver log tail");
    QCOMPARE(failure.value("causeDetermined").toBool(), false);
    QCOMPARE(failure.value("errorCode").toString(), QStringLiteral("SOLVER_EXIT_9"));
    QCOMPARE(failure.value("solverLogSha256").toString().size(), 64);
    QCOMPARE(failure.value("modelSnapshotSha256").toString().size(), 64);
    QVERIFY(hasCode(failure.value("diagnosticReport").toObject().value("issues").toArray(),
                    "SOLVER_FAILURE_UNCLASSIFIED"));
    QVERIFY(!hasCode(failure.value("diagnosticReport").toObject().value("issues").toArray(),
                     "RESULT_SOURCE_STALE"));
}

QTEST_GUILESS_MAIN(TrustedMechanicsServiceTest)
#include "tst_trusted_mechanics_service.moc"
