#include "workflow_widget.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

WorkflowWidget::WorkflowWidget(QWidget *parent) : QWidget(parent), service(this)
{
    setObjectName("workflowWidget");
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Rod / beam control experiments. These reference workflows have their own workspace and do not replace the model you are editing."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *workspaceRow = new QHBoxLayout;
    workspaceEdit = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/workflows/default");
    workspaceEdit->setObjectName("workflowWorkspace");
    workspaceEdit->setAccessibleName(tr("Workflow workspace"));
    workspaceRow->addWidget(workspaceEdit);
    auto *browse = new QPushButton(tr("Browse"));
    workspaceRow->addWidget(browse);
    connectButton = new QPushButton(tr("Open workspace"));
    connectButton->setObjectName("workflowOpen");
    workspaceRow->addWidget(connectButton);
    layout->addLayout(workspaceRow);
    trustedExtensions = new QCheckBox(tr("Trusted developer extensions"));
    trustedExtensions->setObjectName("workflowTrustedExtensions");
    trustedExtensions->setToolTip(tr("Allow reviewed Python extensions to run with this application's file and network permissions. Leave disabled for untrusted generated code."));
    layout->addWidget(trustedExtensions);
    auto *trustDescription = new QLabel(tr("Reviewed Python code runs with this application's file and network permissions. This mode is not a security sandbox."));
    trustDescription->setWordWrap(true); trustDescription->hide(); layout->addWidget(trustDescription);
    connect(trustedExtensions, &QCheckBox::toggled, trustDescription, &QWidget::setVisible);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("Workflow workspace"), workspaceEdit->text());
        if (!path.isEmpty()) workspaceEdit->setText(path);
    });
    connect(&service, &WorkflowProcess::processStateChanged, this, &WorkflowWidget::setActionAvailability);
    connect(connectButton, &QPushButton::clicked, this, [this] {
        if (service.isReady()) service.stop();
        else
        {
            auto config = WorkflowProcess::defaultConfiguration(workspaceEdit->text());
            config.trustedExtensions = trustedExtensions->isChecked();
            openWorkspace(config);
        }
    });
    auto *form = new QFormLayout;
    modelChoice = new QComboBox;
    modelChoice->addItem(tr("Four-rod chain"), "four-rod");
    modelChoice->addItem(tr("Cantilever beam"), "cantilever-beam");
    modelChoice->setObjectName("workflowModel");
    form->addRow(tr("Reference model"), modelChoice);
    dt = new QDoubleSpinBox;
    dt->setDecimals(6); dt->setRange(0.000001, 0.02); dt->setValue(0.005); dt->setSingleStep(0.001);
    dt->setObjectName("workflowTimeStep");
    form->addRow(tr("Time step (s)"), dt);
    steps = new QSpinBox;
    steps->setRange(1, 5000); steps->setValue(400); steps->setObjectName("workflowSteps");
    form->addRow(tr("Steps / additional steps"), steps);
    damping = new QDoubleSpinBox;
    damping->setDecimals(4); damping->setRange(0.0001, 0.5); damping->setValue(0.1);
    damping->setObjectName("workflowDamping");
    form->addRow(tr("Target damping ratio"), damping);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    configureButton = new QPushButton(tr("Preview configuration"));
    runButton = new QPushButton(tr("Preview active / passive run"));
    resumeButton = new QPushButton(tr("Preview continuation"));
    configureButton->setObjectName("workflowConfigure");
    runButton->setObjectName("workflowRun");
    resumeButton->setObjectName("workflowResume");
    buttons->addWidget(configureButton); buttons->addWidget(runButton); buttons->addWidget(resumeButton);
    layout->addLayout(buttons);
    connect(configureButton, &QPushButton::clicked, this, [this] {
        previewCommand("workflow.configure", {{"model", modelChoice->currentData().toString()},
            {"dt", dt->value()}, {"steps", steps->value()}, {"targetDamping", damping->value()}});
    });
    connect(runButton, &QPushButton::clicked, this, [this] {
        previewCommand("workflow.run", {{"mode", "both"}, {"independentReference", true}});
    });
    connect(resumeButton, &QPushButton::clicked, this, [this] {
        if (const auto *item = jobTree->currentItem())
            previewCommand("workflow.resume", {{"jobId", item->data(0, Qt::UserRole).toString()}, {"additionalSteps", steps->value()}});
    });
    previewText = new QTextBrowser;
    previewText->setObjectName("workflowPreview"); previewText->setMaximumHeight(145);
    previewText->setAccessibleName(tr("Changes awaiting approval"));
    layout->addWidget(previewText);
    applyButton = new QPushButton(tr("Apply this preview"));
    applyButton->setObjectName("workflowApply");
    layout->addWidget(applyButton);
    connect(applyButton, &QPushButton::clicked, this, &WorkflowWidget::applyPreview);
    status = new QLabel(tr("Open a workspace to begin."));
    status->setObjectName("workflowStatus"); status->setWordWrap(true);
    layout->addWidget(status);
    jobTree = new QTreeWidget;
    jobTree->setObjectName("workflowJobs");
    jobTree->setHeaderLabels({tr("Task"), tr("State"), tr("Created")});
    jobTree->setRootIsDecorated(false); jobTree->setAccessibleName(tr("Workflow tasks"));
    layout->addWidget(jobTree);
    cancelButton = new QPushButton(tr("Cancel selected task"));
    cancelButton->setObjectName("workflowCancel");
    layout->addWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (const auto *item = jobTree->currentItem())
            request("job.cancel", {{"jobId", item->data(0, Qt::UserRole).toString()}});
    });
    connect(jobTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        setActionAvailability();
        const auto *item = jobTree->currentItem();
        if (item && item->data(1, Qt::UserRole).toString() == "succeeded")
        {
            const auto id = item->data(0, Qt::UserRole).toString();
            if (displayedResult != id) request("results.query", {{"jobId", id}});
        }
    });
    results = new QTableWidget(0, 3);
    results->setObjectName("workflowResults");
    results->setHorizontalHeaderLabels({tr("Quantity"), tr("Active"), tr("Passive")});
    results->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results->setAccessibleName(tr("Verified calculation results"));
    layout->addWidget(results);
    audit = new QTextBrowser;
    audit->setObjectName("workflowAudit"); audit->setMaximumHeight(110);
    audit->setAccessibleName(tr("Workflow operation history"));
    layout->addWidget(audit);
    auto *documentGroup = new QGroupBox(tr("Open-document solver results"));
    auto *documentLayout = new QVBoxLayout(documentGroup);
    documentResults = new QTreeWidget;
    documentResults->setObjectName("documentSolverResults");
    documentResults->setAccessibleName(tr("Document solver result snapshots"));
    documentResults->setRootIsDecorated(false);
    documentResults->setHeaderLabels({tr("Model snapshot"), tr("Source revision"), tr("Result")});
    documentResults->setMaximumHeight(130);
    documentLayout->addWidget(documentResults);
    auto *openResult = new QPushButton(tr("Open selected result as a separate document"));
    openResult->setEnabled(false); documentLayout->addWidget(openResult);
    connect(documentResults, &QTreeWidget::itemSelectionChanged, this, [this, openResult] {
        openResult->setEnabled(documentResults->currentItem() != nullptr);
    });
    connect(openResult, &QPushButton::clicked, this, [this] {
        if (const auto *item = documentResults->currentItem())
            emit openResultRequested(item->data(0, Qt::UserRole).toString());
    });
    layout->addWidget(documentGroup);
    connect(&service, &WorkflowProcess::readyChanged, this, [this](bool ready) {
        connectButton->setText(ready ? tr("Close workspace") : tr("Open workspace"));
        workspaceEdit->setEnabled(!ready);
        trustedExtensions->setEnabled(!ready);
        status->setText(ready ? tr("Workspace ready.") : tr("Workspace is closed."));
        if (ready) { refreshTimer.start(); refreshJobs(); request("audit.query", {{"limit", 100}}); }
        else refreshTimer.stop();
        setActionAvailability();
    });
    connect(&service, &WorkflowProcess::replyReceived, this, &WorkflowWidget::onReply);
    connect(&service, &WorkflowProcess::requestFailed, this,
            [this](const QString &id, const QString &, const QString &, const QString &message) {
        if (!ownedRequests.remove(id)) return;
        continuations.remove(id);
        if (id == jobsRequest) jobsRequest.clear();
        commandInFlight = false;
        showFailure(message);
    });
    connect(&service, &WorkflowProcess::serviceError, this, &WorkflowWidget::showFailure);
    refreshTimer.setInterval(500);
    connect(&refreshTimer, &QTimer::timeout, this, &WorkflowWidget::refreshJobs);
    const auto invalidatePreview = [this] {
        if (preview.isEmpty()) return;
        preview = {};
        previewText->setPlainText(tr("Configuration changed. Create a new preview before applying."));
        setActionAvailability();
    };
    connect(modelChoice, &QComboBox::currentIndexChanged, this, invalidatePreview);
    connect(dt, &QDoubleSpinBox::valueChanged, this, invalidatePreview);
    connect(steps, &QSpinBox::valueChanged, this, invalidatePreview);
    connect(damping, &QDoubleSpinBox::valueChanged, this, invalidatePreview);
    setActionAvailability();
}

