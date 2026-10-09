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
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include "control_monitor_widget.h"
#include "task_plan_widget.h"
#include "native_results_widget.h"
#include "solver_manager.h"
#include <QUuid>
#include "model_command_host.h"
#include "session.h"

WorkflowWidget::WorkflowWidget(QWidget *parent) : QWidget(parent), service(this)
{
    setObjectName("workflowWidget");
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 2, 4, 2);
    pages = new QTabWidget; pages->setObjectName("workflowPages"); rootLayout->addWidget(pages, 1);
    auto *monitorPage = new QWidget;
    auto *monitorLayout = new QVBoxLayout(monitorPage); monitorLayout->setContentsMargins(0, 0, 0, 0);
    pages->addTab(monitorPage, tr("Live monitor"));
    auto *experiment = new QWidget;
    auto *experimentScroll = new QScrollArea; experimentScroll->setWidgetResizable(true);
    experimentScroll->setFrameShape(QFrame::NoFrame); experimentScroll->setWidget(experiment);
    pages->addTab(experimentScroll, tr("Experiment"));
    auto *resultsPage = new QWidget;
    auto *resultsLayout = new QVBoxLayout(resultsPage);
    pages->addTab(resultsPage, tr("Results & history"));
    auto *nativePage = new QWidget;
    auto *nativeLayout = new QVBoxLayout(nativePage);
    nativeMode = new QComboBox;
    nativeMode->setObjectName("workflowNativeMode");
    nativeMode->addItem(tr("Actual active FE output"), "active");
    nativeMode->addItem(tr("Actual passive FE output"), "passive");
    nativeLayout->addWidget(nativeMode);
    workflowNativeResults = new NativeResultsWidget;
    workflowNativeResults->setObjectName("workflowNativeResults");
    workflowNativeResults->setWorkflow(&service);
    nativeLayout->addWidget(workflowNativeResults);
    pages->addTab(nativePage, tr("Workflow FE output"));
    auto *documentPage = new QWidget;
    auto *nativeDocumentLayout = new QVBoxLayout(documentPage);
    nativeJobChoice = new QComboBox;
    nativeJobChoice->setObjectName("nativeResultJobChoice");
    nativeDocumentLayout->addWidget(nativeJobChoice);
    nativeRecoveryButton = new QPushButton(tr("Check selected native recovery"));
    nativeRecoveryButton->setObjectName("nativeRecoveryCheck");
    nativeRecoveryApply = new QPushButton(tr("Approve this native recovery attempt"));
    nativeRecoveryApply->setObjectName("nativeRecoveryApply");
    nativeRecoveryApply->setEnabled(false);
    nativeRecoveryStatus = new QLabel(tr("Select an interrupted native job to inspect its accepted checkpoint."));
    nativeRecoveryStatus->setObjectName("nativeRecoveryStatus");
    nativeRecoveryStatus->setTextFormat(Qt::PlainText);
    nativeRecoveryStatus->setWordWrap(true);
    nativeDocumentLayout->addWidget(nativeRecoveryButton);
    nativeDocumentLayout->addWidget(nativeRecoveryApply);
    nativeDocumentLayout->addWidget(nativeRecoveryStatus);
    connect(nativeRecoveryButton, &QPushButton::clicked, this, &WorkflowWidget::checkNativeRecovery);
    connect(nativeRecoveryApply, &QPushButton::clicked, this, &WorkflowWidget::applyNativeRecovery);
    documentNativeResults = new NativeResultsWidget;
    documentNativeResults->setObjectName("documentNativeResults");
    nativeDocumentLayout->addWidget(documentNativeResults);
    pages->addTab(documentPage, tr("Document FE output"));
    connect(nativeMode, &QComboBox::currentIndexChanged, this, [this] {
        if (const auto *item = jobTree->currentItem())
            workflowNativeResults->setWorkflowJob(item->data(0, Qt::UserRole).toString(), nativeMode->currentData().toString());
    });
    connect(nativeJobChoice, &QComboBox::currentIndexChanged, this, [this] {
        nativeRecoveryPreviewId.clear(); nativeRecoveryJobId.clear();
        nativeRecoveryApply->setEnabled(false);
        documentNativeResults->setArtifact(nativeJobChoice->currentData().toString(), {}, nativeJobChoice->currentText());
    });
    nativeJobTimer.setInterval(1000);
    connect(&nativeJobTimer, &QTimer::timeout, this, &WorkflowWidget::refreshNativeJobs);
    nativeJobTimer.start();
    auto *layout = new QVBoxLayout(experiment);
    auto *intro = new QLabel(tr("Run bounded rod / beam control from a template or an accepted document snapshot. Review the source, units and controller mapping before approval."));
    intro->setWordWrap(true);
    intro->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
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
    form->addRow(tr("Model source"), modelChoice);
    sourceButton = new QPushButton(tr("Use current document snapshot"));
    sourceButton->setObjectName("workflowUseDocumentSnapshot");
    sourceButton->setToolTip(tr("Select exactly one supported document, freeze its accepted snapshot, and register it as a workflow source."));
    form->addRow(tr("Current document"), sourceButton);
    connect(sourceButton, &QPushButton::clicked, this, &WorkflowWidget::useCurrentDocumentSnapshot);
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
    recoverButton = new QPushButton(tr("Check interrupted recovery"));
    recoverButton->setObjectName("workflowRecover");
    configureButton->setObjectName("workflowConfigure");
    runButton->setObjectName("workflowRun");
    resumeButton->setObjectName("workflowResume");
    buttons->addWidget(configureButton); buttons->addWidget(runButton); buttons->addWidget(resumeButton);
    buttons->addWidget(recoverButton);
    layout->addLayout(buttons);
    connect(configureButton, &QPushButton::clicked, this, [this] {
        const QString model = modelChoice->currentData().toString();
        QJsonObject parameters{{"model", model}, {"dt", dt->value()},
            {"steps", steps->value()}, {"targetDamping", damping->value()}};
        const QString sourceId = selectedSourceId();
        if (!sourceId.isEmpty())
        {
            const auto source = documentSources.constFind(sourceId);
            QString reason;
            if (source == documentSources.cend() || !sourceRuntimeIsCurrent(source.value(), &reason))
            {
                status->setText(tr("The current document source is stale. Select it again after reopening or editing the document and capture a new snapshot. %1")
                                    .arg(reason));
                updateSourceChoiceLabel(sourceId);
                setActionAvailability();
                return;
            }
            parameters.insert("sourceId", sourceId);
        }
        // A template item always sends its explicit family and omits sourceId,
        // which intentionally resets a previously configured document source.
        previewCommand("workflow.configure", parameters);
    });
    connect(runButton, &QPushButton::clicked, this, [this] {
        previewCommand("workflow.run", {{"mode", "both"}, {"independentReference", true}});
    });
    connect(resumeButton, &QPushButton::clicked, this, [this] {
        if (const auto *item = jobTree->currentItem())
            previewCommand("workflow.resume", {{"jobId", item->data(0, Qt::UserRole).toString()}, {"additionalSteps", steps->value()}});
    });
    previewText = new QTextBrowser;
    connect(recoverButton, &QPushButton::clicked, this, [this] {
        if (const auto *item = jobTree->currentItem())
        {
            recoveryRequest = request("job.recovery.query", {{"jobId", item->data(0, Qt::UserRole).toString()}});
            status->setText(tr("Checking original checkpoints and completed variants…"));
            setActionAvailability();
        }
    });
    previewText->setObjectName("workflowPreview"); previewText->setMaximumHeight(145);
    previewText->setAccessibleName(tr("Changes awaiting approval"));
    layout->addWidget(previewText);
    applyButton = new QPushButton(tr("Apply this preview"));
    applyButton->setObjectName("workflowApply");
    layout->addWidget(applyButton);
    connect(applyButton, &QPushButton::clicked, this, &WorkflowWidget::applyPreview);
    status = new QLabel(tr("Open a workspace to begin."));
    status->setTextFormat(Qt::PlainText);
    status->setObjectName("workflowStatus"); status->setWordWrap(true);
    rootLayout->addWidget(status);
    auto *monitorSplit = new QSplitter(Qt::Horizontal);
    monitorLayout->addWidget(monitorSplit);
    auto *jobsPanel = new QWidget;
    layout = new QVBoxLayout(jobsPanel); layout->setContentsMargins(0, 0, 0, 0);
    monitorSplit->addWidget(jobsPanel);
    monitor = new ControlMonitorWidget(&service);
    monitorSplit->addWidget(monitor);
    monitorSplit->setStretchFactor(0, 0); monitorSplit->setStretchFactor(1, 1);
    monitorSplit->setSizes({210, 700});
    jobTree = new QTreeWidget;
    jobTree->setObjectName("workflowJobs");
    jobTree->setHeaderLabels({tr("Task"), tr("State"), tr("Created")});
    jobTree->setRootIsDecorated(false); jobTree->setAccessibleName(tr("Workflow tasks"));
    jobTree->setMinimumWidth(130); jobTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    jobTree->hideColumn(2);
    layout->addWidget(jobTree, 1);
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
        const QString command = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
        const bool calculation = command == "workflow.run" || command == "workflow.resume";
        monitor->setJob(calculation ? item->data(0, Qt::UserRole).toString() : QString(),
                        item ? item->data(1, Qt::UserRole).toString() : QString());
        if (item && item->data(1, Qt::UserRole).toString() == "succeeded")
        {
            const auto id = item->data(0, Qt::UserRole).toString();
            if (calculation) workflowNativeResults->setWorkflowJob(id, nativeMode->currentData().toString());
            if (displayedResult != id) request("results.query", {{"jobId", id}});
        }
    });
    layout = resultsLayout;
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
        if (ready) { refreshTimer.start(); refreshJobs(); request("audit.query", {{"limit", 100}});
                    request("model.sources.query", {{"offset", 0}, {"limit", 100}}); }
        else refreshTimer.stop();
        setActionAvailability();
    });
    connect(&service, &WorkflowProcess::replyReceived, this, &WorkflowWidget::onReply);
    connect(&service, &WorkflowProcess::requestFailed, this,
            [this](const QString &id, const QString &, const QString &code, const QString &message) {
        if (!ownedRequests.remove(id)) return;
        if (id == sourceRegisterRequest)
        {
            sourceRegisterRequest.clear();
            sourceRegistrationInFlight = false;
            pendingDocumentSource = {};
            status->setText(tr("Document source registration failed (%1). The captured snapshot remains in the project catalog.")
                                .arg(code));
            setActionAvailability();
            return;
        }
        continuations.remove(id);
        if (id == jobsRequest) jobsRequest.clear();
        if (id == recoveryRequest) recoveryRequest.clear();
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
    connect(modelChoice, &QComboBox::currentIndexChanged, this, [this, invalidatePreview] {
        invalidatePreview();
        updateSelectedSourceStatus();
    });
    connect(dt, &QDoubleSpinBox::valueChanged, this, invalidatePreview);
    connect(steps, &QSpinBox::valueChanged, this, invalidatePreview);
    connect(damping, &QDoubleSpinBox::valueChanged, this, invalidatePreview);
    pages->addTab(new TaskPlanWidget(&service), tr("Task plans"));
    setActionAvailability();
}

void WorkflowWidget::setSession(Session *value)
{
    if (session == value) return;
    if (session) disconnect(session, nullptr, this, nullptr);
    session = value;
    nativeRecoveryPreviewId.clear(); nativeRecoveryJobId.clear();
    nativeRecoveryApply->setEnabled(false);
    if (!session) return;
    documentNativeResults->setQueryHandler([this](const QString &operation, const QJsonObject &parameters) {
        if (!session) return QJsonObject{{"valid", false}, {"message", tr("Document session is unavailable.")}};
        const auto response = session->getModelCommandHost()->handle(QJsonObject{{"apiVersion", "range.host/v1"},
            {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"operation", operation}, {"parameters", parameters}});
        if (response.value("status") == "ok" || response.value("status") == "accepted")
        {
            auto result = response.value("result").toObject();
            if (!result.contains("valid")) result.insert("valid", true);
            return result;
        }
        return QJsonObject{{"valid", false}, {"code", response.value("code")},
            {"message", response.value("message")}};
    });

    connect(session, &Session::documentRevisionChanged, this,
            [this](const QString &documentId, quint64 revision) {
        for (auto it = documentSources.cbegin(); it != documentSources.cend(); ++it)
        {
            if (it.value().runtimeDocumentId == documentId && it.value().runtimeRevision != revision)
            {
                updateSourceChoiceLabel(it.key());
                if (selectedSourceId() == it.key())
                {
                    QString reason;
                    sourceRuntimeIsCurrent(it.value(), &reason);
                    status->setText(tr("The captured document source is stale: %1").arg(reason));
                }
            }
        }
        setActionAvailability();
    });
    connect(session, &Session::runtimeDocumentRemoved, this,
            [this](const QString &documentId) {
        for (auto it = documentSources.cbegin(); it != documentSources.cend(); ++it)
        {
            if (it.value().runtimeDocumentId == documentId)
            {
                updateSourceChoiceLabel(it.key());
                if (selectedSourceId() == it.key())
                    status->setText(tr("The captured document source is stale because its document was closed."));
            }
        }
        setActionAvailability();
    });
    setActionAvailability();
}

QString WorkflowWidget::selectedSourceId() const
{
    return modelChoice->currentData(Qt::UserRole + 1).toString();
}

bool WorkflowWidget::sourceRuntimeStampMatches(const DocumentSourceChoice &source, QString *reason) const
{
    const auto fail = [reason](const QString &message) {
        if (reason) *reason = message;
        return false;
    };
    if (reason) reason->clear();
    if (!session) return fail(tr("No document session is attached."));
    auto *host = session->getModelCommandHost();
    if (!host || host->sessionEpoch() != source.sessionEpoch)
        return fail(tr("The document session has changed."));
    const uint modelId = session->findModelByCommandIdentity(source.runtimeDocumentId);
    if (modelId >= session->getNModels())
        return fail(tr("The source document was closed."));
    const auto runtimeStamp = session->modelCommandStamp(modelId);
    if (runtimeStamp.first != source.runtimeDocumentId || runtimeStamp.second != source.runtimeRevision)
        return fail(tr("The source document was edited after its snapshot was captured."));
    if (session->hasDocumentDraft(modelId))
        return fail(tr("The source document has an unresolved editor draft."));
    if (session->projectWorkspace() != source.catalogRoot)
        return fail(tr("The active project workspace changed."));

    const auto &snapshotRef = source.snapshotRef;
    const auto instanceRef = snapshotRef.value(QStringLiteral("instanceRef")).toObject();
    if (instanceRef.value(QStringLiteral("documentId")).toString() != source.runtimeDocumentId ||
        instanceRef.value(QStringLiteral("sessionEpoch")).toString() != source.sessionEpoch ||
        instanceRef.value(QStringLiteral("documentRevision")).toDouble(-1.0) != double(source.runtimeRevision))
        return fail(tr("The captured runtime document stamp is invalid."));
    if (reason) *reason = tr("The runtime document stamp still matches the captured source.");
    return true;
}

bool WorkflowWidget::sourceRuntimeIsCurrent(const DocumentSourceChoice &source, QString *reason) const
{
    if (!sourceRuntimeStampMatches(source, reason)) return false;
    const auto fail = [reason](const QString &message) {
        if (reason) *reason = message;
        return false;
    };
    if (!session) return fail(tr("No document session is attached."));
    const uint modelId = session->findModelByCommandIdentity(source.runtimeDocumentId);
    if (modelId >= session->getNModels()) return fail(tr("The source document was closed."));
    if (session->hasDocumentDraft(modelId)) return fail(tr("The source document has an unresolved editor draft."));
    const auto &snapshotRef = source.snapshotRef;
    const auto context = session->documentProjectContext(modelId);
    if (context.value(QStringLiteral("availability")).toString() != QStringLiteral("available"))
        return fail(tr("The source document is no longer synchronized with its project catalog."));
    if (context.value("persistence").toObject().value("status") != QJsonValue("available"))
        return fail(tr("The source project persistence is not available."));
    if (context.value(QStringLiteral("projectId")) != snapshotRef.value(QStringLiteral("projectId")) ||
        context.value(QStringLiteral("persistentDocumentId")) != snapshotRef.value(QStringLiteral("persistentDocumentId")) ||
        context.value(QStringLiteral("sourceRevision")) != snapshotRef.value(QStringLiteral("sourceRevision")) ||
        context.value(QStringLiteral("modelSha256")) != snapshotRef.value(QStringLiteral("modelSha256")))
        return fail(tr("The project source version or model hash changed after capture."));
    const auto draft = context.value(QStringLiteral("draft")).toObject();
    if (draft.value(QStringLiteral("status")).toString() == QStringLiteral("pending"))
        return fail(tr("The source document has an unresolved editor draft."));
    if (reason) *reason = tr("The source document still matches the captured version.");
    return true;
}

QString WorkflowWidget::sourceDescription(const DocumentSourceChoice &source, bool includePorts) const
{
    const auto record = source.registration;
    const auto descriptor = record.value(QStringLiteral("descriptor")).toObject();
    const auto origin = record.value(QStringLiteral("originRef")).toObject();
    const QString modelKind = descriptor.value(QStringLiteral("modelKind")).toString();
    const QString modelName = modelKind == QStringLiteral("four-rod") ? tr("Four-rod chain")
                              : modelKind == QStringLiteral("cantilever-beam") ? tr("Cantilever beam")
                              : modelKind;
    QString result = tr("Model: %1\nSource version: %2\nModel SHA-256: %3\nSource manifest SHA-256: %4")
        .arg(modelName,
             origin.value(QStringLiteral("sourceRevision")).toString(),
             record.value(QStringLiteral("registeredModelSha256")).toString(),
             record.value(QStringLiteral("manifestSha256")).toString());
    if (!includePorts) return result;

    const auto ports = descriptor.value(QStringLiteral("ports")).toObject();
    const auto formatPort = [this](const QString &name, const QJsonValue &value) {
        const auto port = value.toObject();
        const auto direction = port.value(QStringLiteral("globalDirection")).toArray();
        const QString directionText = QString::fromUtf8(QJsonDocument(direction).toJson(QJsonDocument::Compact));
        return tr("%1: node %2, global direction %3, unit %4")
            .arg(name, QString::number(port.value(QStringLiteral("nodePhysicalId")).toInteger()),
                 directionText, port.value(QStringLiteral("unit")).toString());
    };
    result += tr("\nGlobal port map from source preflight:\n%1\n%2")
        .arg(formatPort(tr("Force"), ports.value(QStringLiteral("force"))),
             formatPort(tr("Velocity"), ports.value(QStringLiteral("velocity"))));
    return result;
}

QString WorkflowWidget::modelBindingSummary(const QJsonObject &binding) const
{
    if (binding.isEmpty()) return {};
    if (binding.value(QStringLiteral("kind")).toString() != QStringLiteral("native-snapshot"))
        return tr("Model binding: %1").arg(binding.value(QStringLiteral("kind")).toString());

    const QString sourceId = binding.value(QStringLiteral("sourceId")).toString();
    const auto origin = binding.value(QStringLiteral("originRef")).toObject();
    QString modelName = tr("Native document snapshot");
    QString currentStatus = tr("origin document status unavailable; result remains bound to its frozen snapshot");
    const auto source = documentSources.constFind(sourceId);
    if (source != documentSources.cend())
    {
        const QString kind = source->registration.value(QStringLiteral("descriptor")).toObject()
                                 .value(QStringLiteral("modelKind")).toString();
        if (kind == QStringLiteral("four-rod")) modelName = tr("Four-rod chain");
        else if (kind == QStringLiteral("cantilever-beam")) modelName = tr("Cantilever beam");
        QString reason;
        currentStatus = sourceRuntimeIsCurrent(source.value(), &reason)
            ? tr("origin document unchanged; result remains bound to its frozen snapshot")
            : tr("origin document stale (%1); result remains bound to its frozen snapshot").arg(reason);
    }
    return tr("Model/source: %1 · source version %2 · model SHA-256 %3 · %4")
        .arg(modelName,
             origin.value(QStringLiteral("sourceRevision")).toString(),
             binding.value(QStringLiteral("registeredModelSha256")).toString(),
             currentStatus);
}

void WorkflowWidget::updateSourceChoiceLabel(const QString &sourceId)
{
    const auto source = documentSources.constFind(sourceId);
    if (source == documentSources.cend()) return;
    const int index = modelChoice->findData(sourceId, Qt::UserRole + 1);
    if (index < 0) return;
    const auto record = source->registration;
    const auto descriptor = record.value(QStringLiteral("descriptor")).toObject();
    const auto origin = record.value(QStringLiteral("originRef")).toObject();
    const QString kind = descriptor.value(QStringLiteral("modelKind")).toString();
    const QString modelName = kind == QStringLiteral("four-rod") ? tr("Four-rod chain")
                              : kind == QStringLiteral("cantilever-beam") ? tr("Cantilever beam") : kind;
    QString text = tr("Current document · %1 · %2 · %3")
        .arg(modelName, origin.value(QStringLiteral("sourceRevision")).toString(),
             record.value(QStringLiteral("registeredModelSha256")).toString().left(12));
    QString reason;
    if (!sourceRuntimeStampMatches(source.value(), &reason)) text += tr(" · stale");
    modelChoice->setItemText(index, text);
    modelChoice->setItemData(index, kind, Qt::UserRole);
}

void WorkflowWidget::updateSelectedSourceStatus()
{
    const QString sourceId = selectedSourceId();
    if (sourceId.isEmpty())
    {
        status->setText(tr("An explicit reference template is selected. Applying its configuration clears the current document source."));
        return;
    }
    const auto source = documentSources.constFind(sourceId);
    if (source == documentSources.cend())
    {
        status->setText(tr("The selected document source is unavailable. Capture it again from the open document."));
        return;
    }
    QString reason;
    const bool current = sourceRuntimeIsCurrent(source.value(), &reason);
    status->setText(sourceDescription(source.value(), true) + QStringLiteral("\n") +
                    (current ? tr("Current document status: unchanged.")
                             : tr("Current document status: stale — %1").arg(reason)));
    updateSourceChoiceLabel(sourceId);
}

QString WorkflowWidget::previewSourceId() const
{
    const auto normalized = preview.value(QStringLiteral("normalized")).toObject();
    const QString command = normalized.value(QStringLiteral("command")).toString();
    if (command == QStringLiteral("workflow.configure"))
        return normalized.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("sourceId")).toString();
    if (command == QStringLiteral("workflow.run"))
        return preview.value(QStringLiteral("effectiveConfiguration")).toObject().value(QStringLiteral("sourceId")).toString();
    // Resume owns an already accepted immutable source; it does not require
    // the original GUI instance to remain open.
    return {};
}

