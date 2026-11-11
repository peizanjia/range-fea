#ifndef SOLVER_START_DIALOG_H
#define SOLVER_START_DIALOG_H

#include <QDialog>
#include <QJsonObject>

class QCheckBox;

class SolverStartDialog : public QDialog
{

    Q_OBJECT

    private:

        QJsonObject validation;
        QCheckBox *restartSolverCheck = nullptr;

    public:

        //! Constructor.
        explicit SolverStartDialog(uint modelID, QWidget *parent = nullptr);

        //! Execute dialog.
        int exec() override;

};

#endif // SOLVER_START_DIALOG_H