void WorkflowWidget::openWorkspace(const WorkflowProcess::Configuration &configuration)
{
    workspaceEdit->setText(configuration.workspace);
    trustedExtensions->setChecked(configuration.trustedExtensions);
    preview = {}; pendingCommand.clear(); pendingArguments = {}; continuations.clear();
    commandInFlight = false; displayedResult.clear(); jobsRequest.clear();
    ownedRequests.clear();
    jobRows.clear(); jobTree->clear(); results->setRowCount(0); previewText->clear(); audit->clear();
    service.start(configuration);
}

void WorkflowWidget::previewCommand(const QString &command, const QJsonObject &arguments)
{
    if (!service.isReady() || commandInFlight) return;
    preview = {}; pendingCommand = command; pendingArguments = arguments; commandInFlight = true;
    continuations.insert(request("project.query"), "preview");
    status->setText(tr("Checking the current configuration…"));
    setActionAvailability();
}

void WorkflowWidget::applyPreview()
{
    if (!service.isReady() || commandInFlight || preview.isEmpty()) return;
    commandInFlight = true;
    continuations.insert(request("host.approve", {{"previewId", preview.value("previewId")},
                                                          {"previewHash", preview.value("previewHash")}}), "execute");
    setActionAvailability();
}

void WorkflowWidget::refreshJobs()
{
    if (service.isReady() && jobsRequest.isEmpty()) jobsRequest = request("jobs.query");
}