bool WorkflowWidget::previewSourceIsCurrent() const
{
    const QString sourceId = previewSourceId();
    if (sourceId.isEmpty()) return true;
    const auto source = documentSources.constFind(sourceId);
    return source != documentSources.cend() && sourceRuntimeIsCurrent(source.value());
}

void WorkflowWidget::useCurrentDocumentSnapshot()
{
    if (!service.isReady())
    {
        status->setText(tr("Open the workflow workspace before registering a document source."));
        return;
    }
    if (!session)
    {
        status->setText(tr("The document session is unavailable."));
        return;
    }
    if (sourceRegistrationInFlight)
    {
        status->setText(tr("A document source registration is already in progress."));
        return;
    }
    const auto selected = session->getSelectedModelIDs();
    if (selected.size() != 1)
    {
        status->setText(tr("Select exactly one document before capturing a workflow source."));
        return;
    }
    const uint modelId = selected.first();
    if (modelId >= session->getNModels())
    {
        status->setText(tr("The selected document is no longer available."));
        return;
    }
    if (!session->trylock(0))
    {
        status->setText(tr("The selected document is busy. Try again when its current operation finishes."));
        return;
    }
    const Model &model = session->getModel(modelId);
    const bool rod = model.getNNodes() == 5 && model.getNElements() == 10 &&
                     model.getNElements(R_ELEMENT_TRUSS1) == 4 &&
                     model.getNElements(R_ELEMENT_POINT) == 6 &&
                     model.getNElements(R_ELEMENT_BEAM1) == 0;
    const bool beam = model.getNNodes() == 2 && model.getNElements() == 3 &&
                      model.getNElements(R_ELEMENT_BEAM1) == 1 &&
                      model.getNElements(R_ELEMENT_POINT) == 2 &&
                      model.getNElements(R_ELEMENT_TRUSS1) == 0;
    session->unlock();
    if (!rod && !beam)
    {
        status->setText(tr("The selected document does not match the bounded WF-03 topology (five nodes, four TRUSS1 plus six POINT; or two nodes, one BEAM1 plus two POINT). No snapshot was created."));
        return;
    }

    const auto runtimeStamp = session->modelCommandStamp(modelId);
    auto *host = session->getModelCommandHost();
    const QString catalogRoot = session->projectWorkspace();
    if (runtimeStamp.first.isEmpty() || !host || catalogRoot.isEmpty())
    {
        status->setText(tr("The selected document has no trusted project identity or workspace."));
        return;
    }
    const QString sessionEpoch = host->sessionEpoch();
    const auto contextBefore = session->documentProjectContext(modelId);
    const auto beforeDraft = contextBefore.value(QStringLiteral("draft")).toObject();
    if (contextBefore.value(QStringLiteral("availability")).toString() != QStringLiteral("available") ||
        session->hasDocumentDraft(modelId) ||
        beforeDraft.value(QStringLiteral("status")).toString() == QStringLiteral("pending"))
    {
        status->setText(tr("The selected document must have synchronized accepted state and no unresolved drafts before capture."));
        return;
    }
    if (session->modelCommandStamp(modelId) != runtimeStamp)
    {
        status->setText(tr("The selected document changed during the topology check. Retry the capture."));
        return;
    }

    QString error;
    const QJsonObject captured = session->createDocumentSnapshot(modelId, &error);
    if (captured.isEmpty())
    {
        status->setText(tr("Could not capture the accepted document snapshot: %1").arg(error));
        return;
    }
    const auto instanceRef = captured.value(QStringLiteral("instanceRef")).toObject();
    const auto contextAfter = session->documentProjectContext(modelId);
    if (session->modelCommandStamp(modelId) != runtimeStamp || host->sessionEpoch() != sessionEpoch ||
        session->projectWorkspace() != catalogRoot ||
        captured.value(QStringLiteral("projectId")) != contextAfter.value(QStringLiteral("projectId")) ||
        captured.value(QStringLiteral("persistentDocumentId")) != contextAfter.value(QStringLiteral("persistentDocumentId")) ||
        captured.value(QStringLiteral("sourceRevision")) != contextAfter.value(QStringLiteral("sourceRevision")) ||
        captured.value(QStringLiteral("modelSha256")) != contextAfter.value(QStringLiteral("modelSha256")) ||
        instanceRef.value(QStringLiteral("documentId")).toString() != runtimeStamp.first ||
        instanceRef.value(QStringLiteral("sessionEpoch")).toString() != sessionEpoch ||
        instanceRef.value(QStringLiteral("documentRevision")).toDouble(-1.0) != double(runtimeStamp.second))
    {
        status->setText(tr("The document or project changed during snapshot publication. The source was not registered."));
        return;
    }

    const QJsonObject snapshotRef{
        {QStringLiteral("projectId"), captured.value(QStringLiteral("projectId"))},
        {QStringLiteral("persistentDocumentId"), captured.value(QStringLiteral("persistentDocumentId"))},
        {QStringLiteral("sourceRevision"), captured.value(QStringLiteral("sourceRevision"))},
        {QStringLiteral("modelSha256"), captured.value(QStringLiteral("modelSha256"))},
        {QStringLiteral("instanceRef"), instanceRef}};
    pendingDocumentSource = {};
    pendingDocumentSource.snapshotRef = snapshotRef;
    pendingDocumentSource.runtimeDocumentId = runtimeStamp.first;
    pendingDocumentSource.runtimeRevision = runtimeStamp.second;
    pendingDocumentSource.sessionEpoch = sessionEpoch;
    pendingDocumentSource.catalogRoot = catalogRoot;
    sourceRegistrationInFlight = true;
    sourceRegisterRequest = request(QStringLiteral("model.source.register"),
                                   {{QStringLiteral("catalogRoot"), catalogRoot},
                                    {QStringLiteral("snapshotRef"), snapshotRef}});
    status->setText(tr("The accepted snapshot is frozen. Registering the source asynchronously…"));
    setActionAvailability();
}

