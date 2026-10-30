#include <QtTest>
#include <QJsonDocument>

#include <rtrusted_convergence_service.h>
#include <rtrusted_mechanics_service.h>

#include <cmath>
#include <limits>

namespace
{
using Service = RTrustedConvergenceService;

QJsonObject quantity(double value, const char *unit)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("unit"), QString::fromLatin1(unit)}};
}

QJsonArray vector(double x, double y, double z, const char *unit)
{
    return {quantity(x, unit), quantity(y, unit), quantity(z, unit)};
}

QJsonObject rodSpec()
{
    const QJsonObject fixedEnd{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}};
    const QJsonObject allNodes{{QStringLiteral("kind"), QStringLiteral("allNodes")}};
    const QJsonObject freeEnd{{QStringLiteral("kind"), QStringLiteral("freeEnd")}};
    QJsonObject spec;
    spec.insert(QStringLiteral("schemaVersion"), 1);
    spec.insert(QStringLiteral("modelType"), QStringLiteral("axialRod"));
    spec.insert(QStringLiteral("name"), QStringLiteral("convergence rod"));
    spec.insert(QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), quantity(1.0, "m")},
                                                        {QStringLiteral("segments"), 4}});
    spec.insert(QStringLiteral("material"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Steel")},
                                                         {QStringLiteral("source"), QStringLiteral("fixture")},
                                                         {QStringLiteral("youngModulus"), quantity(210.0e9, "Pa")},
                                                         {QStringLiteral("poissonRatio"), 0.3},
                                                         {QStringLiteral("density"), quantity(7850.0, "kg/m3")}});
    spec.insert(QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), quantity(1.0e-4, "m2")}});
    spec.insert(QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}});
    QJsonArray supports;
    supports.append(QJsonObject{{QStringLiteral("selector"), fixedEnd},
                                {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}});
    supports.append(QJsonObject{{QStringLiteral("selector"), allNodes},
                                {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}});
    spec.insert(QStringLiteral("supports"), supports);
    spec.insert(QStringLiteral("loads"), QJsonArray{QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("pointForce")}, {QStringLiteral("selector"), freeEnd},
        {QStringLiteral("components"), vector(1000.0, 0.0, 0.0, "N")}}});
    spec.insert(QStringLiteral("initialConditions"), QJsonArray{});
    spec.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), freeEnd}, {QStringLiteral("quantity"), QStringLiteral("displacement")},
        {QStringLiteral("component"), QStringLiteral("ux")}}});
    spec.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}});
    return spec;
}

QJsonObject beamSpec()
{
    QJsonObject spec = rodSpec();
    spec.insert(QStringLiteral("modelType"), QStringLiteral("cantileverBeam"));
    spec.insert(QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), quantity(1.0e-4, "m2")},
                                                       {QStringLiteral("iy"), quantity(8.333333333333333e-10, "m4")},
                                                       {QStringLiteral("iz"), quantity(8.333333333333333e-10, "m4")},
                                                       {QStringLiteral("torsionalConstant"), quantity(1.666666666666667e-9, "m4")},
                                                       {QStringLiteral("wy"), quantity(1.666666666666667e-7, "m3")},
                                                       {QStringLiteral("wz"), quantity(1.666666666666667e-7, "m3")},
                                                       {QStringLiteral("wt"), quantity(1.666666666666667e-7, "m3")},
                                                       {QStringLiteral("localY"), QJsonArray{0.0, 1.0, 0.0}}});
    const QJsonObject fixedEnd{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}};
    const QJsonArray beamDofs{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                              QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")};
    spec.insert(QStringLiteral("supports"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), fixedEnd}, {QStringLiteral("dofs"), beamDofs}}});
    QJsonArray loads = spec.value(QStringLiteral("loads")).toArray();
    QJsonObject load = loads.first().toObject();
    load.insert(QStringLiteral("components"), vector(0.0, 0.0, -100.0, "N"));
    loads[0] = load;
    spec.insert(QStringLiteral("loads"), loads);
    return spec;
}

