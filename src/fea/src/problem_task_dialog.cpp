#include <QVBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QScopeGuard>
#include <QPushButton>
#include <QDialogButtonBox>

#include "application.h"
#include "model_command_host.h"
#include "problem_task_dialog.h"

ProblemTaskDialog::ProblemTaskDialog(uint modelID, QWidget *parent)
    : QDialog(parent)
{
    const auto *session = Application::instance()->getSession();
    modelStamp = session->modelCommandStamp(modelID);
    const Model &rModel = session->getModel(modelID);
    const bool unconfigured = rModel.getProblemTaskTree().getProblemTypeMask() == R_PROBLEM_NONE;

    this->setWindowTitle(tr("Problem task flow"));
    this->resize(600,300);

    QIcon cancelIcon(":/icons/action/pixmaps/range-cancel.svg");
    QIcon okIcon(":/icons/action/pixmaps/range-ok.svg");

    QVBoxLayout *mainLayout = new QVBoxLayout;
    this->setLayout (mainLayout);

    auto *messageLabel = new QLabel(tr("Analysis tasks for %1").arg(rModel.getName()), this);
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setWordWrap(true);
    mainLayout->addWidget(messageLabel);
    if (unconfigured)
    {
        auto *hint = new QLabel(tr("You can build geometry first and use Set up analysis in the Problem panel when ready."), this);
        hint->setWordWrap(true);
        mainLayout->addWidget(hint);
    }

    this->taskTree = new ProblemTaskTree(rModel.getProblemTaskTree(),this);
    mainLayout->addWidget(this->taskTree);

    QDialogButtonBox *buttonBox = new QDialogButtonBox;
    mainLayout->addWidget(buttonBox);

    auto *cancelButton = new QPushButton(cancelIcon, unconfigured ? tr("Set up later") : tr("Cancel"));
    buttonBox->addButton(cancelButton,QDialogButtonBox::RejectRole);

    this->okButton = new QPushButton(okIcon, tr("Apply tasks"));
    this->okButton->setDisabled(true);
    buttonBox->addButton(this->okButton,QDialogButtonBox::AcceptRole);

    QObject::connect(buttonBox,&QDialogButtonBox::rejected,this,&QDialog::reject);
    QObject::connect(buttonBox,&QDialogButtonBox::accepted,this,&QDialog::accept);

    QObject::connect(this->taskTree,&ProblemTaskTree::changed,this,&ProblemTaskDialog::onProblemTaskTreeChanged);
}

int ProblemTaskDialog::exec()
{
    int retVal = QDialog::exec();

    if (retVal == QDialog::Accepted)
    {
        auto *session = Application::instance()->getSession();
        const auto rejectApply = [this](const QString &title, const QString &message) {
            setResult(QDialog::Rejected);
            QMessageBox::information(parentWidget(), title, message);
            return QDialog::Rejected;
        };
        if (ModelCommandHost::isMutationBlocked() || !session->trylock())
            return rejectApply(tr("Model busy"), tr("Wait for the model operation to finish before applying analysis tasks."));

        uint modelID;
        {
            const auto unlock = qScopeGuard([session] { session->unlock(); });
            modelID = session->findModelByCommandIdentity(modelStamp.first);
            if (modelID != RConstants::eod && session->modelCommandStamp(modelID) == modelStamp)
                session->getModel(modelID).setProblemTaskTree(taskTree->getProblemTaskTree());
            else
                modelID = RConstants::eod;
        }
        if (modelID == RConstants::eod)
        {
            return rejectApply(tr("Model changed"),
                tr("The model changed while this editor was open. Select it again to configure its analysis tasks."));
        }
        RLogger::info("Problem task flow changed for model '%s'.\n", qPrintable(session->getModel(modelID).getName()));
        session->setProblemSelectionChanged(modelID);
    }
    return retVal;
}

void ProblemTaskDialog::onProblemTaskTreeChanged()
{
    RProblemTaskItem problemTaskTree = this->taskTree->getProblemTaskTree();
    this->okButton->setEnabled(problemTaskTree.getNChildren() > 0);
}
