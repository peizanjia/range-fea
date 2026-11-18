#include <algorithm>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QScopeGuard>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "application.h"
#include "main_window.h"
#include "model_command_host.h"
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
        auto *session = Application::instance()->getSession();
        bool adaptiveSupported = false;
        if (!ModelCommandHost::isMutationBlocked() && session->trylock())
        {
            const auto unlock = qScopeGuard([session] { session->unlock(); });
            if (modelID < session->getNModels())
            {
                const auto &selected = session->getModel(modelID);
                // Offer the option from cheap topology checks. Detailed
                // physical validation runs only on explicit opt-in.
                adaptiveSupported = selected.getNSurfaces() > 0 &&
                    selected.getProblemTaskTree().getProblemTypeMask() == R_PROBLEM_STRESS &&
                    !selected.getTimeSolver().getEnabled() &&
                    std::all_of(selected.getElements().begin(), selected.getElements().end(), [](const RElement &element) {
                        const auto type = element.getType();
                        return type == R_ELEMENT_TRI1 || type == R_ELEMENT_TRUSS1 || type == R_ELEMENT_POINT;
                    });
            }
        }
        if (adaptiveSupported)
        {
            auto *row = new QHBoxLayout;
            adaptiveCheck = new QCheckBox(tr("Adaptive local refinement"), this);
            adaptiveCheck->setObjectName(QStringLiteral("solverAdaptiveCheck"));
            adaptiveCheck->setToolTip(tr("Evaluate a verified TRI3 baseline before launching any additional solve."));
            auto *target = new QLabel(tr("Energy error target"), this);
            adaptiveTolerance = new QDoubleSpinBox(this);
            adaptiveTolerance->setObjectName(QStringLiteral("solverAdaptiveTolerancePercent"));
            adaptiveTolerance->setRange(0.01, 100.0);
            adaptiveTolerance->setValue(5.0);
            adaptiveTolerance->setSuffix(tr(" %"));
            target->setBuddy(adaptiveTolerance);
            row->addWidget(adaptiveCheck);
            row->addStretch();
            row->addWidget(target);
            row->addWidget(adaptiveTolerance);
            mainLayout->addLayout(row);
            target->hide();
            adaptiveTolerance->hide();
            connect(adaptiveCheck, &QCheckBox::toggled, this, [this, target, startButton, needsPath](bool checked) {
                target->setVisible(checked);
                adaptiveTolerance->setVisible(checked);
                if (restartSolverCheck) restartSolverCheck->setEnabled(!checked);
                startButton->setText(checked ? tr("Start adaptive review") :
                                     needsPath ? tr("Choose location…") : tr("Start solver"));
            });
        }
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

    const bool adaptive = adaptiveCheck && adaptiveCheck->isChecked();
    QString chosenPath;
    if (!adaptive && session->getModel(modelID).getFileName().isEmpty())
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

    if (adaptive)
    {
        bool validRevision = false;
        const quint64 sourceRevision = revision.toULongLong(&validRevision);
        if (!validRevision || !Application::instance()->getMainWindow()->startAdaptiveMeshStudy(
                {identity, sourceRevision}, adaptiveTolerance->value()))
            return rejectStart(tr("The adaptive study could not be started for this model revision."));
        return QDialog::Accepted;
    }

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