QJsonObject transientSpec(int steps = 10)
{
    QJsonObject spec = rodSpec();
    spec.insert(QStringLiteral("analysis"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("linearTransient")},
        {QStringLiteral("timeStep"), quantity(0.01, "s")},
        {QStringLiteral("steps"), steps},
        {QStringLiteral("damping"), QJsonObject{{QStringLiteral("rayleighAlpha"), quantity(0.0, "1/s")},
                                                 {QStringLiteral("rayleighBeta"), quantity(0.0, "s")}}}});
    spec.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("responseRelative"), 0.01}});
    spec.insert(QStringLiteral("initialConditions"), QJsonArray{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("displacement")},
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
        {QStringLiteral("components"), vector(0.0, 0.0, 0.0, "m")}}});
    return spec;
}

QJsonObject modalSpec()
{
    QJsonObject spec = rodSpec();
    spec.insert(QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("modal")},
                                                        {QStringLiteral("modes"), 1}});
    spec.insert(QStringLiteral("loads"), QJsonArray{});
    spec.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("quantity"), QStringLiteral("frequency")},
        {QStringLiteral("component"), QStringLiteral("mode")}}});
    spec.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("frequencyRelative"), 0.02}});
    return spec;
}

QJsonObject study(const char *kind, int levels = 3)
{
    return {{QStringLiteral("kind"), QString::fromLatin1(kind)},
            {QStringLiteral("maximumLevels"), levels}};
}

QJsonObject displacement(const QJsonArray &values, const QJsonArray &times = {})
{
    QJsonObject item{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                      {QStringLiteral("component"), QStringLiteral("ux")},
                      {QStringLiteral("coordinateM"), QJsonArray{1.0, 0.0, 0.0}},
                      {QStringLiteral("values"), values}};
    if (!times.isEmpty()) item.insert(QStringLiteral("timesS"), times);
    return item;
}

QJsonObject staticMeasurement(double value, double energy)
{
    return {{QStringLiteral("analysisKind"), QStringLiteral("static")},
            {QStringLiteral("expectedDisplacementCount"), 1},
            {QStringLiteral("displacements"), QJsonArray{displacement(QJsonArray{value})}},
            {QStringLiteral("strainEnergyJ"), energy}};
}

QJsonObject modalMeasurement(int mode, double frequency)
{
    return {{QStringLiteral("analysisKind"), QStringLiteral("modal")},
            {QStringLiteral("expectedDisplacementCount"), 0},
            {QStringLiteral("displacements"), QJsonArray{}},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("frequencyHz"), frequency}};
}

QJsonArray qoiChanges(double first, double second)
{
    return {QJsonObject{{QStringLiteral("qoiChanges"), QJsonArray{QJsonObject{
                              {QStringLiteral("status"), QStringLiteral("defined")},
                              {QStringLiteral("relativeChange"), first}}}}},
            QJsonObject{{QStringLiteral("qoiChanges"), QJsonArray{QJsonObject{
                              {QStringLiteral("status"), QStringLiteral("defined")},
                              {QStringLiteral("relativeChange"), second}}}}}};
}
}

class TrustedConvergenceServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsMalformedStudyAndCallerTolerance();
    void rejectsUnsupportedDeclaredQuantitiesOfInterest();
    void spatialCandidatesAreNestedAndChangeOnlySegments();
    void enforcesVerifierDofCap();
    void timeStepCandidatesPreserveDurationAndCapFrames();
    void rejectsUnsafeTransientMeshInitialConditions();
    void comparesEveryStaticQuantityAndHandlesNearZero();
    void comparesTransientHistoriesOnlyAtCommonTimes();
    void modalComparisonsRequireTheSameMode();
    void boundsMaximumTransientEvidenceSize();
    void rejectsOversizedStaticMeasurementsBeforeSolve();
    void requiresTwoConsecutiveChangesBelowOnePercent();
};

