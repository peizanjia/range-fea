#include <QtTest>
#include <QComboBox>
#include <QUuid>
#include <rmodel_command_session.h>
#include "modal_setup_widget.h"
#include "value_line_edit.h"

class TestModalCommandWidget : public QObject
{
    Q_OBJECT
private slots:
    void widgetChangesMatchServiceContract()
    {
        RModel model;
        model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        RModelCommandSession editing(model);
        ModalSetupWidget widget(model.getProblemSetup().getModalSetup());
        int commands = 0;
        QJsonObject response;
        connect(&widget, &ModalSetupWidget::changed, &widget, [&](const RModalSetup &setup) {
            const auto current = editing.query();
            response = editing.execute({{"apiVersion", "range.application/v1"},
                {"commandId", "analysis.modal.configure"}, {"commandVersion", 1},
                {"modelId", current["modelId"]}, {"expectedRevision", current["revision"]},
                {"requestId", QUuid::createUuid().toString()}, {"idempotencyKey", QUuid::createUuid().toString()},
                {"parameters", RModelCommandSession::modalParameters(setup)}, {"dryRun", false}});
            ++commands;
        });
        auto *method = widget.findChild<QComboBox*>("modalMethod");
        auto *iterations = widget.findChild<QSpinBox*>("modalIterations");
        auto *modes = widget.findChild<QSpinBox*>("modalModes");
        auto *tolerance = widget.findChild<ValueLineEdit*>("modalTolerance");
        QVERIFY(method && iterations && modes && tolerance);
        method->setCurrentIndex(R_MODAL_MULTIPLE_MODES);
        iterations->setValue(25);
        modes->setValue(6);
        QVERIFY(commands >= 2);
        QCOMPARE(response.value("status").toString(), QString("accepted"));
        QCOMPARE(editing.model().getProblemSetup().getModalSetup().getNIterations(), 25u);
        QCOMPARE(editing.model().getProblemSetup().getModalSetup().getNModesToExtract(), 6u);
        QCOMPARE(tolerance->getMaximum(), 1.0);
        tolerance->setText("2");
        QVERIFY(tolerance->getValidatorState() != QValidator::Acceptable);
    }
};

QTEST_MAIN(TestModalCommandWidget)
#include "tst_modal_command_widget.moc"
