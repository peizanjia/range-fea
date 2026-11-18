#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include <cmath>
#include <rbl_error.h>
#include <rsolver.h>
#include <rmesh_adaptivity.h>
#include <rmesh_sensitivity.h>
#include "mesh_sensitivity_dialog.h"

namespace {
constexpr double force = 1000.0;
constexpr double modulus = 2.0e11;
constexpr double length = 2.0;
constexpr double width = 0.1;
constexpr double thickness = 0.01;
constexpr double tipMm = 1000.0 * force * length / (modulus * width * thickness);

RElement element(RElementType type, std::initializer_list<uint> nodes)
{
    RElement value(type);
    uint i = 0;
    for (uint node : nodes) value.setNodeId(i++, node);
    return value;
}

RModel strip(bool triangular = false)
{
    RModel model;
    model.setName("Mesh sensitivity axial strip");
    for (uint x = 0; x < 3; ++x)
        for (uint y = 0; y < 2; ++y)
            model.addNode(RNode(double(x), double(y) * width, 0));
    for (uint cell = 0; cell < 2; ++cell)
    {
        const uint a = 2 * cell, b = a + 2, c = a + 3, d = a + 1;
        if (triangular)
        {
            model.addElement(element(R_ELEMENT_TRI1, {a, b, c}), true, 0);
            model.addElement(element(R_ELEMENT_TRI1, {a, c, d}), true, 0);
        }
        else model.addElement(element(R_ELEMENT_QUAD1, {a, b, c, d}), true, 0);
    }
    RMaterial material(RMaterial::Solid);
    RMaterialProperty elastic(RMaterialProperty::ModulusOfElasiticity);
    elastic.add(293.15, modulus); material.add(elastic);
    RMaterialProperty poisson(RMaterialProperty::PoissonRatio);
    poisson.add(293.15, 0.0); material.add(poisson);
    RMaterialProperty density(RMaterialProperty::Density);
    density.add(293.15, 7800.0); material.add(density);
    RMaterialProperty expansion(RMaterialProperty::ThermalExpansionCoefficient);
    expansion.add(293.15, 0.0); material.add(expansion);
    auto &surface = model.getSurface(0);
    surface.setMaterial(material);
    surface.setThickness(thickness);
    surface.setStructuralFormulation(RElementFormulation::PlaneStress);
    // The patch-test oracle has no body load; do not rely on gravity defaults.
    REnvironmentCondition gravity(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
    gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).add(0, 0);
    gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Y)).add(0, 0);
    gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Z)).add(0, 0);
    surface.addEnvironmentCondition(gravity);
    // Component constraints leave axial extension free over the entire strip.
    RBoundaryCondition lateral(R_BOUNDARY_CONDITION_DISPLACEMENT);
    lateral.getComponent(lateral.findComponentPosition(R_VARIABLE_DISPLACEMENT_X)).setEnabled(false);
    lateral.getComponent(lateral.findComponentPosition(R_VARIABLE_DISPLACEMENT_Y)).add(0, 0);
    lateral.getComponent(lateral.findComponentPosition(R_VARIABLE_DISPLACEMENT_Z)).add(0, 0);
    surface.addBoundaryCondition(lateral);
    model.addElement(element(R_ELEMENT_TRUSS1, {0, 1}), true, 0);
    model.getLine(0).setCrossArea(0);
    RBoundaryCondition root(R_BOUNDARY_CONDITION_DISPLACEMENT);
    root.getComponent(root.findComponentPosition(R_VARIABLE_DISPLACEMENT_X)).add(0, 0);
    root.getComponent(root.findComponentPosition(R_VARIABLE_DISPLACEMENT_Y)).setEnabled(false);
    root.getComponent(root.findComponentPosition(R_VARIABLE_DISPLACEMENT_Z)).setEnabled(false);
    model.getLine(0).addBoundaryCondition(root);
    model.addElement(element(R_ELEMENT_TRUSS1, {4, 5}), true, 1);
    model.getLine(1).setCrossArea(0);
    RBoundaryCondition load(R_BOUNDARY_CONDITION_FORCE);
    load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_X)).add(0, force);
    load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_Y)).add(0, 0);
    load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_Z)).add(0, 0);
    model.getLine(1).addBoundaryCondition(load);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.getTimeSolver().setEnabled(false);
    auto &cg = model.getMatrixSolverConf(RMatrixSolverConf::CG);
    cg.setNInnerIterations(1000); cg.setNOuterIterations(1000);
    cg.setSolverCvgValue(1e-12); cg.setOutputFrequency(0);
    return model;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString studyPath(const QString &root)
{
    const auto entries = QDir(root).entryList({"mesh-study-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    return entries.size() == 1 ? QDir(root).filePath(entries.first()) : QString();
}

QJsonObject reviewEvidence(MeshSensitivityDialog &dialog)
{
    QSignalSpy spy(&dialog, &MeshSensitivityDialog::reviewRequested);
    QTest::mouseClick(dialog.findChild<QPushButton *>("meshSensitivityReview"), Qt::LeftButton);
    if (spy.size() != 1) return {};
    const QByteArray question = spy.first().first().toString().toUtf8();
    return QJsonDocument::fromJson(question.mid(question.indexOf('\n') + 1)).object();
}
}

class TestMeshSensitivityDialog : public QObject
{
    Q_OBJECT
private:
    RModel baseline;
    QString artifactRoot;
    QString solver;
private slots:
    void initTestCase()
    {
        artifactRoot = qEnvironmentVariable("RANGE_MESH_TEST_ARTIFACTS");
        QVERIFY(QDir(artifactRoot).isAbsolute());
        solver = qEnvironmentVariable("RANGE_MESH_TEST_SOLVER");
        QVERIFY2(QFileInfo(solver).isExecutable(), "RANGE_MESH_TEST_SOLVER must name the real native solver");
        baseline = strip();
        try { RSolver native(baseline, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        const uint displacement = baseline.findVariable(R_VARIABLE_DISPLACEMENT);
        QVERIFY(displacement != RConstants::eod);
        QVERIFY(std::abs(baseline.getVariable(displacement).getValue(0, 4) * 1000 - tipMm) < 1e-7);
        // Include a point inside an element: nearest-node lookup cannot pass.
        baseline.getMonitoringPointManager().push_back(RMonitoringPoint(R_VARIABLE_DISPLACEMENT, RR3Vector(0.37, 0.035, 0)));
        baseline.getMonitoringPointManager().push_back(RMonitoringPoint(R_VARIABLE_DISPLACEMENT, RR3Vector(2, 0.05, 0)));
    }

    void refinementIsExplicitAndAcceptanceHasReceipt()
    {
        QTemporaryDir artifacts(artifactRoot + "/explicit-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(baseline, solver, artifacts.path(), "document-a", 7, [] { return true; }, true);
        dialog.show();
        QVERIFY(dialog.belongsTo({"document-a", 7}));
        QVERIFY(!dialog.belongsTo({"document-a", 8}));
        auto *table = dialog.findChild<QTableWidget *>("meshSensitivityPoints");
        auto *refine = dialog.findChild<QPushButton *>("meshSensitivityRefine");
        auto *accept = dialog.findChild<QPushButton *>("meshSensitivityAccept");
        auto *status = dialog.findChild<QLabel *>("meshSensitivityStatus");
        QVERIFY2(table->rowCount() == 2, qPrintable(status->text()));
        QVERIFY(refine->isEnabled());
        QTest::qWait(50);
        QVERIFY(!dialog.isRunning());
        QVERIFY(QDir(artifacts.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        const auto initial = reviewEvidence(dialog);
        QCOMPARE(initial.value("status").toString(), "no_comparison");
        QCOMPARE(initial.value("attempts").toArray().size(), 0);
        QCOMPARE(initial.value("levels").toArray().size(), 1);
        QVERIFY(!dialog.isRunning());
        QTest::mouseClick(refine, Qt::LeftButton);
        QVERIFY(!accept->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 120000);
        const QString directory = studyPath(artifacts.path());
        QVERIFY(!directory.isEmpty());
        auto receipt = readJson(directory + "/study.json");
        const auto attempts = receipt.value("attempts").toArray();
        QCOMPARE(attempts.size(), 1);
        QCOMPARE(attempts.first().toObject().value("status").toString(), "complete");
        QCOMPARE(receipt.value("maximumAttempts").toInt(), 3);
        QCOMPARE(receipt.value("maximumElements").toInt(), 200000);
        QCOMPARE(receipt.value("maximumSecondsPerSolve").toInt(), 120);
        const auto levels = receipt.value("levels").toArray();
        QCOMPARE(levels.size(), 2);
        QCOMPARE(levels[0].toObject().value("elements").toInt(), 4);
        QCOMPARE(levels[1].toObject().value("elements").toInt(), 12);
        const auto oldPoints = levels[0].toObject().value("points").toArray();
        const auto points = levels[1].toObject().value("points").toArray();
        QCOMPARE(points.size(), oldPoints.size());
        double maxDelta = 0;
        for (qsizetype i = 0; i < points.size(); ++i)
        {
            const auto point = points[i].toObject(), old = oldPoints[i].toObject();
            QCOMPARE(point.value("xM"), old.value("xM"));
            QCOMPARE(point.value("yM"), old.value("yM"));
            const double expected = tipMm * point.value("xM").toDouble() / length;
            QVERIFY(std::abs(point.value("uxMm").toDouble() - expected) < 1e-7);
            QVERIFY(std::abs(point.value("uyMm").toDouble()) < 1e-8);
            maxDelta = std::max(maxDelta, std::abs(point.value("uxMm").toDouble() - old.value("uxMm").toDouble()));
            maxDelta = std::max(maxDelta, std::abs(point.value("uyMm").toDouble() - old.value("uyMm").toDouble()));
        }
        QVERIFY(std::abs(receipt.value("maximumAdjacentChangeMm").toDouble() - maxDelta) < 1e-12);
        QCOMPARE(receipt.value("status").toString(), "within_tolerance");
        QVERIFY(status->text().contains("within tolerance"));
        QVERIFY(accept->isEnabled());
        if (qEnvironmentVariableIsSet("RANGE_MESH_CAPTURE_DIALOG"))
            QVERIFY(dialog.grab().save(artifactRoot + "/review-dialog.png"));
        QTest::mouseClick(accept, Qt::LeftButton);
        receipt = readJson(directory + "/study.json");
        QCOMPARE(receipt.value("status").toString(), "accepted_with_comparison");
        QCOMPARE(receipt.value("sourceRevision").toString(), "7");
        const auto decision = receipt.value("decision").toObject();
        QVERIFY(!decision.value("acceptedUtc").toString().isEmpty());
        QFile result(directory + "/" + decision.value("result").toString());
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(result.readAll(), QCryptographicHash::Sha256).toHex()),
                 decision.value("resultSha256").toString());
        QVERIFY(!refine->isEnabled());
        QSignalSpy opened(&dialog, &MeshSensitivityDialog::openResultRequested);
        QTest::mouseClick(dialog.findChild<QPushButton *>("meshSensitivityOpen"), Qt::LeftButton);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().first().toString(), result.fileName());
        // The dialog owns a detached copy; source mesh/results remain unchanged.
        QCOMPARE(baseline.getNNodes(), 6u);
        QCOMPARE(baseline.getNElements(), 4u);
    }

    void acceptBaselineSkipsAllSolves()
    {
        QTemporaryDir artifacts(artifactRoot + "/accept-baseline-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(baseline, solver, artifacts.path(), "document-reviewed", 1,
                                     [] { return true; }, true);
        dialog.show();
        auto *accept = dialog.findChild<QPushButton *>("meshSensitivityAccept");
        QVERIFY(accept->isEnabled());
        QTest::mouseClick(accept, Qt::LeftButton);
        QVERIFY(!dialog.isRunning());
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(receipt.value("status").toString(), "accepted_by_review");
        QVERIFY(receipt.value("attempts").toArray().isEmpty());
        QCOMPARE(receipt.value("levels").toArray().size(), 1);
    }

    void sourceRevisionDisablesMutationButAllowsEvidenceReview()
    {
        QTemporaryDir artifacts(artifactRoot + "/stale-XXXXXX");
        bool current = true;
        MeshSensitivityDialog dialog(baseline, solver, artifacts.path(), "document-b", 9, [&] { return current; }, true);
        dialog.show();
        current = false;
        dialog.refresh();
        QVERIFY(!dialog.findChild<QPushButton *>("meshSensitivityRefine")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton *>("meshSensitivityAccept")->isEnabled());
        const auto evidence = reviewEvidence(dialog);
        QCOMPARE(evidence.value("status").toString(), "stale_source");
        QCOMPARE(evidence.value("attempts").toArray().size(), 0);
        QVERIFY(!dialog.isRunning());
        QVERIFY(QDir(artifacts.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void localTriangularRefinementPreservesLoadsAndConformity()
    {
        QTemporaryDir artifacts(artifactRoot + "/local-tri-XXXXXX");
        QVERIFY(artifacts.isValid());
        RModel source = strip(true);
        constexpr double gravityX = 9.81;
        constexpr double densityValue = 7800.0;
        REnvironmentCondition gravity(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).add(0, gravityX);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Y)).add(0, 0);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Z)).add(0, 0);
        source.getSurface(0).addEnvironmentCondition(gravity);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        source.getMonitoringPointManager().push_back(
            RMonitoringPoint(R_VARIABLE_DISPLACEMENT, RR3Vector(length, width / 2, 0)));
        MeshSensitivityDialog dialog(source, solver, artifacts.path(), "document-local-tri", 3,
                                     [] { return true; }, true);
        dialog.show();
        auto *scope = dialog.findChild<QComboBox *>("meshSensitivityScope");
        QVERIFY(scope);
        const int localIndex = scope->findData("local");
        QVERIFY(localIndex >= 0);
        scope->setCurrentIndex(localIndex);
        // Expose the radius control through the same options toggle as a user.
        auto *options = dialog.findChild<QToolButton *>();
        QVERIFY(options);
        QTest::mouseClick(options, Qt::LeftButton);
        auto *radius = dialog.findChild<QDoubleSpinBox *>("meshSensitivityRadius");
        QVERIFY(radius->isVisible());
        radius->setValue(0.001);
        auto *refine = dialog.findChild<QPushButton *>("meshSensitivityRefine");
        QVERIFY(refine->isEnabled());
        QTest::mouseClick(refine, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 120000);
        const QString directory = studyPath(artifacts.path());
        const auto receipt = readJson(directory + "/study.json");
        const auto attempts = receipt.value("attempts").toArray();
        QCOMPARE(attempts.size(), 1);
        const auto attempt = attempts.first().toObject();
        QCOMPARE(attempt.value("status").toString(), "complete");
        QCOMPARE(attempt.value("scope").toString(), "local");
        QCOMPARE(attempt.value("radiusM").toDouble(), 0.001);
        QCOMPARE(attempt.value("pointM").toArray(), (QJsonArray{length, width / 2, 0.0}));
        const auto levels = receipt.value("levels").toArray();
        QCOMPARE(levels.size(), 2);
        const QString attemptDirectory = directory + "/" + attempt.value("directory").toString();
        RModel candidate, result;
        candidate.read(attemptDirectory + "/candidate.rbm");
        result.read(attemptDirectory + "/run-00000.rbm");
        QVERIFY(candidate.getNElements() > source.getNElements());
        // Four original TRI3 cells and two line carriers would become 20 elements
        // under full subdivision; this point must select a strictly smaller pass.
        QVERIFY(candidate.getNElements() < 4u * 4u + 2u * 2u);
        QVERIFY(candidate.getNElements() <= MeshSensitivityDialog::maximumElements);
        QCOMPARE(result.getNElements(), candidate.getNElements());
        QCOMPARE(result.getNNodes(), candidate.getNNodes());
        for (uint n = 0; n < source.getNNodes(); ++n)
        {
            QCOMPARE(candidate.getNode(n).getX(), source.getNode(n).getX());
            QCOMPARE(candidate.getNode(n).getY(), source.getNode(n).getY());
        }
        // No retained child edge may jump over any mesh node. This checks the
        // transition triangles as well as both sides of each original split edge.
        for (uint e = 0; e < candidate.getNElements(); ++e)
        {
            const auto &cell = candidate.getElement(e);
            const uint edgeCount = cell.getType() == R_ELEMENT_TRI1 ? 3u : 1u;
            for (uint edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex)
            {
                const uint aId = cell.getNodeId(edgeIndex), bId = cell.getNodeId((edgeIndex + 1) % cell.size());
                const auto &a = candidate.getNode(aId), &b = candidate.getNode(bId);
                const double dx = b.getX() - a.getX(), dy = b.getY() - a.getY();
                const double squaredLength = dx * dx + dy * dy;
                for (uint n = 0; n < candidate.getNNodes(); ++n)
                {
                    if (n == aId || n == bId) continue;
                    const auto &p = candidate.getNode(n);
                    const double px = p.getX() - a.getX(), py = p.getY() - a.getY();
                    const double fraction = (px * dx + py * dy) / squaredLength;
                    const bool hanging = fraction > 1e-9 && fraction < 1 - 1e-9
                        && std::abs(dx * py - dy * px) < 1e-10 * squaredLength;
                    QVERIFY2(!hanging, qPrintable(QString("Node %1 hangs on element %2 edge %3-%4")
                                                .arg(n).arg(e).arg(aId).arg(bId)));
                }
            }
        }
        const auto &surface = candidate.getSurface(0);
        const auto &material = surface.getMaterial();
        QCOMPARE(material.get(material.findPosition(RMaterialProperty::Density)).getValue(0), densityValue);
        const auto &retainedGravity = surface.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        QCOMPARE(retainedGravity.getComponent(retainedGravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).getValue(0), gravityX);
        double totalForce = 0;
        uint loadedSegments = 0;
        for (uint g = 0; g < candidate.getNLines(); ++g)
        {
            const auto &line = candidate.getLine(g);
            if (!line.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
            const auto &load = line.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
            totalForce += line.size() * load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_X)).getValue(0);
            loadedSegments += line.size();
        }
        QCOMPARE(loadedSegments, 2u);
        QCOMPARE(totalForce, force);
        // Reciprocal work with the exact affine unit-traction solution gives
        // this mean tip displacement, including the nonzero distributed gravity.
        const double expectedMm = tipMm + 1000 * densityValue * gravityX * length * length / (2 * modulus);
        const uint displacement = result.findVariable(R_VARIABLE_DISPLACEMENT);
        QVERIFY(displacement != RConstants::eod);
        uint midpoint = RConstants::eod;
        for (uint n = 0; n < result.getNNodes(); ++n)
            if (result.getNode(n).getX() == length && result.getNode(n).getY() == width / 2)
                midpoint = n;
        QVERIFY(midpoint != RConstants::eod);
        const auto &values = result.getVariable(displacement);
        const double meanTipMm = 1000 * (0.25 * values.getValue(0, 4)
            + 0.5 * values.getValue(0, midpoint) + 0.25 * values.getValue(0, 5));
        QVERIFY2(std::abs(meanTipMm - expectedMm) < 1e-7,
                 qPrintable(QString("Mean tip %1 mm; force+gravity oracle %2 mm").arg(meanTipMm, 0, 'g', 15).arg(expectedMm, 0, 'g', 15)));
        const auto points = levels.last().toObject().value("points").toArray();
        QCOMPARE(points.size(), 1);
        const auto point = points.first().toObject();
        QCOMPARE(point.value("xM").toDouble(), length);
        QCOMPARE(point.value("yM").toDouble(), width / 2);
        QVERIFY(std::abs(point.value("uxMm").toDouble() - 1000 * values.getValue(0, midpoint)) < 1e-10);
        QVERIFY(std::abs(point.value("uyMm").toDouble()) < 1e-8);
        QCOMPARE(source.getNNodes(), 6u);
        QCOMPARE(source.getNElements(), 6u);
    }

    void constantStrainAdaptiveEstimateNeedsNoSolve()
    {
        RModel source = strip(true);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        const auto estimate = RMeshAdaptivity::estimate(source, 0.5);
        QVERIFY(std::isfinite(estimate.relativeError));
        QVERIFY2(estimate.relativeError < 1e-8,
                 qPrintable(QString("Constant-strain relative error: %1").arg(estimate.relativeError, 0, 'g', 15)));
        QTemporaryDir artifacts(artifactRoot + "/adaptive-affine-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(source, solver, artifacts.path(), "adaptive-affine", 1,
                                     [] { return true; }, true);
        dialog.show();
        QTest::qWait(50);
        QVERIFY(!dialog.isRunning());
        QVERIFY(QDir(artifacts.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(dialog.startAdaptive(0.1));
        QTest::qWait(50);
        QVERIFY(!dialog.isRunning());
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QVERIFY(receipt.value("attempts").toArray().isEmpty());
        QCOMPARE(receipt.value("levels").toArray().size(), 1);
        const auto adaptive = receipt.value("adaptive").toObject();
        QCOMPARE(adaptive.value("status").toString(), "converged");
        QCOMPARE(adaptive.value("tolerancePercent").toDouble(), 0.1);
        QCOMPARE(adaptive.value("estimates").toArray().size(), 1);

        QVERIFY(dialog.startAdaptive(0.05));
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
        const auto reassessed = readJson(studyPath(artifacts.path()) + "/study.json");
        const auto newReview = reassessed.value("adaptive").toObject();
        QCOMPARE(newReview.value("status").toString(), "converged");
        QCOMPARE(newReview.value("tolerancePercent").toDouble(), 0.05);
        const auto history = newReview.value("previousReviews").toArray();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().toObject().value("tolerancePercent").toDouble(), 0.1);
        QVERIFY(reassessed.value("attempts").toArray().isEmpty());
        if (qEnvironmentVariableIsSet("RANGE_MESH_CAPTURE_DIALOG"))
            QVERIFY(dialog.grab().save(artifactRoot + "/adaptive-dialog.png"));
        auto *accept = dialog.findChild<QPushButton *>("meshSensitivityAccept");
        QVERIFY(accept->isEnabled());
        QTest::mouseClick(accept, Qt::LeftButton);
        const auto accepted = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(accepted.value("status").toString(), "accepted_with_adaptive_estimate");
        QVERIFY(accepted.value("attempts").toArray().isEmpty());
    }

    void fullyPrescribedGravityHasZeroFreeResidualAndNoAdaptiveSolve()
    {
        RModel source = strip(true);
        auto &surface = source.getSurface(0);
        auto &fixed = surface.getBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT);
        auto &x = fixed.getComponent(fixed.findComponentPosition(R_VARIABLE_DISPLACEMENT_X));
        x.setEnabled(true);
        x.add(0, 0);
        auto &gravity = surface.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).add(0, 9.81);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_Y)).add(0, -9.81);
        source.getLine(1).removeBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        const auto estimate = RMeshAdaptivity::estimate(source, 0.5);
        QCOMPARE(estimate.strainEnergy, 0.0);
        QCOMPARE(estimate.relativeError, 0.0);
        QCOMPARE(estimate.indicatorEnergy, 0.0);
        QVERIFY(estimate.markedElements.empty());
        QTemporaryDir artifacts(artifactRoot + "/adaptive-prescribed-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(source, solver, artifacts.path(), "adaptive-prescribed", 1,
                                     [] { return true; }, true);
        dialog.show();
        QVERIFY(dialog.startAdaptive(0.1));
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(receipt.value("adaptive").toObject().value("status").toString(), "converged");
        QVERIFY(receipt.value("attempts").toArray().isEmpty());
        QCOMPARE(receipt.value("levels").toArray().size(), 1);
    }

    void materialInterfaceWithBalancedTractionHasNoResidual()
    {
        RModel source = strip(true);
        // Two axial segments have different strains but continuous normal traction.
        // A displacement-gradient jump at this interface is therefore not an error.
        RSurface second = source.getSurface(0);
        second.resize(0);
        second.add(2);
        second.add(3);
        RMaterial softer = second.getMaterial();
        RMaterialProperty elastic(RMaterialProperty::ModulusOfElasiticity);
        elastic.add(293.15, modulus / 2);
        softer.add(elastic);
        second.setMaterial(softer);
        source.getSurface(0).resize(2);
        source.addSurface(second);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        const uint displacement = source.findVariable(R_VARIABLE_DISPLACEMENT);
        QVERIFY(displacement != RConstants::eod);
        const auto &values = source.getVariable(displacement);
        const double unitStrain = force / (modulus * width * thickness);
        QVERIFY(std::abs(values.getValue(0, 2) - unitStrain) < 1e-12);
        QVERIFY(std::abs(values.getValue(0, 4) - 3 * unitStrain) < 1e-12);
        const auto estimate = RMeshAdaptivity::estimate(source, 0.5);
        QVERIFY(std::isfinite(estimate.relativeError));
        QVERIFY2(estimate.relativeError < 1e-8,
                 qPrintable(QString("Balanced interface relative error: %1").arg(estimate.relativeError, 0, 'g', 15)));
    }

    void tangentialStressJumpAcrossMaterialInterfaceHasNoResidual()
    {
        RModel source = strip(true);
        for (uint layer = 0; layer < 3; ++layer)
            for (uint side = 0; side < 2; ++side)
                source.setNode(2 * layer + side, RNode(side * length, layer * width / 2, 0));
        source.setElement(0, element(R_ELEMENT_TRI1, {0, 1, 3}), false);
        source.setElement(1, element(R_ELEMENT_TRI1, {0, 3, 2}), false);
        source.setElement(2, element(R_ELEMENT_TRI1, {2, 3, 5}), false);
        source.setElement(3, element(R_ELEMENT_TRI1, {2, 5, 4}), false);
        RSurface upper = source.getSurface(0);
        upper.resize(0);
        upper.add(2);
        upper.add(3);
        RMaterial softer = upper.getMaterial();
        RMaterialProperty elastic(RMaterialProperty::ModulusOfElasiticity);
        elastic.add(293.15, modulus / 2);
        softer.add(elastic);
        upper.setMaterial(softer);
        source.getSurface(0).resize(2);
        source.addSurface(upper);
        source.setElement(4, element(R_ELEMENT_TRUSS1, {0, 2}), false);
        source.addElement(element(R_ELEMENT_TRUSS1, {2, 4}), true, 0);
        source.setElement(5, element(R_ELEMENT_TRUSS1, {1, 3}), false);
        source.addElement(element(R_ELEMENT_TRUSS1, {3, 5}), true, 2);
        source.getLine(2).setCrossArea(0);
        RBoundaryCondition upperLoad = source.getLine(1).getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        upperLoad.getComponent(upperLoad.findComponentPosition(R_VARIABLE_FORCE_X)).add(0, force / 2);
        source.getLine(2).addBoundaryCondition(upperLoad);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        const uint displacement = source.findVariable(R_VARIABLE_DISPLACEMENT);
        QVERIFY(displacement != RConstants::eod);
        const double strain = force / (modulus * (width / 2) * thickness);
        for (uint n = 0; n < source.getNNodes(); ++n)
            QVERIFY(std::abs(source.getVariable(displacement).getValue(0, n) - strain * source.getNode(n).getX()) < 1e-12);
        // sigma_x differs by two across the horizontal interface, while the
        // normal traction (sigma_xy, sigma_y) is zero on both sides.
        const auto estimate = RMeshAdaptivity::estimate(source, 0.5);
        QVERIFY(std::isfinite(estimate.relativeError));
        QVERIFY2(estimate.relativeError < 1e-8,
                 qPrintable(QString("Tangential stress-jump relative error: %1").arg(estimate.relativeError, 0, 'g', 15)));
    }

    void cancelQueuedAdaptivePassLaunchesNoSolver()
    {
        RModel source = strip(true);
        auto &gravity = source.getSurface(0).getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).add(0, 981000.0);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        QTemporaryDir artifacts(artifactRoot + "/adaptive-cancel-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(source, solver, artifacts.path(), "adaptive-cancel", 1,
                                     [] { return true; }, true);
        dialog.show();
        QVERIFY(dialog.startAdaptive(1e-5));
        QVERIFY(dialog.isRunning());
        auto *cancel = dialog.findChild<QPushButton *>("meshSensitivityCancel");
        QVERIFY(cancel->isVisible());
        // Invoke cancellation before the queued estimator/refinement callback.
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
        QTest::qWait(50);
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(receipt.value("adaptive").toObject().value("status").toString(), "cancelled");
        QVERIFY(receipt.value("attempts").toArray().isEmpty());
        QCOMPARE(receipt.value("levels").toArray().size(), 1);
        QCOMPARE(source.getNElements(), 6u);
    }

    void gravityAdaptiveLoopPreservesPhysicalLoad()
    {
        RModel source = strip(true);
        constexpr double gravityX = 981000.0;
        constexpr double densityValue = 7800.0;
        auto &gravity = source.getSurface(0).getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        gravity.getComponent(gravity.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).add(0, gravityX);
        try { RSolver native(source, QString(), QString()); native.run(); }
        catch (const RError &error) { QFAIL(qPrintable(error.getMessage())); }
        const auto initial = RMeshAdaptivity::estimate(source, 0.5);
        QVERIFY(std::isfinite(initial.relativeError));
        QVERIFY(initial.relativeError > 1e-6);
        QVERIFY(!initial.markedElements.empty());
        QCOMPARE(RMeshAdaptivity::estimate(source, 0.5).markedElements, initial.markedElements);
        QVERIFY_EXCEPTION_THROWN(RMeshSensitivity::refineMarked(source, initial.markedElements,
                                                                source.getNElements()),
                                 RMeshSensitivity::RefinementLimit);
        QVERIFY_EXCEPTION_THROWN(RMeshSensitivity::refineMarked(source, initial.markedElements, 1000u, length),
                                 RMeshSensitivity::RefinementLimit);
        QTemporaryDir artifacts(artifactRoot + "/adaptive-gravity-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(source, solver, artifacts.path(), "adaptive-gravity", 1,
                                     [] { return true; }, true);
        dialog.show();
        const double tolerancePercent = 1e-5;
        QVERIFY(dialog.startAdaptive(tolerancePercent));
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 120000);
        // A finished solve may queue the next pass through the event loop.
        QTRY_VERIFY_WITH_TIMEOUT(
            readJson(studyPath(artifacts.path()) + "/study.json").value("adaptive").toObject()
                .value("status").toString() != "running", 120000);
        const QString directory = studyPath(artifacts.path());
        const auto receipt = readJson(directory + "/study.json");
        const auto attempts = receipt.value("attempts").toArray();
        QVERIFY(!attempts.isEmpty());
        QVERIFY(attempts.size() <= MeshSensitivityDialog::maximumAttempts);
        const auto adaptive = receipt.value("adaptive").toObject();
        const QString status = adaptive.value("status").toString();
        QVERIFY2(status == "converged" || status == "budget" || status == "stagnated",
                 qPrintable(QString::fromUtf8(QJsonDocument(receipt).toJson())));
        QCOMPARE(adaptive.value("tolerancePercent").toDouble(), tolerancePercent);
        const auto estimates = adaptive.value("estimates").toArray();
        QCOMPARE(estimates.size(), attempts.size() + 1);
        for (const auto &entry : estimates)
        {
            const auto estimate = entry.toObject();
            QVERIFY(estimate.value("relativeError").isDouble());
            QVERIFY(std::isfinite(estimate.value("relativeError").toDouble()));
            QVERIFY(estimate.value("relativeError").toDouble() >= 0);
        }
        QVERIFY(estimates.first().toObject().value("markedCount").toInt() > 0);
        QVERIFY2(estimates.last().toObject().value("relativeError").toDouble() < initial.relativeError
                     || status == "budget", qPrintable("Estimator neither decreased nor exhausted the documented budget"));
        if (status == "converged")
            QVERIFY(100 * estimates.last().toObject().value("relativeError").toDouble() <= tolerancePercent);
        if (status == "budget") QCOMPARE(attempts.size(), MeshSensitivityDialog::maximumAttempts);
        for (const auto &entry : attempts)
        {
            const auto attempt = entry.toObject();
            QCOMPARE(attempt.value("scope").toString(), "adaptive");
            QCOMPARE(attempt.value("status").toString(), "complete");
            const QString attemptDirectory = directory + "/" + attempt.value("directory").toString();
            RModel candidate, result;
            candidate.read(attemptDirectory + "/candidate.rbm");
            result.read(attemptDirectory + "/run-00000.rbm");
            QVERIFY(candidate.getNElements() > source.getNElements());
            QVERIFY(candidate.getNElements() <= MeshSensitivityDialog::maximumElements);
            QCOMPARE(result.getNElements(), candidate.getNElements());
            const auto finalEstimate = RMeshAdaptivity::estimate(result, 0.5);
            QVERIFY(std::isfinite(finalEstimate.relativeError));
            double boundaryForce = 0;
            for (uint g = 0; g < candidate.getNLines(); ++g)
            {
                const auto &line = candidate.getLine(g);
                if (!line.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
                const auto &load = line.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
                boundaryForce += line.size() * load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_X)).getValue(0);
            }
            QVERIFY(std::abs(boundaryForce - force) < 1e-9);
            double bodyForce = 0;
            for (uint g = 0; g < candidate.getNSurfaces(); ++g)
            {
                const auto &surface = candidate.getSurface(g);
                const auto &material = surface.getMaterial();
                const double rho = material.get(material.findPosition(RMaterialProperty::Density)).getValue(0);
                const auto &condition = surface.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
                const double acceleration = condition.getComponent(condition.findComponentPosition(R_VARIABLE_G_ACCELERATION_X)).getValue(0);
                for (uint i = 0; i < surface.size(); ++i)
                {
                    const auto &cell = candidate.getElement(surface.get(i));
                    QCOMPARE(cell.getType(), R_ELEMENT_TRI1);
                    const auto &a = candidate.getNode(cell.getNodeId(0));
                    const auto &b = candidate.getNode(cell.getNodeId(1));
                    const auto &c = candidate.getNode(cell.getNodeId(2));
                    const double area = 0.5 * std::abs((b.getX() - a.getX()) * (c.getY() - a.getY())
                        - (b.getY() - a.getY()) * (c.getX() - a.getX()));
                    bodyForce += area * surface.getThickness() * rho * acceleration;
                }
            }
            const double expectedBodyForce = length * width * thickness * densityValue * gravityX;
            QVERIFY(std::abs(bodyForce - expectedBodyForce) < 1e-8 * expectedBodyForce);
            QVERIFY(std::abs(boundaryForce + bodyForce - (force + expectedBodyForce)) < 1e-8 * expectedBodyForce);
        }
        QCOMPARE(source.getNNodes(), 6u);
        QCOMPARE(source.getNElements(), 6u);
    }

    void changedLoadsRequireABaselineSolve()
    {
        QTemporaryDir artifacts(artifactRoot + "/baseline-XXXXXX");
        QVERIFY(artifacts.isValid());
        const QString solvedFile = artifacts.path() + "/saved.rbm";
        baseline.write(solvedFile, false);
        QFile saved(solvedFile);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(saved.readAll(), QCryptographicHash::Sha256).toHex());
        QVERIFY(MeshSensitivityDialog::matchesSolvedSnapshot(baseline, solvedFile, digest));
        RModel edited(baseline);
        auto &load = edited.getLine(1).getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_X)).add(0, 2 * force);
        QVERIFY(!MeshSensitivityDialog::matchesSolvedSnapshot(edited, solvedFile, digest));
        MeshSensitivityDialog dialog(edited, solver, artifacts.path(), "document-edited", 9,
                                     [] { return true; }, true, nullptr, false);
        dialog.show();
        auto *refine = dialog.findChild<QPushButton *>("meshSensitivityRefine");
        auto *accept = dialog.findChild<QPushButton *>("meshSensitivityAccept");
        QVERIFY(!accept->isEnabled());
        QCOMPARE(reviewEvidence(dialog).value("status").toString(), "unverified_baseline");
        QCOMPARE(refine->text(), "Solve baseline");
        QTest::mouseClick(refine, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(receipt.value("baselineProof").toString(), "recomputed");
        QCOMPARE(receipt.value("status").toString(), "no_comparison");
        const auto levels = receipt.value("levels").toArray();
        QCOMPARE(levels.size(), 1);
        const auto points = levels.first().toObject().value("points").toArray();
        QVERIFY(std::abs(points.last().toObject().value("uxMm").toDouble() - 2 * tipMm) < 1e-7);
        QVERIFY(accept->isEnabled());
    }

    void cancelPreservesPreviousLevel()
    {
        QTemporaryDir artifacts(artifactRoot + "/cancel-XXXXXX");
        QVERIFY(artifacts.isValid());
        MeshSensitivityDialog dialog(baseline, QCoreApplication::applicationFilePath(), artifacts.path(),
                                     "document-cancel", 1, [] { return true; }, true);
        dialog.show();
        qputenv("RANGE_MESH_TEST_SLOW_CHILD", "1");
        QTest::mouseClick(dialog.findChild<QPushButton *>("meshSensitivityRefine"), Qt::LeftButton);
        qunsetenv("RANGE_MESH_TEST_SLOW_CHILD");
        QVERIFY(dialog.isRunning());
        QTest::mouseClick(dialog.findChild<QPushButton *>("meshSensitivityCancel"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(receipt.value("levels").toArray().size(), 1);
        QCOMPARE(receipt.value("attempts").toArray().first().toObject().value("status").toString(), "cancelled");
    }

    void courseworkAdaptiveVersusGlobalWhenProvided()
    {
        const QString path = qEnvironmentVariable("RANGE_MESH_COURSEWORK_RESULT");
        if (path.isEmpty()) QSKIP("Set RANGE_MESH_COURSEWORK_RESULT for the real adaptive/global benchmark.");
        RModel source;
        source.read(path);
        const QString unsupported = RMeshAdaptivity::eligibility(source);
        if (!unsupported.isEmpty()) QSKIP(qPrintable(unsupported));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString digest = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
        QVERIFY(MeshSensitivityDialog::hasNativeCompletion(path, digest));
        QVERIFY(MeshSensitivityDialog::matchesSolvedSnapshot(source, path, digest));
        const auto initial = RMeshAdaptivity::estimate(source, 0.5);
        QVERIFY(std::isfinite(initial.relativeError));
        if (initial.relativeError <= 0) QSKIP("The supplied coursework baseline has no residual to refine.");
        QTemporaryDir artifacts(artifactRoot + "/coursework-adaptive-global-XXXXXX");
        QVERIFY(artifacts.isValid());
        if (qEnvironmentVariableIsSet("RANGE_MESH_KEEP_COURSEWORK")) artifacts.setAutoRemove(false);
        QVERIFY(QDir().mkpath(artifacts.path() + "/adaptive"));
        QVERIFY(QDir().mkpath(artifacts.path() + "/global"));
        MeshSensitivityDialog automatic(source, solver, artifacts.path() + "/adaptive", "coursework-adaptive", 1,
                                        [] { return true; }, true);
        automatic.show();
        // Usually 20%; a baseline already below that target still gets one pass.
        const double targetPercent = std::min(20.0, initial.relativeError * 50.0);
        QElapsedTimer automaticTimer;
        automaticTimer.start();
        QVERIFY(automatic.startAdaptive(targetPercent));
        QTRY_VERIFY_WITH_TIMEOUT(!automatic.isRunning(), 450000);
        const qint64 automaticMs = automaticTimer.elapsed();
        const QString automaticDirectory = studyPath(artifacts.path() + "/adaptive");
        const auto automaticReceipt = readJson(automaticDirectory + "/study.json");
        const auto attempts = automaticReceipt.value("attempts").toArray();
        QVERIFY(!attempts.isEmpty());
        const auto estimates = automaticReceipt.value("adaptive").toObject().value("estimates").toArray();
        QCOMPARE(estimates.size(), attempts.size() + 1);
        QCOMPARE(automaticReceipt.value("levels").toArray().size(), attempts.size() + 1);
        for (qsizetype i = 0; i < attempts.size(); ++i)
        {
            const auto attempt = attempts.at(i).toObject();
            QCOMPARE(attempt.value("scope").toString(), "adaptive");
            QCOMPARE(attempt.value("status").toString(), "complete");
            const QString attemptDirectory = automaticDirectory + "/" + attempt.value("directory").toString();
            QVERIFY(QFileInfo::exists(attemptDirectory + "/candidate.rbm"));
            RModel result;
            result.read(attemptDirectory + "/run-00000.rbm");
            const auto estimate = RMeshAdaptivity::estimate(result, 0.5);
            QVERIFY(std::isfinite(estimate.relativeError));
            const double recorded = estimates.at(i + 1).toObject().value("relativeError").toDouble();
            QVERIFY(std::abs(recorded - estimate.relativeError) < 1e-10 * std::max(1.0, recorded));
            qInfo().noquote() << "Coursework adaptive level" << i + 1 << "eta" << estimate.relativeError
                              << "elements" << result.getNElements() << "nodes" << result.getNNodes();
        }
        const QString terminal = automaticReceipt.value("adaptive").toObject().value("status").toString();
        QVERIFY2(terminal == "converged" || terminal == "budget" || terminal == "stagnated", qPrintable(terminal));
        qInfo().noquote() << "Coursework adaptive baseline eta" << initial.relativeError << "elements" << source.getNElements()
                          << "nodes" << source.getNNodes() << "targetPercent" << targetPercent
                          << "status" << terminal << "totalMs" << automaticMs << "evidence" << automaticDirectory;

        MeshSensitivityDialog global(source, solver, artifacts.path() + "/global", "coursework-global", 1,
                                     [] { return true; }, true);
        global.show();
        auto *scope = global.findChild<QComboBox *>("meshSensitivityScope");
        QVERIFY(scope);
        const int globalIndex = scope->findData("global");
        QVERIFY(globalIndex >= 0);
        scope->setCurrentIndex(globalIndex);
        auto *refine = global.findChild<QPushButton *>("meshSensitivityRefine");
        QVERIFY(refine->isEnabled());
        QElapsedTimer globalTimer;
        globalTimer.start();
        QTest::mouseClick(refine, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!global.isRunning(), 150000);
        const qint64 globalMs = globalTimer.elapsed();
        const QString globalDirectory = studyPath(artifacts.path() + "/global");
        const auto globalReceipt = readJson(globalDirectory + "/study.json");
        const auto globalAttempts = globalReceipt.value("attempts").toArray();
        QCOMPARE(globalAttempts.size(), 1);
        const auto globalAttempt = globalAttempts.first().toObject();
        QCOMPARE(globalAttempt.value("scope").toString(), "global");
        QCOMPARE(globalAttempt.value("status").toString(), "complete");
        QCOMPARE(globalReceipt.value("levels").toArray().size(), 2);
        RModel globalResult;
        globalResult.read(globalDirectory + "/" + globalAttempt.value("directory").toString() + "/run-00000.rbm");
        QVERIFY(globalResult.getNElements() > source.getNElements());
        const auto globalEstimate = RMeshAdaptivity::estimate(globalResult, 0.5);
        QVERIFY(std::isfinite(globalEstimate.relativeError));
        qInfo().noquote() << "Coursework global h/2 eta" << globalEstimate.relativeError
                          << "elements" << globalResult.getNElements() << "nodes" << globalResult.getNNodes()
                          << "totalMs" << globalMs << "evidence" << globalDirectory;
        // The independent global comparison must leave all adaptive evidence intact.
        QCOMPARE(readJson(automaticDirectory + "/study.json"), automaticReceipt);
    }

    void courseworkWhenProvided()
    {
        const QString path = qEnvironmentVariable("RANGE_MESH_COURSEWORK_RESULT");
        if (path.isEmpty()) QSKIP("Set RANGE_MESH_COURSEWORK_RESULT for the local five-material test.");
        RModel source;
        source.read(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString digest = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
        QVERIFY(MeshSensitivityDialog::hasNativeCompletion(path, digest));
        QVERIFY(MeshSensitivityDialog::matchesSolvedSnapshot(source, path, digest));
        QTemporaryDir artifacts(artifactRoot + "/coursework-XXXXXX");
        QVERIFY(artifacts.isValid());
        if (qEnvironmentVariableIsSet("RANGE_MESH_KEEP_COURSEWORK"))
            artifacts.setAutoRemove(false);
        MeshSensitivityDialog dialog(source, solver, artifacts.path(), "coursework", 1, [] { return true; }, true);
        dialog.show();
        auto *refine = dialog.findChild<QPushButton *>("meshSensitivityRefine");
        auto *status = dialog.findChild<QLabel *>("meshSensitivityStatus");
        auto *tolerance = dialog.findChild<QDoubleSpinBox *>("meshSensitivityTolerance");
        QVERIFY2(refine->isEnabled(), qPrintable(status->text()));
        tolerance->setValue(0.01);
        QTest::mouseClick(refine, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 90000);
        const QString directory = studyPath(artifacts.path());
        const auto study = readJson(directory + "/study.json");
        qInfo().noquote() << "Coursework evidence:" << directory
                          << study.value("maximumAdjacentChangeMm").toDouble() << "mm";
        const auto attempts = study.value("attempts").toArray();
        QVERIFY2(!attempts.isEmpty() && attempts.first().toObject().value("status") == "complete",
                 qPrintable(status->text()));
        QCOMPARE(study.value("status").toString(), "above_tolerance");
        QVERIFY(!dialog.findChild<QPushButton *>("meshSensitivityAccept")->isEnabled());
        const auto levels = study.value("levels").toArray();
        QCOMPARE(levels.last().toObject().value("points").toArray().size(), 3);
        auto *scope = dialog.findChild<QComboBox *>("meshSensitivityScope");
        if (scope->currentData() == QStringLiteral("local"))
        {
            auto *radius = dialog.findChild<QDoubleSpinBox *>("meshSensitivityRadius");
            radius->setValue(radius->value()/2);
        }
        QTest::mouseClick(refine, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 90000);
        const auto second = readJson(directory + "/study.json");
        const auto secondAttempts = second.value("attempts").toArray();
        QCOMPARE(secondAttempts.size(), 2);
        QCOMPARE(secondAttempts.last().toObject().value("status").toString(), "complete");
        QCOMPARE(second.value("levels").toArray().size(), 3);
        QVERIFY(second.value("maximumAdjacentChangeMm").toDouble() <= 0.01);
        qInfo() << "Second coursework pass, maximum probe displacement change (mm):"
                << second.value("maximumAdjacentChangeMm").toDouble();
        auto *accept = dialog.findChild<QPushButton *>("meshSensitivityAccept");
        QVERIFY2(accept->isEnabled(), qPrintable(status->text()));
        QTest::mouseClick(accept, Qt::LeftButton);
        QCOMPARE(readJson(directory + "/study.json").value("status").toString(), "accepted_with_comparison");
    }

    void failedAttemptsPreserveBaselineAndExhaustBudget()
    {
        QTemporaryDir artifacts(artifactRoot + "/failure-XXXXXX");
        // An executable that deterministically exits unsuccessfully exercises QProcess
        // failure without fabricating a result or depending on network services.
        MeshSensitivityDialog dialog(baseline, QCoreApplication::applicationFilePath(), artifacts.path(), "document-c", 1, [] { return true; }, true);
        dialog.show();
        auto *refine = dialog.findChild<QPushButton *>("meshSensitivityRefine");
        for (int i = 0; i < 3; ++i)
        {
            QVERIFY(refine->isEnabled());
            QTest::mouseClick(refine, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
            const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
            QCOMPARE(receipt.value("levels").toArray().size(), 1);
            const auto attempts = receipt.value("attempts").toArray();
            QCOMPARE(attempts.size(), i + 1);
            QCOMPARE(attempts.last().toObject().value("status").toString(), "failed");
        }
        QVERIFY(!refine->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>("meshSensitivityStatus")->text().contains("budget"));
        QTest::mouseClick(dialog.findChild<QPushButton *>("meshSensitivityAccept"), Qt::LeftButton);
        const auto receipt = readJson(studyPath(artifacts.path()) + "/study.json");
        QCOMPARE(receipt.value("status").toString(), "accepted_by_review");
        QCOMPARE(receipt.value("decision").toObject().value("result").toString(), "baseline.rbm");
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]).startsWith("--file="))
    {
        if (qEnvironmentVariableIsSet("RANGE_MESH_TEST_SLOW_CHILD"))
            QThread::msleep(15000);
        return 1;
    }
    QString root = qEnvironmentVariable("RANGE_MESH_TEST_ARTIFACTS");
    if (root.isEmpty()) root = QDir::current().absoluteFilePath("artifacts/mesh-sensitivity-dialog-tests");
    root = QDir(root).absolutePath();
    for (const QString &child : {QString("tmp"), QString("config"), QString("cache"), QString("runtime")})
        if (!QDir().mkpath(root + "/" + child)) return 2;
    qputenv("RANGE_MESH_TEST_ARTIFACTS", root.toUtf8());
    qputenv("TMPDIR", (root + "/tmp").toUtf8());
    qputenv("TMP", (root + "/tmp").toUtf8());
    qputenv("TEMP", (root + "/tmp").toUtf8());
    qputenv("XDG_CONFIG_HOME", (root + "/config").toUtf8());
    qputenv("XDG_CACHE_HOME", (root + "/cache").toUtf8());
    qputenv("XDG_RUNTIME_DIR", (root + "/runtime").toUtf8());
    QApplication application(argc, argv);
    TestMeshSensitivityDialog test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mesh_sensitivity_dialog.moc"