void TrustedConvergenceServiceTest::rejectsMalformedStudyAndCallerTolerance()
{
    auto result = Service::validateStudy(rodSpec(), study("mesh"));
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(result.data.value(QStringLiteral("fixedToleranceRelative")).toDouble() == Service::FixedRelativeTolerance);

    QJsonObject invalid = study("mesh");
    invalid.insert(QStringLiteral("tolerance"), 0.02);
    result = Service::validateStudy(rodSpec(), invalid);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("STUDY_TOLERANCE_FIXED"));

    invalid = study("mesh", 2);
    result = Service::validateStudy(rodSpec(), invalid);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("STUDY_LEVELS_INVALID"));

    invalid = study("timeStep");
    result = Service::validateStudy(rodSpec(), invalid);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("STUDY_ANALYSIS_MISMATCH"));
}

void TrustedConvergenceServiceTest::rejectsUnsupportedDeclaredQuantitiesOfInterest()
{
    QJsonObject unsupported = rodSpec();
    unsupported.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("quantity"), QStringLiteral("stress")},
        {QStringLiteral("component"), QStringLiteral("x")}}});
    const auto result = Service::validateStudy(unsupported, study("mesh"));
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("STUDY_QOI_UNSUPPORTED"));

    unsupported = rodSpec();
    QJsonArray observables = unsupported.value(QStringLiteral("observables")).toArray();
    QJsonObject displacementQoi = observables.first().toObject();
    displacementQoi.insert(QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}});
    observables[0] = displacementQoi;
    unsupported.insert(QStringLiteral("observables"), observables);
    const auto multipleNodes = Service::validateStudy(unsupported, study("mesh"));
    QVERIFY(!multipleNodes.ok);
    QCOMPARE(multipleNodes.code, QStringLiteral("STUDY_QOI_UNSUPPORTED"));

    QVERIFY2(Service::validateStudy(modalSpec(), study("mesh")).ok,
             "A modal frequency QOI is supported and must remain admissible.");
    QJsonObject modalWithAmplitude = modalSpec();
    QJsonArray modalObservables = modalWithAmplitude.value(QStringLiteral("observables")).toArray();
    modalObservables.append(QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("quantity"), QStringLiteral("displacement")},
        {QStringLiteral("component"), QStringLiteral("ux")}});
    modalWithAmplitude.insert(QStringLiteral("observables"), modalObservables);
    const auto arbitraryAmplitude = Service::validateStudy(modalWithAmplitude, study("mesh"));
    QVERIFY(!arbitraryAmplitude.ok);
    QCOMPARE(arbitraryAmplitude.code, QStringLiteral("STUDY_QOI_UNSUPPORTED"));
}

void TrustedConvergenceServiceTest::spatialCandidatesAreNestedAndChangeOnlySegments()
{
    const QJsonObject baseline = rodSpec();
    const QJsonObject meshStudy = study("mesh", 4);
    QJsonObject level1, level2, level3;
    QVERIFY(Service::proposeLevel(baseline, meshStudy, 1, &level1).ok);
    QVERIFY(Service::proposeLevel(baseline, meshStudy, 2, &level2).ok);
    QVERIFY(Service::proposeLevel(baseline, meshStudy, 3, &level3).ok);
    QCOMPARE(level1.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("segments")).toInt(), 8);
    QCOMPARE(level2.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("segments")).toInt(), 16);
    QCOMPARE(level3.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("segments")).toInt(), 32);
    for (QJsonObject candidate : {level1, level2, level3}) {
        QVERIFY(RTrustedMechanicsService::validate(candidate).valid);
        QJsonObject candidateGeometry = candidate.value(QStringLiteral("geometry")).toObject();
        candidateGeometry.remove(QStringLiteral("segments"));
        QJsonObject baselineGeometry = baseline.value(QStringLiteral("geometry")).toObject();
        baselineGeometry.remove(QStringLiteral("segments"));
        candidate.insert(QStringLiteral("geometry"), candidateGeometry);
        QJsonObject baselineOther = baseline;
        baselineOther.remove(QStringLiteral("geometry"));
        QJsonObject candidateOther = candidate;
        candidateOther.remove(QStringLiteral("geometry"));
        QCOMPARE(candidateGeometry, baselineGeometry);
        QCOMPARE(candidateOther, baselineOther);
    }
    QJsonObject output;
    const auto outOfRange = Service::proposeLevel(baseline, meshStudy, 4, &output);
    QVERIFY(!outOfRange.ok);
    QCOMPARE(outOfRange.code, QStringLiteral("LEVEL_INDEX_INVALID"));
}