void WorkflowWidget::openWorkspace(const WorkflowProcess::Configuration &configuration)
{
    workspaceEdit->setText(configuration.workspace);
    trustedExtensions->setChecked(configuration.trustedExtensions);
    {
        const QSignalBlocker blocker(modelChoice);
        while (modelChoice->count() > 2) modelChoice->removeItem(modelChoice->count() - 1);
        modelChoice->setCurrentIndex(0);
    }
    documentSources.clear();
    pendingDocumentSource = {};
    sourceRegisterRequest.clear();
    sourceRegistrationInFlight = false;
    preview = {}; pendingCommand.clear(); pendingArguments = {}; continuations.clear();
    commandInFlight = false; displayedResult.clear(); jobsRequest.clear();
    ownedRequests.clear();
    monitor->setJob({}, {});
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
    if (!previewSourceIsCurrent())
    {
        preview = {};
        previewText->setPlainText(tr("The captured document source is stale. Capture and select a fresh snapshot before approval."));
        status->setText(tr("The document source changed or closed before approval; no approval request was sent."));
        setActionAvailability();
        return;
    }
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
    if (operation == "job.recovery.query" && id == recoveryRequest)
    {
        recoveryRequest.clear();
        const auto *item = jobTree->currentItem();
        if (!item || item->data(0, Qt::UserRole).toString() != result.value("jobId").toString())
        { setActionAvailability(); return; }
        const QString eligibility = result.value("eligibility").toString();
        const QMap<QString, QString> labels{{"recoverable", tr("Recoverable")},
            {"recompute_required", tr("Recomputation required")},
            {"reconcile_required", tr("Reconciliation required")}, {"conflict", tr("Conflict")}};
        status->setText(labels.value(eligibility, tr("Recovery unavailable")) + " · " + result.value("diagnostic").toString());
        if (eligibility == "recoverable")
            previewCommand("workflow.recover", {{"jobId", result.value("jobId")},
                {"qualificationSha256", result.value("qualificationSha256")}});
        setActionAvailability();
        return;
    }
    if (operation == QStringLiteral("model.sources.query"))
    {
        const QString selected = selectedSourceId();
        const QSignalBlocker blocker(modelChoice);
        for (const auto &value : result.value(QStringLiteral("sources")).toArray())
        {
            const QJsonObject registration = value.toObject();
            const QString sourceId = registration.value(QStringLiteral("sourceId")).toString();
            if (sourceId.isEmpty() || documentSources.contains(sourceId)) continue;
            const auto origin = registration.value(QStringLiteral("originRef")).toObject();
            const auto instance = origin.value(QStringLiteral("instanceRef")).toObject();
            DocumentSourceChoice source;
            source.registration = registration;
            source.snapshotRef = origin;
            source.runtimeDocumentId = instance.value(QStringLiteral("documentId")).toString();
            source.sessionEpoch = instance.value(QStringLiteral("sessionEpoch")).toString();
            source.runtimeRevision = quint64(instance.value(QStringLiteral("documentRevision")).toDouble());
            source.catalogRoot = session ? session->projectWorkspace() : QString();
            documentSources.insert(sourceId, source);
            const QString kind = registration.value(QStringLiteral("descriptor")).toObject().value(QStringLiteral("modelKind")).toString();
            modelChoice->addItem(tr("Saved document snapshot · %1 · %2").arg(kind,
                registration.value(QStringLiteral("registeredModelSha256")).toString().left(12)), kind);
            modelChoice->setItemData(modelChoice->count() - 1, sourceId, Qt::UserRole + 1);
            updateSourceChoiceLabel(sourceId);
            if (selected == sourceId) modelChoice->setCurrentIndex(modelChoice->count() - 1);
        }
        const int offset = result.value(QStringLiteral("offset")).toInt();
        const int count = result.value(QStringLiteral("sources")).toArray().size();
        if (offset + count < result.value(QStringLiteral("total")).toInt() && count > 0)
            request("model.sources.query", {{"offset", offset + count}, {"limit", 100}});
        setActionAvailability();
        return;
    }
    if (operation == QStringLiteral("model.source.register") && id == sourceRegisterRequest)
    {
        sourceRegisterRequest.clear();
        sourceRegistrationInFlight = false;
        const QString sourceId = result.value(QStringLiteral("sourceId")).toString();
        const auto origin = result.value(QStringLiteral("originRef")).toObject();
        const auto descriptor = result.value(QStringLiteral("descriptor")).toObject();
        const QString modelKind = descriptor.value(QStringLiteral("modelKind")).toString();
        if (sourceId.isEmpty() ||
            (modelKind != QStringLiteral("four-rod") && modelKind != QStringLiteral("cantilever-beam")) ||
            result.value(QStringLiteral("registeredModelSha256")).toString().size() != 64 ||
            result.value(QStringLiteral("manifestSha256")).toString().size() != 64 || origin.isEmpty())
        {
            pendingDocumentSource = {};
            status->setText(tr("Source registration returned incomplete identity or hash metadata."));
            setActionAvailability();
            return;
        }
        pendingDocumentSource.registration = result;
        preview = {}; applyButton->setEnabled(false);
        pendingDocumentSource.registration.remove(QStringLiteral("path"));
        pendingDocumentSource.registration.remove(QStringLiteral("manifestPath"));
        documentSources.insert(sourceId, pendingDocumentSource);
        pendingDocumentSource = {};
        const QString modelName = modelKind == QStringLiteral("four-rod") ? tr("Four-rod chain") : tr("Cantilever beam");
        const QString itemText = tr("Current document · %1 · %2 · %3")
            .arg(modelName, origin.value(QStringLiteral("sourceRevision")).toString(),
                 result.value(QStringLiteral("registeredModelSha256")).toString().left(12));
        modelChoice->addItem(itemText, modelKind);
        const int index = modelChoice->count() - 1;
        modelChoice->setItemData(index, sourceId, Qt::UserRole + 1);
        modelChoice->setItemData(index, true, Qt::UserRole + 2);
        modelChoice->setCurrentIndex(index);
        updateSourceChoiceLabel(sourceId);
        updateSelectedSourceStatus();
        setActionAvailability();
        return;
    }
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
                              : command == "workflow.recover" ? tr("Recover interrupted calculation with a new attempt")
                              : command == "workflow.resume" ? tr("Continue selected calculation") : tr("Run control comparison");
        QString summary = tr("%1\nModel: %2\nTime step: %3 s\nConfigured steps: %4\nTarget damping ratio: %5\n%6")
            .arg(title, configuration.value("model").toString())
            .arg(configuration.value("dt").toDouble()).arg(configuration.value("steps").toInt())
            .arg(configuration.value("targetDamping").toDouble())
            .arg(command == "workflow.resume" ? tr("Additional steps: %1").arg(normalized.value("parameters").toObject().value("additionalSteps").toInt())
                                              : tr("FE results will be checked against an independent reference."));
        const QString sourceId = previewSourceId();
        const auto source = documentSources.constFind(sourceId);
        if (!sourceId.isEmpty() && source != documentSources.cend())
        {
            QString reason;
            const bool current = sourceRuntimeIsCurrent(source.value(), &reason);
            summary += QStringLiteral("\n\n") + sourceDescription(source.value(), true) + QStringLiteral("\n") +
                       (current ? tr("Origin document status: unchanged at preview.")
                                : tr("Origin document status: stale — %1").arg(reason));
        }
        previewText->setPlainText(summary);
        if (command == "workflow.recover")
        {
            const auto selection = result.value("recoverySelection").toObject();
            summary += tr("\nFrozen target: %1 accepted steps\nOriginal attempt: %2")
                .arg(selection.value("targetSteps").toInt()).arg(selection.value("attemptId").toString());
            const auto variants = selection.value("variants").toObject();
            for (const QString &variant : {QStringLiteral("active"), QStringLiteral("passive")})
            {
                if (!variants.value(variant).isObject()) continue;
                const auto state = variants.value(variant).toObject();
                summary += tr("\n%1: %2 · accepted %3 · remaining %4 · checkpoint %5")
                    .arg(variant, state.value("phase").toString())
                    .arg(state.value("acceptedSeq").toInt()).arg(state.value("remainingSteps").toInt())
                    .arg(state.value("checkpointSha256").toString().left(12));
            }
            previewText->setPlainText(summary);
        }
        status->setText(tr("Review the values, then apply this preview."));
        emit previewAvailable(preview);
    }
    else if (operation == "host.approve" && continuation == "execute")
    {
        if (!previewSourceIsCurrent())
        {
            commandInFlight = false;
            preview = {};
            previewText->setPlainText(tr("The captured document source changed while approval was being prepared. Capture a fresh snapshot before submitting."));
            status->setText(tr("The source became stale before task submission; the approval token was discarded."));
        }
        else
            request("command.execute", {{"previewId", preview.value("previewId")}, {"approvalToken", result.value("approvalToken")}});
    }
    else if (operation == "command.execute")
    {
        commandInFlight = false; preview = {};
        previewText->setPlainText(tr("The approved task has been submitted. Its progress appears below."));
        status->setText(tr("Task accepted."));
        foregroundJob = result.value("jobId").toString();
        pages->setCurrentIndex(0);
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
            const QString command = job.value("command").toString();
            item->setData(0, Qt::UserRole + 1, command);
            const QMap<QString, QString> labels{{"workflow.configure", tr("Settings")},
                {"workflow.run", tr("Control run")}, {"workflow.resume", tr("Continuation")},
                {"workflow.recover", tr("Recovery attempt")}};
            item->setText(0, labels.value(command, tr("Extension task")) + " · " + jobId.left(6));
            QString jobTooltip = tr("Created: %1\nTask: %2").arg(job.value("createdAt").toString(), jobId);
            const auto jobBinding = job.value(QStringLiteral("result")).toObject().value(QStringLiteral("modelBinding")).toObject();
            if (!jobBinding.isEmpty()) jobTooltip += QStringLiteral("\n") + modelBindingSummary(jobBinding);
            item->setToolTip(0, jobTooltip);
            item->setText(1, state == "succeeded" ? tr("Completed") : state);
            item->setText(2, job.value("createdAt").toString()); item->setData(1, Qt::UserRole, state);
            if (jobTree->currentItem() == item)
                monitor->setJob(command == "workflow.run" || command == "workflow.resume" ? jobId : QString(), state);
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
    if (result.contains("nativeResults"))
        workflowNativeResults->setWorkflowJob(displayedResult, nativeMode->currentData().toString());
    results->setRowCount(0);
    const QString bindingSummary = modelBindingSummary(result.value(QStringLiteral("modelBinding")).toObject());
    if (!result.contains("active") && !result.contains("passive"))
    {
        status->setText(tr("Configuration saved. You can now preview a calculation.%1")
                            .arg(bindingSummary.isEmpty() ? QString() : QStringLiteral("\n") + bindingSummary));
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
    status->setText(tr("Verified %1 steps at %2 s. Results remain tied to this task's input snapshot.%3")
        .arg(result.value("totalSteps").toInt()).arg(result.value("dtSeconds").toDouble())
        .arg(bindingSummary.isEmpty() ? QString() : QStringLiteral("\n") + bindingSummary));
    emit resultDisplayed(result);
}

void WorkflowWidget::refreshNativeJobs()
{
    if (!session || !isVisible()) return;
    const QString selected = nativeJobChoice->currentData().toString();
    const QSignalBlocker blocker(nativeJobChoice);
    QJsonArray records;
    for (int offset = 0; offset < 10000; offset += 200)
    {
        const QJsonArray page = SolverManager::getInstance().jobsQuery(offset, 200).value("jobs").toArray();
        for (const auto &job : page) records.append(job);
        if (page.size() < 200) break;
    }
    nativeJobChoice->clear();
    for (const auto &value : records)
    {
        const auto job = value.toObject();
        const QString state = job.value("state").toString();
        if (state != "succeeded" && state != "running" && state != "cancelling" && state != "interrupted") continue;
        const QString id = job.value("jobId").toString();
        nativeJobChoice->addItem(tr("%1 · %2").arg(id.left(8), state), id);
        if (id == selected) nativeJobChoice->setCurrentIndex(nativeJobChoice->count() - 1);
    }
    const QString current = nativeJobChoice->currentData().toString();
    if (current != selected)
    {
        nativeRecoveryPreviewId.clear(); nativeRecoveryJobId.clear();
        nativeRecoveryApply->setEnabled(false);
        documentNativeResults->setArtifact(current, {}, nativeJobChoice->currentText());
    }
}

void WorkflowWidget::checkNativeRecovery()
{
    nativeRecoveryPreviewId.clear(); nativeRecoveryApply->setEnabled(false);
    if (!session || nativeJobChoice->currentData().toString().isEmpty()) return;
    nativeRecoveryJobId = nativeJobChoice->currentData().toString();
    auto *host = session->getModelCommandHost();
    const auto requestHost = [host](const QString &operation, const QJsonObject &parameters) {
        return host->handle({{"apiVersion", "range.host/v1"}, {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"operation", operation}, {"parameters", parameters}});
    };
    const auto response = requestHost("native.job.recovery.query", {{"jobId", nativeRecoveryJobId}});
    const auto qualification = response.value("result").toObject();
    const auto checkpoint = qualification.value("checkpoint").toObject();
    const QMap<QString, QString> states{{"recoverable", tr("Recoverable")}, {"recompute_required", tr("Recomputation required")},
        {"reconcile_required", tr("Reconciliation required")}, {"conflict", tr("Conflict")}};
    nativeRecoveryStatus->setText(states.value(qualification.value("eligibility").toString(), tr("Unavailable"))
        + " · " + qualification.value("message").toString());
    const int remaining = checkpoint.value("remainingSteps").toInt();
    if (qualification.value("eligibility") != QJsonValue("recoverable") || remaining <= 0) return;
    const auto proposal = requestHost("command.preview", {{"command", "analysis.recover"}, {"commandVersion", 1},
        {"arguments", QJsonObject{{"jobId", nativeRecoveryJobId}, {"checkpointSha256", checkpoint.value("checkpointSha256")},
            {"remainingSteps", remaining}}}});
    if (proposal.value("status") != QJsonValue("preview"))
    { nativeRecoveryStatus->setText(proposal.value("message").toString()); return; }
    nativeRecoveryPreviewId = proposal.value("result").toObject().value("previewId").toString();
    nativeRecoveryStatus->setText(tr("Checkpoint step %1 · target %2 · remaining %3\nSHA %4\nA separate attempt will preserve the original document and job history.")
        .arg(checkpoint.value("acceptedSeq").toInt()).arg(checkpoint.value("targetSteps").toInt()).arg(remaining)
        .arg(checkpoint.value("checkpointSha256").toString()));
    nativeRecoveryApply->setEnabled(!nativeRecoveryPreviewId.isEmpty());
}

void WorkflowWidget::applyNativeRecovery()
{
    if (!session || nativeRecoveryPreviewId.isEmpty() || nativeJobChoice->currentData().toString() != nativeRecoveryJobId) return;
    auto *host = session->getModelCommandHost();
    const QString approved = nativeRecoveryPreviewId;
    nativeRecoveryPreviewId.clear(); nativeRecoveryApply->setEnabled(false);
    if (!host->approvePreview(approved))
    { nativeRecoveryStatus->setText(tr("Checkpoint qualification changed. Check recovery again before approving.")); return; }
    const auto accepted = host->handle({{"apiVersion", "range.host/v1"}, {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"operation", "command.execute"}, {"parameters", QJsonObject{{"previewId", approved},
            {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}}});
    nativeRecoveryStatus->setText(accepted.value("status") == QJsonValue("accepted")
        ? tr("Recovery attempt %1 was submitted. Its actual progress and results remain independently queryable.")
            .arg(accepted.value("result").toObject().value("jobId").toString()) : accepted.value("message").toString());
    refreshNativeJobs();
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
    QString sourceReason;
    const QString sourceId = selectedSourceId();
    const auto source = documentSources.constFind(sourceId);
    const bool selectedSourceCurrent = sourceId.isEmpty() ||
        (source != documentSources.cend() && sourceRuntimeIsCurrent(source.value(), &sourceReason));
    const QString previewSourceIdentity = previewSourceId();
    const auto previewSource = documentSources.constFind(previewSourceIdentity);
    const bool previewRuntimeCurrent = previewSourceIdentity.isEmpty() ||
        (previewSource != documentSources.cend() && sourceRuntimeIsCurrent(previewSource.value()));
    sourceButton->setEnabled(service.isReady() && session && !sourceRegistrationInFlight);
    configureButton->setEnabled(available && selectedSourceCurrent); runButton->setEnabled(available);
    modelChoice->setEnabled(!commandInFlight); dt->setEnabled(!commandInFlight);
    steps->setEnabled(!commandInFlight); damping->setEnabled(!commandInFlight);
    applyButton->setEnabled(available && !preview.isEmpty() && previewRuntimeCurrent);
    const auto *item = jobTree->currentItem();
    const auto state = item ? item->data(1, Qt::UserRole).toString() : QString();
    resumeButton->setEnabled(available && state == "succeeded" && item &&
        (item->data(0, Qt::UserRole + 1).toString() == "workflow.run" || item->data(0, Qt::UserRole + 1).toString() == "workflow.resume"));
    cancelButton->setEnabled(service.isReady() && (state == "queued" || state == "running"));
    const QString selectedCommand = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
    const bool calculation = selectedCommand == "workflow.run" || selectedCommand == "workflow.resume" || selectedCommand == "workflow.recover";
    resumeButton->setEnabled(available && state == "succeeded" && calculation);
    recoverButton->setEnabled(available && state == "interrupted" && calculation && recoveryRequest.isEmpty());
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
