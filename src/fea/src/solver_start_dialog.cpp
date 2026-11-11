#include <QCheckBox>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "application.h"
#include "model_io.h"
#include "session.h"
#include "solver_manager.h"
#include "solver_start_dialog.h"
#include "solver_task.h"

SolverStartDialog::SolverStartDialog(uint modelID, QWidget *parent)
    : QDialog(parent)
    , validation(SolverManager::getInstance().validateRun(modelID))
{
    const bool canStart = this->validation.value(QStringLiteral("valid")).toBool();
    this->setWindowTitle(canStart ? tr("Start solver") : tr("Solver setup needs attention"));

    auto *mainLayout = new QVBoxLayout(this);
    const QString modelName = this->validation.value(QStringLiteral("modelName")).toString();
    auto *messageLabel = new QLabel(modelName.isEmpty()
        ? tr("Check the solver setup.")
        : (canStart ? tr("Start solver for %1?") : tr("Check the solver setup for %1.")).arg(modelName));
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setWordWrap(true);
    mainLayout->addWidget(messageLabel);

    QString diagnostics;
    const auto appendDiagnostics = [&diagnostics](const QString &heading, const QJsonArray &messages)
    {
        if (messages.isEmpty()) return;
        diagnostics += "<h3>" + heading.toHtmlEscaped() + "</h3><ul>";
        for (const QJsonValue &message : messages)
            diagnostics += "<li>" + message.toString().toHtmlEscaped() + "</li>";
        diagnostics += "</ul>";
    };
    appendDiagnostics(tr("Resolve before starting"), this->validation.value(QStringLiteral("errors")).toArray());
    appendDiagnostics(tr("Warnings"), this->validation.value(QStringLiteral("warnings")).toArray());
    if (!diagnostics.isEmpty())
    {
        auto *messagesBrowser = new QTextBrowser;
        messagesBrowser->setHtml(diagnostics);
        mainLayout->addWidget(messagesBrowser);
    }

    auto *buttonBox = new QDialogButtonBox;
    if (canStart)
    {
        const Model &model = Application::instance()->getSession()->getModel(modelID);
        if (model.getNVariables() > 0)
        {
            this->restartSolverCheck = new QCheckBox(tr("Continue from existing results"));
            this->restartSolverCheck->setChecked(model.getProblemSetup().getRestart());
            mainLayout->addWidget(this->restartSolverCheck);
        }
        const bool needsPath = model.getFileName().isEmpty();
        if (needsPath)
        {
            auto *pathHint = new QLabel(tr("Choose a model location for the solver files."));
            pathHint->setWordWrap(true);
            mainLayout->addWidget(pathHint);
        }
        buttonBox->setStandardButtons(QDialogButtonBox::Cancel);
        auto *startButton = buttonBox->addButton(needsPath ? tr("Choose location…") : tr("Start solver"),
                                                 QDialogButtonBox::AcceptRole);
        // Require an explicit start when the user has diagnostics to review.
        if (!diagnostics.isEmpty())
        {
            startButton->setAutoDefault(false);
            auto *cancelButton = buttonBox->button(QDialogButtonBox::Cancel);
            cancelButton->setDefault(true);
            cancelButton->setFocus();
        }
        else
            startButton->setDefault(true);
    }
    else
    {
        buttonBox->setStandardButtons(QDialogButtonBox::Close);
    }
    mainLayout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    this->resize(440, diagnostics.isEmpty() ? this->sizeHint().height() : 270);
}

int SolverStartDialog::exec()
{
    if (QDialog::exec() != QDialog::Accepted || !this->validation.value(QStringLiteral("valid")).toBool())
        return QDialog::Rejected;

    Session *session = Application::instance()->getSession();
    const auto *settings = Application::instance()->getApplicationSettings();
    auto &manager = SolverManager::getInstance();
    const QJsonObject stamp = this->validation.value(QStringLiteral("sourceStamp")).toObject();
    const QString identity = stamp.value(QStringLiteral("documentId")).toString();
    const QString revision = stamp.value(QStringLiteral("revision")).toString();
    const auto resolveModel = [session, &identity, &revision]()
    {
        const uint id = session->findModelByCommandIdentity(identity);
        return !identity.isEmpty() && id != RConstants::eod &&
               QString::number(session->modelCommandStamp(id).second) == revision ? id : RConstants::eod;
    };
    const auto rejectStart = [this](const QString &message)
    {
        QMessageBox notice(QMessageBox::Warning, tr("Solver not started"), message,
                           QMessageBox::Ok, this->parentWidget());
        notice.setTextFormat(Qt::PlainText);
        notice.exec();
        return QDialog::Rejected;
    };
    const QString changed = tr("The model changed or closed. Review the setup before starting again.");
    uint modelID = resolveModel();
    if (modelID == RConstants::eod)
        return rejectStart(changed);

    QString chosenPath;
    if (session->getModel(modelID).getFileName().isEmpty())
    {
        chosenPath = ModelIO::getModelSaveName(settings, this->parentWidget(), session->getModel(modelID));
        if (chosenPath.isEmpty()) return QDialog::Rejected;
        modelID = resolveModel();
        if (modelID == RConstants::eod || !session->getModel(modelID).getFileName().isEmpty())
            return rejectStart(changed);
    }

    const QJsonObject current = manager.validateRun(modelID);
    if (!current.value(QStringLiteral("valid")).toBool())
    {
        const QJsonArray errors = current.value(QStringLiteral("errors")).toArray();
        return rejectStart(errors.isEmpty() ? tr("Check the solver setup again.") : errors.first().toString());
    }
    if (current.value(QStringLiteral("sourceStamp")).toObject() != stamp)
        return rejectStart(changed);
    if (current.value(QStringLiteral("warnings")) != this->validation.value(QStringLiteral("warnings")))
        return rejectStart(tr("Solver warnings changed. Review them before starting again."));

    session->storeCurentModelVersion(modelID, tr("Configure solver run"));
    Model &model = session->getModel(modelID);
    if (!chosenPath.isEmpty())
        model.setFileName(chosenPath);
    const bool restart = this->restartSolverCheck && this->restartSolverCheck->isChecked();
    model.getProblemSetup().setRestart(restart);
    model.getTimeSolver().harmonizeTimesWithInput(restart);
    auto *task = new SolverTask(settings, modelID);
    session->setProblemChanged(modelID);

    QString error;
    if (manager.submitValidated(task, QJsonObject(), true, &error).isEmpty())
    {
        const uint currentID = session->findModelByCommandIdentity(identity);
        if (!chosenPath.isEmpty() && currentID != RConstants::eod &&
            session->getModel(currentID).getFileName() == chosenPath)
            session->getModel(currentID).setFileName(QString());
        return rejectStart(error.isEmpty() ? tr("The solver job could not be submitted.") : error);
    }
    return QDialog::Accepted;
}