void TrustedConvergenceServiceTest::enforcesVerifierDofCap()
{
    const auto tooFine = Service::validateStudy(beamSpec(), study("mesh", 4));
    QVERIFY(!tooFine.ok);
    QCOMPARE(tooFine.code, QStringLiteral("STUDY_DOF_LIMIT"));

    QJsonObject smallerBeam = beamSpec();
    QJsonObject geometry = smallerBeam.value(QStringLiteral("geometry")).toObject();
    geometry.insert(QStringLiteral("segments"), 2);
    smallerBeam.insert(QStringLiteral("geometry"), geometry);
    QVERIFY(Service::validateStudy(smallerBeam, study("mesh", 4)).ok);
    QJsonObject candidate;
    QVERIFY(Service::proposeLevel(smallerBeam, study("mesh", 4), 3, &candidate).ok);
    QCOMPARE(candidate.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("segments")).toInt(), 16);
}

void TrustedConvergenceServiceTest::timeStepCandidatesPreserveDurationAndCapFrames()
{
    const QJsonObject baseline = transientSpec(10);
    const QJsonObject temporalStudy = study("timeStep", 4);
    const auto validated = Service::validateStudy(baseline, temporalStudy);
    QVERIFY2(validated.ok, qPrintable(validated.message));
    QJsonObject level1, level2, level3;
    QVERIFY(Service::proposeLevel(baseline, temporalStudy, 1, &level1).ok);
    QVERIFY(Service::proposeLevel(baseline, temporalStudy, 2, &level2).ok);
    QVERIFY(Service::proposeLevel(baseline, temporalStudy, 3, &level3).ok);
    for (const QJsonObject &candidate : {level1, level2, level3}) {
        const auto candidateValidation = RTrustedMechanicsService::validate(candidate);
        QVERIFY(candidateValidation.valid);
        const QJsonObject candidateAnalysis = candidateValidation.normalizedSpec.value(QStringLiteral("analysis")).toObject();
        QCOMPARE(candidateAnalysis.value(QStringLiteral("duration")).toDouble(), 0.1);
    }
    const QJsonObject finalAnalysis = RTrustedMechanicsService::validate(level3).normalizedSpec
        .value(QStringLiteral("analysis")).toObject();
    QCOMPARE(finalAnalysis.value(QStringLiteral("timeStep")).toDouble(), 0.00125);
    QCOMPARE(finalAnalysis.value(QStringLiteral("steps")).toInt(), 80);

    const auto tooManyFrames = Service::validateStudy(transientSpec(1250), temporalStudy);
    QVERIFY(!tooManyFrames.ok);
    QCOMPARE(tooManyFrames.code, QStringLiteral("STUDY_FRAME_LIMIT"));
}

void TrustedConvergenceServiceTest::rejectsUnsafeTransientMeshInitialConditions()
{
    QJsonObject nonzero = transientSpec();
    QJsonArray initial = nonzero.value(QStringLiteral("initialConditions")).toArray();
    QJsonObject condition = initial.first().toObject();
    condition.insert(QStringLiteral("components"), vector(1.0e-4, 0.0, 0.0, "m"));
    initial[0] = condition;
    nonzero.insert(QStringLiteral("initialConditions"), initial);
    const auto result = Service::validateStudy(nonzero, study("mesh"));
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MESH_INITIAL_CONDITION_UNSUPPORTED"));

    const auto zero = Service::validateStudy(transientSpec(), study("mesh"));
    QVERIFY2(zero.ok, qPrintable(zero.message));
}

