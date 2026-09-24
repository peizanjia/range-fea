#include <QtTest>

#include "solver_input.h"

namespace
{
class InspectableSolverInput : public SolverInput
{
public:
    using SolverInput::SolverInput;

    QString monitoringFile() const { return this->monitoringFileName; }
    bool restartEnabled() const { return this->restart; }
};
}

class TestSolverInput : public QObject
{
    Q_OBJECT

private slots:
    void omittedThreadOptionDoesNotBecomeAnOverride();
    void explicitOverrideAndOtherOptionsSurviveCopyAndAssignment();
    void assigningDefaultClearsAnOldOverride();
    void controlTransportOptionsSurviveCopyAndAssignment();
};

void TestSolverInput::omittedThreadOptionDoesNotBecomeAnOverride()
{
    InspectableSolverInput input(QStringLiteral("input.rbm"));
    QVERIFY(!input.getNThreadsOverride().has_value());
    QVERIFY(!input.restartEnabled());
    QVERIFY(input.monitoringFile().isEmpty());
}

void TestSolverInput::explicitOverrideAndOtherOptionsSurviveCopyAndAssignment()
{
    InspectableSolverInput original(QStringLiteral("input.rbm"));
    original.setNThreads(4);
    original.setMonitoringFileName(QStringLiteral("trace.mon"));
    original.setRestart(true);

    InspectableSolverInput copy(original);
    QVERIFY(copy.getNThreadsOverride().has_value());
    QCOMPARE(*copy.getNThreadsOverride(),4u);
    QCOMPARE(copy.monitoringFile(),QStringLiteral("trace.mon"));
    QVERIFY(copy.restartEnabled());

    InspectableSolverInput assigned(QStringLiteral("other.rbm"));
    assigned = original;
    QVERIFY(assigned.getNThreadsOverride().has_value());
    QCOMPARE(*assigned.getNThreadsOverride(),4u);
    QCOMPARE(assigned.monitoringFile(),QStringLiteral("trace.mon"));
    QVERIFY(assigned.restartEnabled());
}

void TestSolverInput::assigningDefaultClearsAnOldOverride()
{
    InspectableSolverInput defaultInput(QStringLiteral("default.rbm"));
    InspectableSolverInput assigned(QStringLiteral("override.rbm"));
    assigned.setNThreads(0); // Explicit zero is distinct from omission.
    QVERIFY(assigned.getNThreadsOverride().has_value());

    assigned = defaultInput;
    QVERIFY(!assigned.getNThreadsOverride().has_value());
    QVERIFY(!assigned.restartEnabled());
    QVERIFY(assigned.monitoringFile().isEmpty());
}

void TestSolverInput::controlTransportOptionsSurviveCopyAndAssignment()
{
    InspectableSolverInput original(QStringLiteral("input.rbm"));
    original.setExternalControlProgram(QStringLiteral("python.exe"),
                                       {QStringLiteral("controller.py"),QStringLiteral("--strict")});
    original.setExternalControlTimeoutMs(1250);
    InspectableSolverInput copy(original);
    QCOMPARE(copy.getExternalControlProgram(),QStringLiteral("python.exe"));
    QCOMPARE(copy.getExternalControlArguments(),
             QStringList({QStringLiteral("controller.py"),QStringLiteral("--strict")}));
    QCOMPARE(copy.getExternalControlTimeoutMs(),1250u);

    InspectableSolverInput assigned(QStringLiteral("other.rbm"));
    assigned = copy;
    QCOMPARE(assigned.getExternalControlProgram(),copy.getExternalControlProgram());
    QCOMPARE(assigned.getExternalControlArguments(),copy.getExternalControlArguments());
    QCOMPARE(assigned.getExternalControlTimeoutMs(),1250u);
    assigned = InspectableSolverInput(QStringLiteral("default.rbm"));
    QVERIFY(assigned.getExternalControlProgram().isEmpty());
    QVERIFY(assigned.getExternalControlArguments().isEmpty());
    QCOMPARE(assigned.getExternalControlTimeoutMs(),5000u);
}

QTEST_APPLESS_MAIN(TestSolverInput)
#include "tst_solver_input.moc"