void WorkflowWidget::onReply(const QString &id, const QString &operation, const QJsonObject &result)
{
    if (!ownedRequests.remove(id)) return;
    const auto continuation = continuations.take(id);
    if (operation == "project.query" && continuation == "preview")
    {
        request("command.preview", {{"command", pendingCommand}, {"parameters", pendingArguments},
                                            {"expectedRevision", result.value("revision")}});
    }
    else if (operation == "command.preview")
    {
        preview = result; commandInFlight = false;
        const auto configuration = result.value("effectiveConfiguration").toObject();
        const auto normalized = result.value("normalized").toObject();
        const QString command = normalized.value("command").toString();
        const QString title = command == "workflow.configure" ? tr("Save configuration")
                              : command == "workflow.resume" ? tr("Continue selected calculation") : tr("Run control comparison");
        previewText->setPlainText(tr("%1\nModel: %2\nTime step: %3 s\nConfigured steps: %4\nTarget damping ratio: %5\n%6")
            .arg(title, configuration.value("model").toString())
            .arg(configuration.value("dt").toDouble()).arg(configuration.value("steps").toInt())
            .arg(configuration.value("targetDamping").toDouble())
            .arg(command == "workflow.resume" ? tr("Additional steps: %1").arg(normalized.value("parameters").toObject().value("additionalSteps").toInt())
                                              : tr("FE results will be checked against an independent reference.")));
        status->setText(tr("Review the values, then apply this preview."));
        emit previewAvailable(preview);
    }
    else if (operation == "host.approve" && continuation == "execute")
        request("command.execute", {{"previewId", preview.value("previewId")}, {"approvalToken", result.value("approvalToken")}});
    else if (operation == "command.execute")
    {
        commandInFlight = false; preview = {};
        previewText->setPlainText(tr("The approved task has been submitted. Its progress appears below."));
        status->setText(tr("Task accepted."));
        foregroundJob = result.value("jobId").toString();
        emit jobAccepted(result.value("jobId").toString());
        refreshJobs(); request("audit.query", {{"limit", 100}});
    }
    else if (operation == "jobs.query")
    {
        jobsRequest.clear();
        for (const auto &value : result.value("jobs").toArray())
        {
            const auto job = value.toObject();
            const auto jobId = job.value("jobId").toString();
            auto *item = jobRows.value(jobId, nullptr);
            if (!item)
            {
                item = new QTreeWidgetItem(jobTree); jobRows.insert(jobId, item);
                item->setData(0, Qt::UserRole, jobId);
                if (jobId == foregroundJob || !jobTree->currentItem()) jobTree->setCurrentItem(item);
            }
            const auto oldState = item->data(1, Qt::UserRole).toString();
            const auto state = job.value("state").toString();
            item->setText(0, job.value("command").toString()); item->setText(1, state);
            item->setText(2, job.value("createdAt").toString()); item->setData(1, Qt::UserRole, state);
            if (oldState != state && state == "succeeded")
            {
                if (jobTree->currentItem() == item && displayedResult != jobId)
                    request("results.query", {{"jobId", jobId}});
                request("audit.query", {{"limit", 100}});
            }
            else if (oldState != state && (state == "failed" || state == "cancelled" || state == "interrupted"))
                status->setText(tr("Task %1. %2").arg(state, job.value("error").toObject().value("message").toString()));
        }
    }
    else if (operation == "results.query")
    {
        const auto value = result.value("result").toObject();
        const auto *selected = jobTree->currentItem();
        if (selected && selected->data(0, Qt::UserRole).toString() == value.value("jobId").toString())
            showResult(value);
    }
    else if (operation == "audit.query")
    {
        QStringList lines;
        for (const auto &value : result.value("records").toArray())
        {
            const auto record = value.toObject().value("record").toObject();
            lines.append(record.value("commandId").toString() + " — " + record.value("status").toString());
        }
        audit->setPlainText(lines.join('\n'));
    }
    else if (operation == "job.cancel") { status->setText(tr("Cancellation requested.")); refreshJobs(); }
    setActionAvailability();
}