void TrustedConvergenceServiceTest::comparesEveryStaticQuantityAndHandlesNearZero()
{
    QJsonObject missingEnergy = staticMeasurement(1.0, 1.0);
    missingEnergy.remove(QStringLiteral("strainEnergyJ"));
    auto result = Service::compareMeasurements("mesh", staticMeasurement(1.0, 1.0), missingEnergy);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_INVALID"));

    result = Service::compareMeasurements("mesh", staticMeasurement(1.0e-30, 2.0),
                                          staticMeasurement(1.005e-30, 2.01));
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.data.value(QStringLiteral("qoiChanges")).toArray().size(), 2);
    QVERIFY(result.data.value(QStringLiteral("belowTolerance")).toBool());
    QVERIFY(std::abs(result.data.value(QStringLiteral("aggregateRelativeChange")).toDouble() - (0.005 / 1.005)) < 1.0e-12);

    QJsonObject missingQoi = staticMeasurement(1.0, 1.0);
    missingQoi.insert(QStringLiteral("expectedDisplacementCount"), 2);
    result = Service::compareMeasurements("mesh", staticMeasurement(1.0, 1.0), missingQoi);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_INVALID"));

    result = Service::compareMeasurements("static", staticMeasurement(1.0, 1.0), staticMeasurement(0.0, 1.0));
    QVERIFY(result.ok);
    QCOMPARE(result.data.value(QStringLiteral("status")).toString(), QStringLiteral("undefined"));
    QVERIFY(!result.data.value(QStringLiteral("belowTolerance")).toBool());
    QVERIFY(result.data.value(QStringLiteral("qoiChanges")).toArray().at(0).toObject()
                .value(QStringLiteral("relativeChange")).isNull());

    result = Service::compareMeasurements("mesh", staticMeasurement(0.0, 0.0), staticMeasurement(0.0, 0.0));
    QVERIFY(result.ok);
    QVERIFY(result.data.value(QStringLiteral("belowTolerance")).toBool());

    QJsonObject mismatched = staticMeasurement(1.0, 1.0);
    QJsonArray displacements = mismatched.value(QStringLiteral("displacements")).toArray();
    QJsonObject item = displacements.first().toObject();
    item.insert(QStringLiteral("coordinateM"), QJsonArray{0.9, 0.0, 0.0});
    displacements[0] = item;
    mismatched.insert(QStringLiteral("displacements"), displacements);
    result = Service::compareMeasurements("mesh", staticMeasurement(1.0, 1.0), mismatched);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_SELECTOR_MISMATCH"));
}

void TrustedConvergenceServiceTest::comparesTransientHistoriesOnlyAtCommonTimes()
{
    const QJsonObject coarse{{QStringLiteral("analysisKind"), QStringLiteral("linearTransient")},
                             {QStringLiteral("expectedDisplacementCount"), 1},
                             {QStringLiteral("displacements"), QJsonArray{displacement(
                                  QJsonArray{0.0, 1.0, 2.0}, QJsonArray{0.0, 0.1, 0.2})}}};
    const QJsonObject fine{{QStringLiteral("analysisKind"), QStringLiteral("linearTransient")},
                           {QStringLiteral("expectedDisplacementCount"), 1},
                           {QStringLiteral("displacements"), QJsonArray{displacement(
                                QJsonArray{0.0, 0.5, 1.01, 1.5, 2.02}, QJsonArray{0.0, 0.05, 0.1, 0.15, 0.2})}}};
    auto result = Service::compareMeasurements("timeStep", coarse, fine);
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(std::abs(result.data.value(QStringLiteral("aggregateRelativeChange")).toDouble() - (0.02 / 2.02)) < 1.0e-12);
    QVERIFY(result.data.value(QStringLiteral("belowTolerance")).toBool());

    const QJsonObject noCommon{{QStringLiteral("analysisKind"), QStringLiteral("linearTransient")},
                               {QStringLiteral("expectedDisplacementCount"), 1},
                               {QStringLiteral("displacements"), QJsonArray{displacement(
                                    QJsonArray{0.0, 1.0, 2.0}, QJsonArray{0.01, 0.11, 0.21})}}};
    result = Service::compareMeasurements("mesh", coarse, noCommon);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_COMMON_TIMES_MISSING"));

    QJsonObject invalid = fine;
    QJsonArray entries = invalid.value(QStringLiteral("displacements")).toArray();
    QJsonObject history = entries.first().toObject();
    history.insert(QStringLiteral("values"), QJsonArray{0.0, 0.5, QJsonValue(std::numeric_limits<double>::quiet_NaN()), 1.5, 2.02});
    entries[0] = history;
    invalid.insert(QStringLiteral("displacements"), entries);
    result = Service::compareMeasurements("timeStep", coarse, invalid);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_INVALID"));
}

