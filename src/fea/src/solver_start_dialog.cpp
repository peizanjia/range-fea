#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QTextBrowser>
#include <QScrollBar>
#include <QScrollArea>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QJsonArray>

#include "application.h"
#include "session.h"
#include "solver_manager.h"
#include "solver_start_dialog.h"
#include "solver_task.h"

SolverStartDialog::SolverStartDialog(uint modelID, QWidget *parent)
    : QDialog(parent)
    , modelID(modelID)
    , pSolverTask(nullptr)
{
    const QJsonObject validation = SolverManager::getInstance().validateRun(this->modelID);
    for (const QJsonValue &value : validation.value(QStringLiteral("warnings")).toArray())
        this->warnings.append(value.toString());
    for (const QJsonValue &value : validation.value(QStringLiteral("errors")).toArray())
        this->errors.append(value.toString());

    const QString modelName = validation.value(QStringLiteral("modelName")).toString();
    const bool modelReadable = validation.value(QStringLiteral("modelReadable")).toBool();
    if (validation.value(QStringLiteral("valid")).toBool())
    {
        this->pSolverTask = new SolverTask(Application::instance()->getApplicationSettings(), this->modelID);
        this->pSolverTask->setBlocking(false);
    }

    this->setWindowTitle(tr("Start solver"));

    QIcon cancelIcon(":/icons/action/pixmaps/range-cancel.svg");
    QIcon okIcon(":/icons/action/pixmaps/range-ok.svg");

    QVBoxLayout *mainLayout = new QVBoxLayout;
    this->setLayout (mainLayout);

    QLabel *messageLabel = new QLabel;
    messageLabel->setText(tr("Start solver for model") + ": <b>" +
                          (modelName.isEmpty() ? tr("Model %1").arg(this->modelID) : modelName) + "</b>");
    mainLayout->addWidget(messageLabel);

    if (this->warnings.size() || this->errors.size())
    {
        QTextBrowser *messagesBrowser = new QTextBrowser;
        mainLayout->addWidget(messagesBrowser);

        QString htmlMessage;

        if (this->errors.size() > 0)
        {
            htmlMessage += "<h3>" + tr("Errors") + "</h3>";
            htmlMessage += "<ul>";
            for (int i=0;i<this->errors.size();i++)
            {
                htmlMessage += "<li>";
                htmlMessage += this->errors.at(i);
                htmlMessage += "</li>";
            }
            htmlMessage += "</ul>";
        }

        if (this->warnings.size() > 0)
        {
            htmlMessage += "<h3>" + tr("Warnings") + "</h3>";
            htmlMessage += "<ul>";
            for (int i=0;i<this->warnings.size();i++)
            {
                htmlMessage += "<li>";
                htmlMessage += this->warnings.at(i);
                htmlMessage += "</li>";
            }
            htmlMessage += "</ul>";
        }

        messagesBrowser->insertHtml(htmlMessage);

        QTextCursor textCursor = messagesBrowser->textCursor();
        textCursor.movePosition(QTextCursor::Start);
        messagesBrowser->setTextCursor(textCursor);

        this->resize(600,400);
    }

    QLabel *commandTitleLabel = new QLabel(tr("Command line"));
    mainLayout->addWidget(commandTitleLabel);

    QScrollArea *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);
    mainLayout->addWidget(scrollArea);

    QLabel *commandLineLabel = new QLabel(this->pSolverTask
                                          ? this->pSolverTask->getCommandLine()
                                          : tr("Solver command is unavailable until validation succeeds."));
    commandLineLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    commandLineLabel->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);
    scrollArea->setWidget(commandLineLabel);

    this->restartSolverCheck = new QCheckBox(tr("Restart solver / continue"));
    if (!modelReadable || !this->pSolverTask)
    {
        this->restartSolverCheck->setDisabled(true);
        this->restartSolverCheck->setChecked(false);
    }
    else if (Application::instance()->getSession()->getModel(this->modelID).getNVariables() == 0)
    {
        this->restartSolverCheck->setDisabled(true);
        this->restartSolverCheck->setChecked(false);
    }
    else
    {
        this->restartSolverCheck->setEnabled(true);
        this->restartSolverCheck->setChecked(Application::instance()->getSession()
                                             ->getModel(this->modelID).getProblemSetup().getRestart());
    }
    mainLayout->addWidget(this->restartSolverCheck);

    QDialogButtonBox *buttonBox = new QDialogButtonBox;
    mainLayout->addWidget(buttonBox);

    QPushButton *cancelButton = new QPushButton(cancelIcon, tr("Cancel"));
    buttonBox->addButton(cancelButton,QDialogButtonBox::RejectRole);

    QPushButton *okButton = new QPushButton(okIcon, tr("OK"));
    okButton->setDisabled(this->errors.size() > 0);
    buttonBox->addButton(okButton,QDialogButtonBox::AcceptRole);

    QObject::connect(buttonBox,&QDialogButtonBox::rejected,this,&QDialog::reject);
    QObject::connect(buttonBox,&QDialogButtonBox::accepted,this,&QDialog::accept);
}

int SolverStartDialog::exec()
{
    int retVal = QDialog::exec();

    if (retVal == QDialog::Accepted)
    {
        const QJsonObject validation = SolverManager::getInstance().validateRun(this->modelID);
        if (!this->pSolverTask || !validation.value(QStringLiteral("valid")).toBool())
        {
            const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
            QMessageBox::warning(this, tr("Solver not started"),
                                 errors.isEmpty() ? tr("The solver validation is no longer valid.") : errors.first().toString());
            if (this->pSolverTask) this->pSolverTask->deleteLater();
            this->pSolverTask = nullptr;
            return QDialog::Rejected;
        }

        Session *session = Application::instance()->getSession();
        session->storeCurentModelVersion(this->modelID, tr("Configure solver run"));
        session->getModel(this->modelID).getProblemSetup().setRestart(this->restartSolverCheck->isChecked());
        session->getModel(this->modelID).getTimeSolver().harmonizeTimesWithInput(this->restartSolverCheck->isChecked());
        session->setProblemChanged(this->modelID);

        // Start solver task
        QString error;
        const QString jobId = SolverManager::getInstance().submitValidated(
                    this->pSolverTask, QJsonObject(), true, &error);
        if (jobId.isEmpty())
        {
            QMessageBox::warning(this, tr("Solver not started"),
                                 error.isEmpty() ? tr("The solver job could not be submitted.") : error);
            retVal = QDialog::Rejected;
        }
    }
    else
    {
        if (this->pSolverTask) this->pSolverTask->deleteLater();
        this->pSolverTask = nullptr;
    }

    return retVal;
}
