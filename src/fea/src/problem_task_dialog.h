#ifndef PROBLEM_TASK_DIALOG_H
#define PROBLEM_TASK_DIALOG_H

#include <QDialog>

#include "problem_task_tree.h"

class ProblemTaskDialog : public QDialog
{

    Q_OBJECT

    protected:

        //! Identity and revision of the model shown in this editor.
        QPair<QString, quint64> modelStamp;
        //! Problem task tree widget.
        ProblemTaskTree *taskTree;
        //! OK button.
        QPushButton *okButton;

    public:

        //! Constructor.
        explicit ProblemTaskDialog(uint modelID, QWidget *parent = nullptr);

        //! Execute dialog.
        int exec();

    protected slots:

        //! handle ProblemtaskTree::changed() signal.
        void onProblemTaskTreeChanged();

};

#endif // PROBLEM_TASK_DIALOG_H