void TrustedConvergenceServiceTest::modalComparisonsRequireTheSameMode()
{
    auto result = Service::compareMeasurements("mesh", modalMeasurement(1, 25.0),
                                               modalMeasurement(1, 25.1));
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.data.value(QStringLiteral("qoiChanges")).toArray().size(), 1);
    QVERIFY(result.data.value(QStringLiteral("belowTolerance")).toBool());

    result = Service::compareMeasurements("mesh", modalMeasurement(1, 25.0),
                                          modalMeasurement(2, 25.1));
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_QOI_MISMATCH"));

    QJsonObject missingMode = modalMeasurement(1, 25.0);
    missingMode.remove(QStringLiteral("mode"));
    result = Service::compareMeasurements("mesh", missingMode, modalMeasurement(1, 25.1));
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("MEASUREMENT_INVALID"));
}

void TrustedConvergenceServiceTest::boundsMaximumTransientEvidenceSize()
{
    const QJsonObject extremeNumbers{{QStringLiteral("minimum"), -std::numeric_limits<double>::max()},
                                     {QStringLiteral("maximum"), std::numeric_limits<double>::max()}};
    const QByteArray extremeNumberBytes = RTrustedRecordSerialization::compact(extremeNumbers);
    QCOMPARE(extremeNumberBytes.size(),
             QJsonDocument(extremeNumbers).toJson(QJsonDocument::Compact).size());
    QCOMPARE(QJsonDocument::fromJson(extremeNumberBytes).object(), extremeNumbers);

    const QJsonObject maximumResultRecord{{QStringLiteral("recordNo"), 10000},
        {QStringLiteral("timeS"), std::numeric_limits<double>::max()},
        {QStringLiteral("originKind"), QString(64, QLatin1Char('x'))},
        {QStringLiteral("modelSha256"), QString(64, QLatin1Char('f'))}};
    QCOMPARE(maximumResultRecord.value(QStringLiteral("originKind")).toString().size(), 64);
    QVERIFY(RTrustedRecordSerialization::compact(maximumResultRecord).size() <= 256);

    QJsonObject escapedSpec = transientSpec(9999);
    escapedSpec.insert(QStringLiteral("name"), QString::fromUtf8("rod雪")
                       + QChar(0x0001) + QChar(0x0000) + QChar(0x2028));
    const auto escapedValidation = RTrustedMechanicsService::validate(escapedSpec);
    QVERIFY(escapedValidation.valid);
    const QJsonObject normalizedEscapedSpec = escapedValidation.normalizedSpec;
    const qsizetype maximumOneQoi = Service::estimateMaximumTransientRecordBytes(
        escapedSpec, Service::MaximumTransientFrames, 1);
    QVERIFY(maximumOneQoi > Service::MaximumDurableRecordBytes);
    const QJsonObject boundedTransient = transientSpec(7500);
    QVERIFY(Service::estimateMaximumTransientRecordBytes(boundedTransient, 7501, 1)
            < Service::MaximumDurableRecordBytes);

    QJsonObject excessiveQois = escapedSpec;
    QJsonArray excessiveObservables = excessiveQois.value(QStringLiteral("observables")).toArray();
    while (excessiveObservables.size() < 16) excessiveObservables.append(excessiveObservables.first());
    excessiveQois.insert(QStringLiteral("observables"), excessiveObservables);
    QVERIFY(RTrustedMechanicsService::validate(excessiveQois).valid);
    QVERIFY(Service::estimateMaximumTransientRecordBytes(excessiveQois,
                Service::MaximumTransientFrames, 16)
            > Service::MaximumDurableRecordBytes);
    QVERIFY(Service::estimateMaximumTransientRecordBytes(0, 1)
            > Service::MaximumDurableRecordBytes);

    QJsonObject positionSpec = transientSpec(10);
    QJsonArray positionObservables = positionSpec.value(QStringLiteral("observables")).toArray();
    QJsonObject positionObservable = positionObservables.first().toObject();
    positionObservable.insert(QStringLiteral("selector"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("position")},
        {QStringLiteral("x"), quantity(1.0, "m")},
        {QStringLiteral("tolerance"), quantity(1.0e-9, "m")}});
    positionObservables[0] = positionObservable;
    positionSpec.insert(QStringLiteral("observables"), positionObservables);
    const auto positionValidation = RTrustedMechanicsService::validate(positionSpec);
    QVERIFY(positionValidation.valid);
    const auto positionStudy = Service::validateStudy(positionSpec, study("mesh", 3));
    QVERIFY2(positionStudy.ok, qPrintable(positionStudy.message));
    const QJsonObject normalizedPosition = positionStudy.data
        .value(QStringLiteral("normalizedProblemSpec")).toObject();
    QVERIFY(RTrustedRecordSerialization::compact(
                normalizedPosition.value(QStringLiteral("observables")).toArray().first().toObject()
                    .value(QStringLiteral("selector")).toObject()).size()
            < Service::MaximumSelectorIdentityBytes);

    QJsonObject malformedSelector = normalizedEscapedSpec;
    QJsonArray malformedObservables = malformedSelector.value(QStringLiteral("observables")).toArray();
    QJsonObject malformedObservable = malformedObservables.first().toObject();
    QJsonObject oversizedSelector = malformedObservable.value(QStringLiteral("selector")).toObject();
    oversizedSelector.insert(QStringLiteral("padding"), QString(1024, QLatin1Char('x')));
    malformedObservable.insert(QStringLiteral("selector"), oversizedSelector);
    malformedObservables[0] = malformedObservable;
    malformedSelector.insert(QStringLiteral("observables"), malformedObservables);
    QVERIFY(Service::estimateMaximumTransientRecordBytes(malformedSelector,
                Service::MaximumTransientFrames, 1)
            > Service::MaximumDurableRecordBytes);

    const QJsonObject escapedObject{{QStringLiteral("control"), QString::fromUtf8("雪")
        + QChar(0x0001) + QChar(0x0000) + QChar(0x2028)}};
    const QByteArray serialized = RTrustedRecordSerialization::compact(escapedObject);
    QCOMPARE(serialized.size(), QJsonDocument(escapedObject).toJson(QJsonDocument::Compact).size());
    QVERIFY(serialized.contains("\\u0001"));
    QVERIFY(serialized.contains("\\u0000"));

    QJsonObject oversizedSelectorMeasurement = staticMeasurement(1.0, 1.0);
    QJsonArray oversizedSelectorEntries = oversizedSelectorMeasurement
        .value(QStringLiteral("displacements")).toArray();
    QJsonObject oversizedSelectorEntry = oversizedSelectorEntries.first().toObject();
    oversizedSelectorEntry.insert(QStringLiteral("selector"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("freeEnd")},
        {QStringLiteral("padding"), QString(1024, QLatin1Char('x'))}});
    oversizedSelectorEntries[0] = oversizedSelectorEntry;
    oversizedSelectorMeasurement.insert(QStringLiteral("displacements"), oversizedSelectorEntries);
    const auto rejectedSelector = Service::compareMeasurements(
        "mesh", oversizedSelectorMeasurement, oversizedSelectorMeasurement);
    QVERIFY(!rejectedSelector.ok);
    QCOMPARE(rejectedSelector.code, QStringLiteral("MEASUREMENT_INVALID"));

    const auto largeGridWithinRecordCap = Service::validateStudy(transientSpec(937), study("timeStep", 4));
    QVERIFY2(largeGridWithinRecordCap.ok, qPrintable(largeGridWithinRecordCap.code + QStringLiteral(": ")
             + largeGridWithinRecordCap.message));
    const auto overFrameLimit = Service::validateStudy(transientSpec(1250), study("timeStep", 4));
    QVERIFY(!overFrameLimit.ok);
    QCOMPARE(overFrameLimit.code, QStringLiteral("STUDY_FRAME_LIMIT"));

    const auto largeMeshHistoryWithinRecordCap = Service::validateStudy(transientSpec(7500), study("mesh", 4));
    QVERIFY2(largeMeshHistoryWithinRecordCap.ok,
             qPrintable(largeMeshHistoryWithinRecordCap.code + QStringLiteral(": ")
                        + largeMeshHistoryWithinRecordCap.message));
    QCOMPARE(largeMeshHistoryWithinRecordCap.data.value(QStringLiteral("normalizedProblemSpec")).toObject()
                 .value(QStringLiteral("analysis")).toObject()
                 .value(QStringLiteral("steps")).toInt(), 7500);
    const auto oversizedMeshHistory = Service::validateStudy(transientSpec(10000), study("mesh", 4));
    QVERIFY(!oversizedMeshHistory.ok);
    QCOMPARE(oversizedMeshHistory.code, QStringLiteral("STUDY_FRAME_LIMIT"));

    const auto oversizedRecord = Service::validateStudy(excessiveQois, study("mesh", 4));
    QVERIFY(!oversizedRecord.ok);
    QCOMPARE(oversizedRecord.code, QStringLiteral("STUDY_RECORD_SIZE_LIMIT"));
}