void WorkflowWidget::showResult(const QJsonObject &result)
{
    displayedResult = result.value("jobId").toString();
    results->setRowCount(0);
    if (!result.contains("active") && !result.contains("passive"))
    {
        status->setText(tr("Configuration saved. You can now preview a calculation."));
        emit resultDisplayed(result);
        return;
    }
    const QList<QPair<QString, QString>> rows{{"energyJ", tr("Final energy (J)")},
        {"maxDisplacementErrorM", tr("Displacement reference error (m)")},
        {"maxVelocityErrorMPerSecond", tr("Velocity reference error (m/s)")},
        {"maxRotationErrorRad", tr("Rotation reference error (rad)")},
        {"maxAngularVelocityErrorRadPerSecond", tr("Angular velocity error (rad/s)")}};
    for (const auto &row : rows)
    {
        const int position = results->rowCount(); results->insertRow(position);
        results->setItem(position, 0, new QTableWidgetItem(row.second));
        int column = 1;
        for (const auto &variant : {QString("active"), QString("passive")})
        {
            const auto data = result.value(variant).toObject();
            results->setItem(position, column++, new QTableWidgetItem(data.contains(row.first)
                ? QString::number(data.value(row.first).toDouble(), 'g', 8) : tr("Not calculated")));
        }
    }
    status->setText(tr("Verified %1 steps at %2 s. Results remain tied to this task's input snapshot.")
        .arg(result.value("totalSteps").toInt()).arg(result.value("dtSeconds").toDouble()));
    emit resultDisplayed(result);
}

void WorkflowWidget::showFailure(const QString &message)
{
    status->setText(message); commandInFlight = false; preview = {};
    applyButton->setEnabled(false); setActionAvailability();
}

void WorkflowWidget::setActionAvailability()
{
    const bool available = service.isReady() && !commandInFlight;
    connectButton->setEnabled(service.isReady() || service.isStopped());
    workspaceEdit->setEnabled(service.isStopped());
    trustedExtensions->setEnabled(service.isStopped());
    configureButton->setEnabled(available); runButton->setEnabled(available);
    modelChoice->setEnabled(!commandInFlight); dt->setEnabled(!commandInFlight);
    steps->setEnabled(!commandInFlight); damping->setEnabled(!commandInFlight);
    applyButton->setEnabled(available && !preview.isEmpty());
    const auto *item = jobTree->currentItem();
    const auto state = item ? item->data(1, Qt::UserRole).toString() : QString();
    resumeButton->setEnabled(available && state == "succeeded" && item &&
        (item->text(0) == "workflow.run" || item->text(0) == "workflow.resume"));
    cancelButton->setEnabled(service.isReady() && (state == "queued" || state == "running"));
}

QString WorkflowWidget::request(const QString &operation, const QJsonObject &parameters)
{
    const auto id = service.request(operation, parameters);
    ownedRequests.insert(id);
    return id;
}

void WorkflowWidget::showDocumentResult(const QString &documentId, quint64 sourceRevision,
                                         const QString &artifactPath, bool applied)
{
    auto *item = new QTreeWidgetItem(documentResults);
    item->setText(0, QFileInfo(artifactPath).fileName());
    item->setToolTip(0, tr("Source document: %1\n%2").arg(documentId, artifactPath));
    item->setData(0, Qt::UserRole, artifactPath);
    item->setText(1, QString::number(sourceRevision));
    item->setText(2, applied ? tr("Attached to source") : tr("Retained separately; source was changed or unavailable"));
    if (!documentResults->currentItem()) documentResults->setCurrentItem(item);
}
