#ifndef SOLVER_START_DIALOG_H
#define SOLVER_START_DIALOG_H

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QDoubleSpinBox;

class SolverStartDialog : public QDialog
{

    Q_OBJECT

    private:

        QJsonObject validation;
        QCheckBox *restartSolverCheck = nullptr;
        QCheckBox *adaptiveCheck = nullptr;
        QDoubleSpinBox *adaptiveTolerance = nullptr;

    public:

        //! Constructor.
        explicit SolverStartDialog(uint modelID, QWidget *parent = nullptr);

        //! Execute dialog.
        int exec() override;

};

#endif // SOLVER_START_DIALOG_H