void TrustedConvergenceServiceTest::rejectsOversizedStaticMeasurementsBeforeSolve()
{
    QJsonObject spec = rodSpec();
    QJsonArray observables;
    for (int index = 1; index <= 2000; ++index) {
        const double x = double(index) / 2001.0;
        observables.append(QJsonObject{
            {QStringLiteral("selector"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("position")},
                {QStringLiteral("x"), quantity(x, "m")},
                {QStringLiteral("tolerance"), quantity(1.0e-9, "m")}}},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("component"), QStringLiteral("ux")}});
        spec.insert(QStringLiteral("observables"), observables);
        if (QJsonDocument(spec).toJson(QJsonDocument::Compact).size() > 60 * 1024) {
            observables.removeLast();
            spec.insert(QStringLiteral("observables"), observables);
            break;
        }
    }

    const qsizetype problemSpecBytes = QJsonDocument(spec).toJson(QJsonDocument::Compact).size();
    QVERIFY(problemSpecBytes <= 60 * 1024);
    QVERIFY(observables.size() > 100);
    const auto mechanics = RTrustedMechanicsService::validate(spec);
    QVERIFY2(mechanics.valid, qPrintable(QJsonDocument(QJsonObject{
        {QStringLiteral("issues"), mechanics.issues}}).toJson(QJsonDocument::Compact)));

    const auto result = Service::validateStudy(spec, study("mesh", 3));
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("STUDY_MEASUREMENT_SIZE_LIMIT"));
    QVERIFY(result.data.value(QStringLiteral("maximumMeasurementBytes")).toDouble()
            > Service::MaximumConvergenceMeasurementBytes);
    QCOMPARE(result.data.value(QStringLiteral("maximumConvergenceMeasurementBytes")).toInt(),
             int(Service::MaximumConvergenceMeasurementBytes));
}

void TrustedConvergenceServiceTest::requiresTwoConsecutiveChangesBelowOnePercent()
{
    QJsonArray oneChange{qoiChanges(0.005, 0.009).first()};
    QVERIFY(!Service::isConverged(oneChange));
    QVERIFY(Service::isConverged(qoiChanges(0.005, 0.009)));
    QVERIFY(!Service::isConverged(qoiChanges(0.005, 0.01)));
    QVERIFY(!Service::isConverged(qoiChanges(0.005, 0.009), 3));

    QJsonArray undefined = qoiChanges(0.005, 0.009);
    QJsonObject last = undefined.last().toObject();
    QJsonArray qois = last.value(QStringLiteral("qoiChanges")).toArray();
    QJsonObject qoi = qois.first().toObject();
    qoi.insert(QStringLiteral("status"), QStringLiteral("undefined"));
    qois[0] = qoi;
    last.insert(QStringLiteral("qoiChanges"), qois);
    undefined[1] = last;
    QVERIFY(!Service::isConverged(undefined));
}

QTEST_GUILESS_MAIN(TrustedConvergenceServiceTest)

#include "tst_trusted_convergence_service.moc"
