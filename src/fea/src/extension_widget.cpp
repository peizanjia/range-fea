#include "extension_widget.h"

#include "workflow_process.h"
#include "series_plot_widget.h"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QUuid>
#include <algorithm>

#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSet>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QAbstractItemView>
#include <QVBoxLayout>
#include <QDir>

#include <cmath>

namespace
{
constexpr int KindRole = Qt::UserRole + 1;
constexpr int ExtensionIdRole = Qt::UserRole + 2;
constexpr int CandidateIdRole = Qt::UserRole + 3;
constexpr int BundleHashRole = Qt::UserRole + 4;
constexpr int VersionRole = Qt::UserRole + 5;
constexpr int StateRole = Qt::UserRole + 6;

constexpr int MaxUiSeriesSamples = 512;
constexpr qsizetype MaxUiSeriesTextBytes = 8192;

enum SelectionKind
{
    ExtensionSelection = 1,
    CandidateSelection,
    VersionSelection
};

QPlainTextEdit *readOnlyText(QWidget *parent, const QString &objectName)
{
    auto *edit = new QPlainTextEdit(parent);
    edit->setObjectName(objectName);
    edit->setReadOnly(true);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    return edit;
}

QString jsonText(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QJsonObject supportedScientificGui()
{
    return {{"form", QJsonArray{
        QJsonObject{{"id", "sources"}, {"type", "channel-select"}, {"minimum", 1}, {"maximum", 4}},
        QJsonObject{{"id", "window"}, {"type", "enum"}, {"values", QJsonArray{"rectangular", "hann"}}},
        QJsonObject{{"id", "removeMean"}, {"type", "enum"}, {"values", QJsonArray{false, true}}},
        QJsonObject{{"id", "filter"}, {"type", "enum"}, {"values", QJsonArray{"none", "butterworth-lowpass"}}},
        QJsonObject{{"id", "cutoffHz"}, {"type", "number"}, {"unit", "Hz"}}}},
        {"table", QJsonObject{{"columns", QJsonArray{"channel", "unit", "sampleCount", "binCount"}}}},
        {"plots", QJsonArray{"filtered-time", "amplitude", "psd", "real", "imag", "phase"}}};
}

QString jsonValueText(const QJsonValue &value)
{
    if (value.isObject()) return jsonText(value.toObject());
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
    if (value.isString()) return value.toString();
    if (value.isUndefined()) return {};
    return value.toVariant().toString();
}

bool validUnit(const QString &unit)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9./*^_-]{0,31}$"));
    return expression.match(unit).hasMatch();
}

bool parseNumberList(const QString &text, QJsonArray *array, QString *error, const QString &label)
{
    if (text.toUtf8().size() > MaxUiSeriesTextBytes)
    {
        if (error) *error = QStringLiteral("%1 input exceeds the GUI transfer limit.").arg(label);
        return false;
    }
    if (text.trimmed().isEmpty())
    {
        if (error) *error = QStringLiteral("Enter at least one %1 value.").arg(label.toLower());
        return false;
    }

    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    if (tokens.isEmpty() || tokens.size() > MaxUiSeriesSamples)
    {
        if (error) *error = QStringLiteral("Enter between 1 and %1 %2 values.").arg(MaxUiSeriesSamples).arg(label.toLower());
        return false;
    }

    QJsonArray parsed;
    for (const QString &token : tokens)
    {
        if (token.size() > 32)
        {
            if (error) *error = QStringLiteral("A %1 value is too long.").arg(label.toLower());
            return false;
        }
        bool converted = false;
        const double number = token.toDouble(&converted);
        if (!converted || !std::isfinite(number) || std::abs(number) > 1e100)
        {
            if (error) *error = QStringLiteral("Every %1 value must be a finite number with magnitude at most 1e100.").arg(label.toLower());
            return false;
        }
        parsed.append(number);
    }
    *array = parsed;
    return true;
}

QString normalizedWorkspace(const QString &workspace)
{
    if (workspace.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(workspace).absoluteFilePath());
}
}

ExtensionWidget::ExtensionWidget(WorkflowProcess *service, QWidget *parent)
    : QWidget(parent), service(service)
{
    setObjectName(QStringLiteral("extensionWidget"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Inspect service-managed extensions and operate on staged or active versions. Extension actions are previewed and require explicit host approval."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *statusRow = new QHBoxLayout;
    status = new QLabel(tr("Open a workflow workspace to inspect extensions."));
    status->setObjectName(QStringLiteral("extensionStatus"));
    status->setWordWrap(true);
    statusRow->addWidget(status, 1);
    refreshButton = new QPushButton(tr("Refresh"));
    refreshButton->setObjectName(QStringLiteral("extensionRefresh"));
    statusRow->addWidget(refreshButton);
    loadMoreButton = new QPushButton(tr("Load more candidates"));
    loadMoreButton->setObjectName(QStringLiteral("extensionLoadMoreCandidates"));
    statusRow->addWidget(loadMoreButton);
    layout->addLayout(statusRow);

    trustStatus = new QLabel(tr("Runtime trust status is not available."));
    trustStatus->setObjectName(QStringLiteral("extensionTrustStatus"));
    trustStatus->setWordWrap(true);
    layout->addWidget(trustStatus);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    extensionTree = new QTreeWidget(splitter);
    extensionTree->setObjectName(QStringLiteral("extensionList"));
    extensionTree->setHeaderLabels({tr("Extension / version"), tr("State"), tr("Bundle hash")});
    extensionTree->setRootIsDecorated(true);
    extensionTree->setAccessibleName(tr("Installed extensions and staged candidates"));
    extensionTree->setMinimumWidth(310);

    detailTabs = new QTabWidget(splitter);
    detailTabs->setObjectName(QStringLiteral("extensionDetails"));
    manifestView = readOnlyText(detailTabs, QStringLiteral("extensionManifest"));
    sourceView = readOnlyText(detailTabs, QStringLiteral("extensionSource"));
    diffView = readOnlyText(detailTabs, QStringLiteral("extensionDiff"));
    testsView = readOnlyText(detailTabs, QStringLiteral("extensionTests"));
    reportView = readOnlyText(detailTabs, QStringLiteral("extensionReport"));
    detailTabs->addTab(manifestView, tr("Manifest"));
    detailTabs->addTab(sourceView, tr("Source"));
    detailTabs->addTab(diffView, tr("Diff"));
    detailTabs->addTab(testsView, tr("Tests"));
    detailTabs->addTab(reportView, tr("Validation report"));
    splitter->addWidget(extensionTree);
    splitter->addWidget(detailTabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 2);

    auto *actions = new QHBoxLayout;
    validateButton = new QPushButton(tr("Preview validation"));
    validateButton->setObjectName(QStringLiteral("extensionValidate"));
    activateButton = new QPushButton(tr("Preview activation"));
    activateButton->setObjectName(QStringLiteral("extensionActivate"));
    rollbackTarget = new QComboBox;
    rollbackTarget->setObjectName(QStringLiteral("extensionRollbackTarget"));
    rollbackTarget->setAccessibleName(tr("Retained version to restore"));
    rollbackButton = new QPushButton(tr("Preview rollback"));
    rollbackButton->setObjectName(QStringLiteral("extensionRollback"));
    actions->addWidget(validateButton);
    actions->addWidget(activateButton);
    actions->addWidget(rollbackTarget, 1);
    actions->addWidget(rollbackButton);
    layout->addLayout(actions);

    // Keep input, plots, provenance and approval reachable on small/high-DPI
    // desktops instead of allowing their combined minimum heights to grow the
    // top-level window beyond the available screen.
    auto *featureScroll = new QScrollArea(this);
    featureScroll->setObjectName("extensionFeatureScroll");
    featureScroll->setWidgetResizable(true);
    featureScroll->setMinimumHeight(120);
    featureScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *featureContent = new QWidget(featureScroll);
    auto *featureContentLayout = new QVBoxLayout(featureContent);
    featureScroll->setWidget(featureContent);
    layout->addWidget(featureScroll, 3);

    auto *featureGroup = new QGroupBox(tr("Active extension feature"), this);
    featureGroup->setObjectName(QStringLiteral("extensionFeature"));
    auto *featureLayout = new QVBoxLayout(featureGroup);
    auto *sourceRow = new QHBoxLayout;
    sourceMode = new QComboBox(featureGroup);
    sourceMode->setObjectName(QStringLiteral("extensionSourceMode"));
    sourceMode->addItem(tr("Enter a numeric series"), QStringLiteral("series"));
    sourceMode->addItem(tr("Use an independent reference result"), QStringLiteral("provenance"));
    sourceMode->addItem(tr("Use actual active FE frames"), QStringLiteral("native-active"));
    sourceMode->addItem(tr("Use actual passive FE frames"), QStringLiteral("native-passive"));
    sourceJob = new QComboBox(featureGroup);
    sourceJob->setObjectName(QStringLiteral("extensionSourceJob"));
    sourceSignal = new QComboBox(featureGroup);
    sourceSignal->setObjectName(QStringLiteral("extensionSourceSignal"));
    sourceSignal->addItem(tr("Active sensor velocity (m/s)"), QStringLiteral("reference.active.sensorVelocity"));
    sourceSignal->addItem(tr("Active energy (J)"), QStringLiteral("reference.active.energyJ"));
    sourceSignal->addItem(tr("Passive energy (J)"), QStringLiteral("reference.passive.energyJ"));
    sourceRow->addWidget(sourceMode);
    sourceRow->addWidget(sourceJob, 1);
    sourceRow->addWidget(sourceSignal, 1);
    featureLayout->addLayout(sourceRow);

    auto *seriesForm = new QFormLayout;
    timesInput = new QPlainTextEdit(featureGroup);
    timesInput->setObjectName(QStringLiteral("extensionSeriesTimes"));
    timesInput->setPlaceholderText(tr("0, 0.01, 0.02"));
    timesInput->setMaximumHeight(58);
    valuesInput = new QPlainTextEdit(featureGroup);
    valuesInput->setObjectName(QStringLiteral("extensionSeriesValues"));
    valuesInput->setPlaceholderText(tr("0, 1.2, 1.8"));
    valuesInput->setMaximumHeight(58);
    timeUnitInput = new QComboBox(featureGroup);
    timeUnitInput->setObjectName(QStringLiteral("extensionTimeUnit"));
    timeUnitInput->setEditable(true);
    timeUnitInput->addItems({QStringLiteral("s"), QStringLiteral("ms")});
    valueUnitInput = new QComboBox(featureGroup);
    valueUnitInput->setObjectName(QStringLiteral("extensionValueUnit"));
    valueUnitInput->setEditable(true);
    valueUnitInput->addItems({QStringLiteral("1"), QStringLiteral("m"), QStringLiteral("m/s"),
                              QStringLiteral("J"), QStringLiteral("N"), QStringLiteral("Pa")});
    seriesForm->addRow(tr("Times (CSV, strictly increasing)"), timesInput);
    seriesForm->addRow(tr("Values (CSV)"), valuesInput);
    seriesForm->addRow(tr("Time unit"), timeUnitInput);
    seriesForm->addRow(tr("Value unit"), valueUnitInput);
    featureLayout->addLayout(seriesForm);

    scientificForm = new QWidget(featureGroup);
    scientificForm->setObjectName("extensionScientificForm");
    auto *scienceLayout = new QFormLayout(scientificForm);
    windowInput = new QComboBox(scientificForm); windowInput->setObjectName("extensionWindow");
    windowInput->addItems({"rectangular", "hann"});
    removeMeanInput = new QCheckBox(tr("Remove mean"), scientificForm); removeMeanInput->setObjectName("extensionRemoveMean");
    filterInput = new QComboBox(scientificForm); filterInput->setObjectName("extensionFilter");
    filterInput->addItems({"none", "butterworth-lowpass"});
    cutoffInput = new QDoubleSpinBox(scientificForm); cutoffInput->setObjectName("extensionCutoffHz");
    cutoffInput->setDecimals(6); cutoffInput->setRange(0.000001, 1e12); cutoffInput->setValue(1.0); cutoffInput->setSuffix(" Hz");
    scienceLayout->addRow(tr("Window"), windowInput); scienceLayout->addRow(removeMeanInput);
    scienceLayout->addRow(tr("Filter (order 1, causal, zero initial condition)"), filterInput);
    scienceLayout->addRow(tr("Cutoff"), cutoffInput);
    sourcesTable = new QTableWidget(0, 4, scientificForm); sourcesTable->setObjectName("extensionSources");
    sourcesTable->setHorizontalHeaderLabels({tr("Job"), tr("Mode"), tr("Artifact"), tr("Channel")});
    sourcesTable->setEditTriggers(QAbstractItemView::NoEditTriggers); sourcesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    scienceLayout->addRow(sourcesTable);
    addSourceButton = new QPushButton(tr("Add verified native channel"), scientificForm); addSourceButton->setObjectName("extensionAddSource");
    removeSourceButton = new QPushButton(tr("Remove selected source"), scientificForm); removeSourceButton->setObjectName("extensionRemoveSource");
    scienceLayout->addRow(addSourceButton, removeSourceButton);
    featureLayout->addWidget(scientificForm); scientificForm->hide();

    auto *provenanceNote = new QLabel(tr("Native FE channels use published MODEL frames and a registered manifest. Independent reference histories and manually entered series retain their own source labels. The service revalidates the selected source before and after execution."), featureGroup);
    provenanceNote->setObjectName(QStringLiteral("extensionProvenanceNote"));
    provenanceNote->setWordWrap(true);
    featureLayout->addWidget(provenanceNote);
    runButton = new QPushButton(tr("Preview feature run"), featureGroup);
    runButton->setObjectName(QStringLiteral("extensionRun"));
    featureLayout->addWidget(runButton, 0, Qt::AlignRight);
    featureContentLayout->addWidget(featureGroup);

    metricsTable = new QTableWidget(0, 3, this);
    metricsTable->setObjectName(QStringLiteral("extensionMetrics"));
    metricsTable->setHorizontalHeaderLabels({tr("Metric"), tr("Value"), tr("Unit")});
    metricsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    metricsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    metricsTable->setAccessibleName(tr("Extension metrics with units"));
    featureContentLayout->addWidget(metricsTable, 1);

    scienceView = new QComboBox(this); scienceView->setObjectName("extensionScienceView");
    scienceView->addItems({"filtered-time", "amplitude", "psd", "real", "imag", "phase"});
    pendingSciencePlots = new QTabWidget(this); pendingSciencePlots->hide();
    sciencePlots = new QTabWidget(this); sciencePlots->setObjectName("extensionSciencePlots");
    scienceProvenance = new QLabel(this); scienceProvenance->setObjectName("extensionScienceProvenance"); scienceProvenance->setWordWrap(true);
    featureContentLayout->addWidget(scienceView); featureContentLayout->addWidget(sciencePlots); featureContentLayout->addWidget(scienceProvenance);
    scienceView->hide(); sciencePlots->hide(); scienceProvenance->hide();

    auto *previewGroup = new QGroupBox(tr("Command preview and result"), this);
    previewGroup->setObjectName(QStringLiteral("extensionApproval"));
    auto *previewLayout = new QVBoxLayout(previewGroup);
    previewView = readOnlyText(previewGroup, QStringLiteral("extensionPreview"));
    previewView->setMaximumHeight(135);
    previewLayout->addWidget(previewView);
    auto *approveRow = new QHBoxLayout;
    auto *approvalNote = new QLabel(tr("Nothing is changed until you approve this exact preview."), previewGroup);
    approvalNote->setWordWrap(true);
    approveRow->addWidget(approvalNote, 1);
    approveButton = new QPushButton(tr("Approve and execute"), previewGroup);
    approveButton->setObjectName(QStringLiteral("extensionApprove"));
    approveRow->addWidget(approveButton);
    previewLayout->addLayout(approveRow);
    resultView = readOnlyText(previewGroup, QStringLiteral("extensionResult"));
    resultView->setMaximumHeight(110);
    previewLayout->addWidget(resultView);
    featureContentLayout->addWidget(previewGroup, 1);

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(1800);
    jobPollTimer = new QTimer(this);
    jobPollTimer->setInterval(500);
    runtimeVerificationTimer = new QTimer(this);
    runtimeVerificationTimer->setInterval(1000);
    connect(runtimeVerificationTimer, &QTimer::timeout, this, &ExtensionWidget::pollRuntimeVerification);

    connect(refreshButton, &QPushButton::clicked, this, [this] {
        refreshExtensions();
        refreshJobs();
        if (!runtimeVerificationReady && !runtimeVerificationTimer->isActive() && runtimeVerificationRequest.isEmpty())
        {
            runtimeVerificationGeneration.clear(); runtimeVerificationId.clear(); startRuntimeVerification();
        }
        else if (!runtimeVerificationId.isEmpty()) pollRuntimeVerification();
    });
    connect(loadMoreButton, &QPushButton::clicked, this, &ExtensionWidget::loadMoreCandidates);
    connect(validateButton, &QPushButton::clicked, this, &ExtensionWidget::validateSelectedCandidate);
    connect(activateButton, &QPushButton::clicked, this, &ExtensionWidget::activateSelectedCandidate);
    connect(rollbackButton, &QPushButton::clicked, this, &ExtensionWidget::rollbackSelectedExtension);
    connect(runButton, &QPushButton::clicked, this, &ExtensionWidget::runSelectedExtension);
    connect(approveButton, &QPushButton::clicked, this, &ExtensionWidget::approvePreview);
    connect(extensionTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
        if (rebuildInProgress) return;
        if (!deferredSelectionKey.isEmpty() && current &&
            current->data(0, ExtensionIdRole).toString() != deferredSelectionExtensionId)
        {
            deferredSelectionKey.clear();
            deferredSelectionExtensionId.clear();
        }
        else if (!deferredSelectionKey.isEmpty() && current &&
                 current->data(0, KindRole).toInt() != ExtensionSelection &&
                 currentSelectionKey() != deferredSelectionKey)
        {
            deferredSelectionKey.clear();
            deferredSelectionExtensionId.clear();
        }
        scientificGeneration.clear();
        clearPreview(tr("Selection changed. Create a new preview before applying an action."));
        requestSelectedDetails(current);
        updateAvailability();
    });
    connect(rollbackTarget, &QComboBox::currentIndexChanged, this, [this] {
        if (!currentPreview.isEmpty()) clearPreview(tr("Rollback target changed. Create a new preview."));
        updateAvailability();
    });
    connect(sourceMode, &QComboBox::currentIndexChanged, this, [this] {
        if (!currentPreview.isEmpty()) clearPreview(tr("Run input changed. Create a new preview."));
        updateAvailability();
    });
    const auto invalidateRunPreview = [this] {
        if (!currentPreview.isEmpty()) clearPreview(tr("Run input changed. Create a new preview."));
        updateAvailability();
    };
    connect(windowInput, &QComboBox::currentIndexChanged, this, invalidateRunPreview);
    connect(removeMeanInput, &QCheckBox::toggled, this, invalidateRunPreview);
    connect(filterInput, &QComboBox::currentIndexChanged, this, invalidateRunPreview);
    connect(cutoffInput, &QDoubleSpinBox::valueChanged, this, invalidateRunPreview);
    connect(addSourceButton, &QPushButton::clicked, this, &ExtensionWidget::addScientificSource);
    connect(removeSourceButton, &QPushButton::clicked, this, [this] {
        const int row = sourcesTable->currentRow(); if (row < 0 || row >= scientificSources.size()) return;
        scientificSources.removeAt(row); refreshScientificSources();
        clearPreview(tr("Sources changed. Create a new preview.")); updateAvailability();
    });
    connect(scienceView, &QComboBox::currentIndexChanged, this, &ExtensionWidget::loadScientificView);
    connect(timesInput, &QPlainTextEdit::textChanged, this, invalidateRunPreview);
    connect(valuesInput, &QPlainTextEdit::textChanged, this, invalidateRunPreview);
    connect(timeUnitInput, &QComboBox::currentTextChanged, this, invalidateRunPreview);
    connect(valueUnitInput, &QComboBox::currentTextChanged, this, invalidateRunPreview);
    connect(sourceJob, &QComboBox::currentIndexChanged, this, invalidateRunPreview);
    connect(sourceSignal, &QComboBox::currentIndexChanged, this, invalidateRunPreview);
    connect(sourceMode, &QComboBox::currentIndexChanged, this, &ExtensionWidget::refreshSourceSignals);
    connect(sourceJob, &QComboBox::currentIndexChanged, this, &ExtensionWidget::refreshSourceSignals);
    connect(refreshTimer, &QTimer::timeout, this, [this] {
        refreshExtensions();
        refreshJobs();
    });
    connect(jobPollTimer, &QTimer::timeout, this, &ExtensionWidget::pollJob);

    if (this->service)
    {
        connect(this->service, &WorkflowProcess::readyChanged, this, &ExtensionWidget::handleReadyChanged);
        connect(this->service, &WorkflowProcess::replyReceived, this, &ExtensionWidget::onReply);
        connect(this->service, &WorkflowProcess::requestFailed, this,
                [this](const QString &id, const QString &operation, const QString &code, const QString &message) {
            onRequestFailed(id, operation, code, message);
        });
        connect(this->service, &QObject::destroyed, this, [this] {
            runtimeVerificationTimer->stop(); runtimeVerificationReady = false;
            runtimeVerificationGeneration.clear(); runtimeVerificationId.clear(); runtimeVerificationRequest.clear();
            refreshTimer->stop();
            jobPollTimer->stop();
            scientificGeneration.clear();
        pendingRequests.clear();
            this->service.clear();
            commandInFlight = false;
            runtimeCapabilityKnown = false;
            trustEnabled = false;
            status->setText(tr("Workflow service closed."));
            updateTrustStatus();
            clearPreview();
            updateAvailability();
        });
        if (this->service->isReady()) handleReadyChanged(true);
    }
    updateTrustStatus();
    updateAvailability();
}

bool ExtensionWidget::parseNumericSeries(const QString &timesText, const QString &valuesText,
                                         const QString &timeUnit, const QString &valueUnit,
                                         QJsonObject *series, QString *error)
{
    if (error) error->clear();
    if (!series)
    {
        if (error) *error = QStringLiteral("Series output is required.");
        return false;
    }
    if (!validUnit(timeUnit) || !validUnit(valueUnit))
    {
        if (error) *error = QStringLiteral("Units must use 1 to 32 ASCII letters, digits, or . / * ^ _ - characters, and start with a letter or digit.");
        return false;
    }
    QJsonArray times;
    QJsonArray values;
    if (!parseNumberList(timesText, &times, error, QStringLiteral("Times")) ||
        !parseNumberList(valuesText, &values, error, QStringLiteral("Values")))
        return false;
    if (times.size() != values.size())
    {
        if (error) *error = QStringLiteral("Times and values must contain the same number of samples.");
        return false;
    }
    for (qsizetype i = 1; i < times.size(); ++i)
    {
        if (times.at(i).toDouble() <= times.at(i - 1).toDouble())
        {
            if (error) *error = QStringLiteral("Times must be strictly increasing.");
            return false;
        }
    }
    *series = {{QStringLiteral("times"), times}, {QStringLiteral("values"), values},
               {QStringLiteral("timeUnit"), timeUnit}, {QStringLiteral("valueUnit"), valueUnit}};
    return true;
}

QString ExtensionWidget::request(const QString &operation, const QJsonObject &parameters,
                                 const PendingRequest &context)
{
    if (!service || !service->isReady()) return {};
    const QString id = service->request(operation, parameters);
    if (!id.isEmpty())
    {
        PendingRequest correlated = context;
        correlated.operation = operation;
        pendingRequests.insert(id, correlated);
    }
    return id;
}

void ExtensionWidget::handleReadyChanged(bool ready)
{
    if (!ready)
    {
        runtimeVerificationTimer->stop();
        runtimeVerificationGeneration.clear(); runtimeVerificationId.clear(); runtimeVerificationRequest.clear();
        runtimeVerificationReady = false; runtimeVerificationAvailable = false; runtimeVerificationMessage.clear();
        refreshTimer->stop();
        jobPollTimer->stop();
        pendingRequests.clear();
        extensionListRequest.clear();
        jobsRequest.clear();
        extensionDetailsRequests.clear();
        commandInFlight = false;
        clearPreview(tr("Workspace closed. Reopen it and refresh before continuing."));
        runtimeCapabilityKnown = false;
        trustEnabled = false;
        updateTrustStatus();
        if (!pendingJobId.isEmpty())
            status->setText(tr("Workspace disconnected while job %1 was active. Reopen the same workspace to reconcile its state.").arg(pendingJobId));
        else
            status->setText(tr("Workflow workspace is closed."));
        updateAvailability();
        return;
    }

    const QString workspace = normalizedWorkspace(service->configuration().workspace);
    if (!observedWorkspace.isEmpty() && observedWorkspace != workspace)
    {
        resetScientificWorkspace();
        pendingJobId.clear();
        pendingJobCommand.clear();
        jobPollTimer->stop();
        extensionListResult = {};
        extensionDetails.clear();
        extensionDetailsRequests.clear();
        selectedCandidate = {};
        selectedVersion = {};
        selectedExtensionId.clear();
        selectedCandidateId.clear();
        selectedBundleHash.clear();
        deferredSelectionKey.clear();
        deferredSelectionExtensionId.clear();
        extensionTree->clear();
        clearDetails(tr("Select an extension or candidate to inspect its service-provided details."));
    }
    observedWorkspace = workspace;
    runtimeCapabilityKnown = false;
    trustEnabled = false;
    manifestSchema = {};
    numericSeriesSchema = {};
    refreshSelectedDetailsAfterList = true;
    status->setText(tr("Workflow service connected; loading extension state."));
    request("schema.get", {{"schemaId", "range.extension/v2/manifest"}}, PendingRequest{RequestKind::ScientificSchema});
    request(QStringLiteral("capabilities.query"), {}, PendingRequest{RequestKind::Capabilities});
    request(QStringLiteral("schema.get"), {{QStringLiteral("schemaId"), QStringLiteral("range.extension/v1/manifest")}},
            PendingRequest{RequestKind::ManifestSchema});
    request(QStringLiteral("schema.get"), {{QStringLiteral("schemaId"), QStringLiteral("range.extension/v1/numeric-series")}},
            PendingRequest{RequestKind::NumericSeriesSchema});
    refreshExtensions();
    refreshJobs();
    refreshTimer->start();
    if (!pendingJobId.isEmpty())
    {
        status->setText(tr("Reconnecting to extension job %1.").arg(pendingJobId));
        jobPollTimer->start();
    }
    updateAvailability();
}

void ExtensionWidget::refreshExtensions()
{
    if (!service || !service->isReady() || !extensionListRequest.isEmpty()) return;
    extensionListRequest = request(QStringLiteral("extension.list"), {}, PendingRequest{RequestKind::ExtensionList});
}

QString ExtensionWidget::extensionDetailsRequestKey(const QString &extensionId, int candidateOffset) const
{
    return extensionId + QLatin1Char('\n') + QString::number(candidateOffset);
}

void ExtensionWidget::requestExtensionDetails(const QString &extensionId, int candidateOffset)
{
    if (!service || !service->isReady() || extensionId.isEmpty() || candidateOffset < 0) return;
    const QString key = extensionDetailsRequestKey(extensionId, candidateOffset);
    if (extensionDetailsRequests.contains(key)) return;
    const QString requestId = request(QStringLiteral("extension.list"),
        {{QStringLiteral("extensionId"), extensionId},
         {QStringLiteral("candidateOffset"), candidateOffset},
         {QStringLiteral("candidateLimit"), 16}},
        PendingRequest{RequestKind::ExtensionDetails, extensionId, {}, {}, {}, {}, candidateOffset});
    if (!requestId.isEmpty()) extensionDetailsRequests.insert(key, requestId);
}

void ExtensionWidget::loadMoreCandidates()
{
    const QJsonObject extension = currentExtensionSummary();
    const QJsonValue nextOffset = extension.value(QStringLiteral("nextCandidateOffset"));
    if (extension.isEmpty() || !nextOffset.isDouble()) return;
    requestExtensionDetails(selectedExtensionId, nextOffset.toInt());
    status->setText(tr("Loading staged candidates %1 onward for %2…")
                    .arg(nextOffset.toInt() + 1).arg(selectedExtensionId));
    updateAvailability();
}

void ExtensionWidget::refreshJobs()
{
    if (!service || !service->isReady() || !jobsRequest.isEmpty() || !pendingJobId.isEmpty()) return;
    jobsRequest = request(QStringLiteral("jobs.query"), {}, PendingRequest{RequestKind::Jobs});
}

void ExtensionWidget::onReply(const QString &requestId, const QString &operation, const QJsonObject &result)
{
    auto found = pendingRequests.find(requestId);
    if (found == pendingRequests.end()) return;
    const PendingRequest context = found.value();
    pendingRequests.erase(found);
    if (requestId == extensionListRequest) extensionListRequest.clear();
    if (requestId == jobsRequest) jobsRequest.clear();
    if (context.kind == RequestKind::ExtensionDetails)
    {
        const QString key = extensionDetailsRequestKey(context.extensionId, context.candidateOffset);
        if (extensionDetailsRequests.value(key) == requestId) extensionDetailsRequests.remove(key);
    }
    if (context.operation != operation)
    {
        if (context.kind == RequestKind::ProjectForPreview || context.kind == RequestKind::CommandPreview ||
            context.kind == RequestKind::HostApproval || context.kind == RequestKind::CommandExecute)
        {
            commandInFlight = false;
            clearPreview(tr("The service reply did not match its request operation."));
        }
        updateAvailability();
        return;
    }

    switch (context.kind)
    {
        case RequestKind::ScientificSchema:
            if (result.value("apiVersion") == QJsonValue("range.extension/v2") && result.value("schemaId") == QJsonValue("range.extension/v2/manifest"))
                scientificGui = result.value("schema").toObject().value("supportedGui").toObject();
            if (selectedVersion.value("manifest").toObject().value("sdkVersion") == QJsonValue("range.extension/v2"))
                renderFormAndMetrics(selectedVersion.value("manifest").toObject());
            startRuntimeVerification(); updateAvailability(); break;
        case RequestKind::RuntimeVerifyStart:
        case RequestKind::RuntimeVerifyPoll:
        {
            if (context.intent != runtimeVerificationGeneration || context.extensionId != observedWorkspace ||
                observedWorkspace != normalizedWorkspace(service->configuration().workspace)) break;
            runtimeVerificationRequest.clear();
            const QString id = result.value("verificationId").toString();
            const QString state = result.value("state").toString();
            if (id.isEmpty() || (!runtimeVerificationId.isEmpty() && id != runtimeVerificationId))
            {
                runtimeVerificationTimer->stop(); runtimeVerificationReady = false;
                runtimeVerificationMessage = tr("Private scientific environment verification returned an inconsistent identity.");
            }
            else
            {
                runtimeVerificationId = id;
                if (state == "ready" && result.value("isCurrent").toBool() &&
                    !result.value("runtimeHash").toString().isEmpty() &&
                    result.value("profile") == QJsonValue("packaged") &&
                    !result.value("expectedEnvironmentStamp").isNull() && !result.value("expectedEnvironmentStamp").isUndefined() &&
                    result.value("fullFileVerify").toObject().value("verifiedFiles").toBool() &&
                    result.value("fullFileVerify").toObject().value("runtimeHash") == result.value("runtimeHash") &&
                    result.value("fullFileVerify").toObject().value("environmentStamp") == result.value("expectedEnvironmentStamp"))
                {
                    runtimeVerificationReady = true; runtimeVerificationTimer->stop();
                    runtimeVerificationMessage = tr("Private scientific environment checked at %1. Currentity checks cover the environment stamp; execution rechecks all frozen file bytes. This does not establish an OS sandbox.")
                        .arg(result.value("verifiedAt").toVariant().toString());
                }
                else if (state == "queued" || state == "running")
                {
                    runtimeVerificationReady = false; runtimeVerificationTimer->start();
                    runtimeVerificationMessage = tr("Checking private scientific environment…");
                }
                else
                {
                    runtimeVerificationReady = false; runtimeVerificationTimer->stop();
                    runtimeVerificationMessage = tr("Private scientific environment verification failed: %1")
                        .arg(jsonValueText(result.value("error")));
                    clearPreview(runtimeVerificationMessage);
                }
            }
            updateTrustStatus(); updateAvailability(); break;
        }
        case RequestKind::ScientificResult:
            if (context.intent == scientificGeneration && context.extensionId == scientificJobId &&
                scientificSelection == currentSelectionKey() && context.bundleHash == selectedBundleHash)
                consumeScientificPage(result);
            break;
        case RequestKind::Capabilities:
        {
            const QJsonValue runtimeValue = result.value(QStringLiteral("extensionRuntime"));
            runtimeCapabilityKnown = runtimeValue.isObject();
            activeRuntimeCapability = runtimeValue.toObject();
            trustEnabled = runtimeCapabilityKnown &&
                activeRuntimeCapability.value(QStringLiteral("protocolVersion")).toString() == QStringLiteral("range.extension/v1") &&
                  activeRuntimeCapability.value(QStringLiteral("trustedExtensions")).toBool();
            QSet<QString> operations;
            for (const auto &value : result.value("operations").toArray()) operations.insert(value.toObject().value("id").toString());
            runtimeVerificationAvailable = operations.contains("extension.runtime.verify") && operations.contains("extension.runtime.verification.query");
            startRuntimeVerification();
            updateTrustStatus();
            updateAvailability();
            break;
        }
        case RequestKind::NativeChannels:
        {
            if (requestId != nativeChannelRequest || context.intent != nativeSourceKey) break;
            nativeChannelRequest.clear();
            if (result.value("artifactId") != nativeSourceDescriptor.value("artifactId")) break;
            const QSignalBlocker blocker(sourceSignal);
            for (const auto &value : result.value("channels").toArray())
            {
                const auto channel = value.toObject();
                const QString id = channel.value("channelId").toString();
                if (id.isEmpty() || sourceSignal->findData(id) >= 0) continue;
                sourceSignal->addItem(id + " [" + channel.value("unit").toString() + "]", id);
                if (id == desiredNativeChannel) sourceSignal->setCurrentIndex(sourceSignal->count() - 1);
            }
            if (result.contains("nextCursor"))
            {
                const QString mode = sourceMode->currentData().toString().mid(7);
                nativeChannelRequest = request("result.channels.query", {{"jobId", sourceJob->currentData().toString()},
                    {"mode", mode}, {"artifactId", nativeSourceDescriptor.value("artifactId")},
                    {"cursor", result.value("nextCursor")}, {"limit", 200}}, context);
            }
            updateAvailability();
            break;
        }
        case RequestKind::ManifestSchema:
            if (result.value(QStringLiteral("apiVersion")).toString() == QStringLiteral("range.extension/v1") &&
                result.value(QStringLiteral("schemaId")).toString() == QStringLiteral("range.extension/v1/manifest"))
                manifestSchema = result.value(QStringLiteral("schema")).toObject();
            updateAvailability();
            break;
        case RequestKind::NumericSeriesSchema:
            if (result.value(QStringLiteral("apiVersion")).toString() == QStringLiteral("range.extension/v1") &&
                result.value(QStringLiteral("schemaId")).toString() == QStringLiteral("range.extension/v1/numeric-series"))
                numericSeriesSchema = result.value(QStringLiteral("schema")).toObject();
            updateAvailability();
            break;
        case RequestKind::ExtensionList:
            extensionListResult = result;
            renderExtensionList(result);
            break;
        case RequestKind::ExtensionDetails:
        {
            const QJsonObject entry = extensionDetailsEntry(result);
            const QJsonObject summary = rootExtensionSummary(context.extensionId);
            if (entry.isEmpty() || summary.isEmpty() ||
                entry.value(QStringLiteral("registryRevision")) != summary.value(QStringLiteral("registryRevision")))
            {
                refreshExtensions();
                status->setText(tr("Extension summaries changed while loading details. Refreshing the list."));
                break;
            }
            QJsonObject merged = entry;
            if (context.candidateOffset > 0)
            {
                const QJsonObject previous = extensionDetails.value(context.extensionId);
                QJsonArray candidates = previous.value(QStringLiteral("candidates")).toArray();
                QSet<QString> candidateIds;
                for (const QJsonValue &candidateValue : candidates)
                    candidateIds.insert(candidateValue.toObject().value(QStringLiteral("candidateId")).toString());
                for (const QJsonValue &candidateValue : entry.value(QStringLiteral("candidates")).toArray())
                {
                    const QJsonObject candidate = candidateValue.toObject();
                    const QString candidateId = candidate.value(QStringLiteral("candidateId")).toString();
                    if (!candidateId.isEmpty() && !candidateIds.contains(candidateId))
                    {
                        candidates.append(candidate);
                        candidateIds.insert(candidateId);
                    }
                }
                merged.insert(QStringLiteral("candidates"), candidates);
            }
            extensionDetails.insert(context.extensionId, merged);
            renderExtensionList(extensionListResult);
            break;
        }
        case RequestKind::CandidateDetails:
            if (context.candidateId == selectedCandidateId && context.extensionId == selectedExtensionId)
                renderCandidate(result.value(QStringLiteral("candidate")).toObject());
            break;
        case RequestKind::VersionDetails:
            if (context.extensionId == selectedExtensionId && context.bundleHash == selectedBundleHash)
                renderVersion(result.value(QStringLiteral("version")).toObject());
            break;
        case RequestKind::Jobs:
        {
            const QString selectedJobId = sourceJob->currentData().toString();
            availableJobs = result.value(QStringLiteral("jobs")).toArray();
            const QSignalBlocker sourceJobSignals(sourceJob);
            sourceJob->clear();
            for (const QJsonValue &jobValue : availableJobs)
            {
                const QJsonObject job = jobValue.toObject();
                const QString command = job.value(QStringLiteral("command")).toString();
                if (job.value(QStringLiteral("state")).toString() != QStringLiteral("succeeded") ||
                    (command != QStringLiteral("workflow.run") && command != QStringLiteral("workflow.resume")))
                    continue;
                const QString jobId = job.value(QStringLiteral("jobId")).toString();
                const QString label = tr("%1 · %2 · %3").arg(command, jobId.left(8), job.value(QStringLiteral("completedAt")).toString());
                sourceJob->addItem(label, jobId);
                if (jobId == selectedJobId) sourceJob->setCurrentIndex(sourceJob->count() - 1);
            }
            const QString refreshedJobId = sourceJob->currentData().toString();
            if (selectedJobId != refreshedJobId &&
                sourceMode->currentData().toString() == QStringLiteral("provenance"))
            {
                const QJsonObject normalized = currentPreview.value(QStringLiteral("normalized")).toObject();
                const QJsonObject previewParameters = normalized.value(QStringLiteral("parameters")).toObject();
                const QString previewJobId = previewParameters.value(QStringLiteral("provenance")).toObject()
                    .value(QStringLiteral("jobId")).toString();
                if (normalized.value(QStringLiteral("command")).toString() == QStringLiteral("extension.run") &&
                    !selectedJobId.isEmpty() && previewJobId == selectedJobId)
                    clearPreview(tr("Reference source changed. Create a new preview before running the extension."));
            }
            updateAvailability();
            refreshSourceSignals();
            break;
        }
        case RequestKind::ProjectForPreview:
        {
            const QJsonValue revision = result.value(QStringLiteral("revision"));
            if (!revision.isDouble())
            {
                commandInFlight = false;
                status->setText(tr("The workflow service did not return a project revision; no preview was created."));
                updateAvailability();
                break;
            }
            request(QStringLiteral("command.preview"),
                    {{QStringLiteral("command"), pendingCommand},
                     {QStringLiteral("parameters"), pendingCommandParameters},
                     {QStringLiteral("expectedRevision"), revision}},
                    PendingRequest{RequestKind::CommandPreview, selectedExtensionId,
                                   selectedCandidateId, selectedBundleHash, pendingIntent});
            break;
        }
        case RequestKind::CommandPreview:
            if (context.extensionId != selectedExtensionId || context.candidateId != selectedCandidateId ||
                context.bundleHash != selectedBundleHash ||
                result.value(QStringLiteral("normalized")).toObject().value(QStringLiteral("command")).toString() != pendingCommand ||
                result.value(QStringLiteral("previewId")).toString().isEmpty() ||
                result.value(QStringLiteral("previewHash")).toString().isEmpty())
            {
                commandInFlight = false;
                clearPreview(tr("The preview no longer matches the selected extension or command. Create a fresh preview."));
                updateAvailability();
                break;
            }
            currentPreview = result;
            commandInFlight = false;
            previewView->setPlainText(jsonText(result));
            status->setText(tr("Review the service preview. No extension action has run yet."));
            updateAvailability();
            break;
        case RequestKind::HostApproval:
        {
            const QString approvalToken = result.value(QStringLiteral("approvalToken")).toString();
            if (approvalToken.isEmpty() || currentPreview.isEmpty())
            {
                commandInFlight = false;
                clearPreview(tr("The service did not return a valid approval token."));
                updateAvailability();
                break;
            }
            request(QStringLiteral("command.execute"),
                    {{QStringLiteral("previewId"), currentPreview.value(QStringLiteral("previewId"))},
                     {QStringLiteral("approvalToken"), approvalToken}},
                    PendingRequest{RequestKind::CommandExecute, context.extensionId,
                                   context.candidateId, context.bundleHash, context.intent});
            break;
        }
        case RequestKind::CommandExecute:
            pendingJobId = result.value(QStringLiteral("jobId")).toString();
            pendingJobCommand = pendingCommand;
            if (pendingJobId.isEmpty())
            {
                commandInFlight = false;
                clearPreview(tr("The service accepted no job identifier; refresh extension state before retrying."));
                status->setText(tr("The extension operation could not be correlated with a job."));
                updateAvailability();
                break;
            }
            currentPreview = {};
            previewView->clear();
            status->setText(tr("Approved job %1 was submitted. Waiting for its service result.").arg(pendingJobId));
            jobPollTimer->start();
            pollJob();
            break;
        case RequestKind::JobPoll:
            renderJob(result.value(QStringLiteral("job")).toObject());
            break;
    }
}

void ExtensionWidget::onRequestFailed(const QString &requestId, const QString &operation,
                                      const QString &code, const QString &message)
{
    auto found = pendingRequests.find(requestId);
    if (found == pendingRequests.end()) return;
    const PendingRequest context = found.value();
    pendingRequests.erase(found);
    if (requestId == extensionListRequest) extensionListRequest.clear();
    if (requestId == jobsRequest) jobsRequest.clear();
    if (context.kind == RequestKind::ExtensionDetails)
    {
        const QString key = extensionDetailsRequestKey(context.extensionId, context.candidateOffset);
        if (extensionDetailsRequests.value(key) == requestId) extensionDetailsRequests.remove(key);
    }
    if (context.operation != operation) return;

    const QString detail = message.isEmpty() ? code : message;
    switch (context.kind)
    {
        case RequestKind::ExtensionList:
            status->setText(tr("Could not refresh extension state: %1").arg(detail));
            break;
        case RequestKind::ExtensionDetails:
            status->setText(tr("Could not load staged candidates and installed versions for %1: %2")
                            .arg(context.extensionId, detail));
            break;
        case RequestKind::CandidateDetails:
        case RequestKind::VersionDetails:
            clearDetails(tr("Extension details are unavailable: %1").arg(detail));
            break;
        case RequestKind::Capabilities:
            runtimeCapabilityKnown = false;
            trustEnabled = false;
            updateTrustStatus();
            break;
        case RequestKind::ScientificSchema:
        case RequestKind::ManifestSchema:
        case RequestKind::NumericSeriesSchema:
            status->setText(tr("Extension schema query failed: %1").arg(detail));
            break;
        case RequestKind::RuntimeVerifyStart:
        case RequestKind::RuntimeVerifyPoll:
            if (context.intent == runtimeVerificationGeneration && context.extensionId == observedWorkspace)
            {
                runtimeVerificationRequest.clear(); runtimeVerificationReady = false; runtimeVerificationTimer->stop();
                runtimeVerificationMessage = tr("Could not verify private scientific environment: %1 (%2)").arg(detail, code);
                clearPreview(runtimeVerificationMessage); updateTrustStatus();
            }
            break;
        case RequestKind::ScientificResult:
            if (context.intent == scientificGeneration) {
                scientificGeneration.clear(); status->setText(tr("Could not read scientific results: %1. Last complete display retained.").arg(detail));
            }
            break;
        case RequestKind::Jobs:
            status->setText(tr("Could not refresh reference job sources: %1").arg(detail));
            break;
        case RequestKind::NativeChannels:
            if (nativeChannelRequest == requestId)
            {
                nativeChannelRequest.clear(); nativeSourceKey.clear(); nativeSourceDescriptor = {};
                sourceSignal->clear();
                clearPreview(tr("Native source channels are unavailable. Refresh and create a new preview."));
                status->setText(tr("Could not read native FE channels: %1").arg(detail));
            }
            break;
        case RequestKind::JobPoll:
            status->setText(tr("Could not read job %1: %2. Refresh the workspace state before retrying.").arg(pendingJobId, detail));
            jobPollTimer->stop();
            break;
        case RequestKind::ProjectForPreview:
        case RequestKind::CommandPreview:
        case RequestKind::HostApproval:
        case RequestKind::CommandExecute:
            if (code == "RUNTIME_CHANGED" || code == "RUNTIME_NOT_VERIFIED"
                || code == "RUNTIME_VERIFICATION_REQUIRED" || code == "RUNTIME_VERIFICATION_IN_PROGRESS")
            {
                runtimeVerificationReady = false;
                runtimeVerificationTimer->stop();
                runtimeVerificationGeneration.clear(); runtimeVerificationId.clear(); runtimeVerificationRequest.clear();
                runtimeVerificationMessage = tr("Private scientific environment changed. Refresh to check it again before using v2 actions.");
                updateTrustStatus();
            }
            commandInFlight = false;
            clearPreview(tr("The extension action did not complete: %1").arg(detail));
            status->setText(tr("%1 (%2)").arg(detail, code));
            break;
    }
    updateAvailability();
}

void ExtensionWidget::renderExtensionList(const QJsonObject &result)
{
    const QString selectionKey = currentSelectionKey();
    QSet<QString> liveIds;
    for (const QJsonValue &extensionValue : result.value(QStringLiteral("extensions")).toArray())
    {
        const QJsonObject extension = extensionValue.toObject();
        const QString extensionId = extension.value(QStringLiteral("extensionId")).toString();
        if (!extensionId.isEmpty()) liveIds.insert(extensionId);
    }
    QStringList staleDetails;
    for (auto it = extensionDetails.cbegin(); it != extensionDetails.cend(); ++it)
    {
        const QJsonObject summary = [&result, &it]() {
            for (const QJsonValue &value : result.value(QStringLiteral("extensions")).toArray())
            {
                const QJsonObject candidate = value.toObject();
                if (candidate.value(QStringLiteral("extensionId")).toString() == it.key()) return candidate;
            }
            return QJsonObject{};
        }();
        if (!liveIds.contains(it.key()) || summary.isEmpty() ||
            it.value().value(QStringLiteral("registryRevision")) != summary.value(QStringLiteral("registryRevision")))
        {
            if (it.key() == selectedExtensionId &&
                (selectionKey.startsWith(QStringLiteral("candidate:")) || selectionKey.startsWith(QStringLiteral("version:"))))
            {
                deferredSelectionKey = selectionKey;
                deferredSelectionExtensionId = it.key();
            }
            staleDetails.append(it.key());
        }
    }
    for (const QString &extensionId : staleDetails)
    {
        extensionDetails.remove(extensionId);
        if (extensionId == selectedExtensionId)
        {
            refreshSelectedDetailsAfterList = true;
            if (!currentPreview.isEmpty()) clearPreview(tr("Extension registry changed. Create a new preview before applying an action."));
        }
    }
    for (auto it = extensionDetailsRequests.begin(); it != extensionDetailsRequests.end();)
    {
        if (!liveIds.contains(it.key().section(QLatin1Char('\n'), 0, 0))) it = extensionDetailsRequests.erase(it);
        else ++it;
    }

    rebuildInProgress = true;
    extensionTree->clear();
    const QJsonArray extensions = result.value(QStringLiteral("extensions")).toArray();
    for (const QJsonValue &extensionValue : extensions)
    {
        const QJsonObject extension = extensionValue.toObject();
        const QString extensionId = extension.value(QStringLiteral("extensionId")).toString();
        const QString activeHash = extension.value(QStringLiteral("activeBundleHash")).toString();
        const QString activeVersion = extension.value(QStringLiteral("activeVersion")).toString();
        const QJsonObject details = extensionDetails.value(extensionId);
        const bool detailsLoaded = !details.isEmpty() &&
            details.value(QStringLiteral("registryRevision")) == extension.value(QStringLiteral("registryRevision"));
        auto *root = new QTreeWidgetItem(extensionTree);
        root->setText(0, extensionId);
        const int candidateCount = extension.value(QStringLiteral("candidateCount")).toInt();
        const int versionCount = extension.value(QStringLiteral("versionCount")).toInt();
        const QString activeState = activeHash.isEmpty() ? tr("No active version") : tr("Active v%1").arg(activeVersion);
        root->setText(1, tr("%1 · %2 candidates · %3 versions")
                      .arg(activeState).arg(candidateCount).arg(versionCount));
        root->setText(2, activeHash.left(12));
        root->setData(0, KindRole, ExtensionSelection);
        root->setData(0, ExtensionIdRole, extensionId);
        root->setData(0, BundleHashRole, activeHash);
        root->setData(0, StateRole, activeHash.isEmpty() ? QStringLiteral("inactive") : QStringLiteral("active"));

        const QJsonArray candidates = detailsLoaded ? details.value(QStringLiteral("candidates")).toArray() : QJsonArray{};
        for (const QJsonValue &candidateValue : candidates)
        {
            const QJsonObject candidate = candidateValue.toObject();
            auto *item = new QTreeWidgetItem(root);
            item->setText(0, tr("Candidate v%1").arg(candidate.value(QStringLiteral("version")).toString()));
            item->setText(1, candidate.value(QStringLiteral("state")).toString());
            const QString bundleHash = candidate.value(QStringLiteral("bundleHash")).toString();
            item->setText(2, bundleHash.left(12));
            item->setData(0, KindRole, CandidateSelection);
            item->setData(0, ExtensionIdRole, extensionId);
            item->setData(0, CandidateIdRole, candidate.value(QStringLiteral("candidateId")).toString());
            item->setData(0, BundleHashRole, bundleHash);
            item->setData(0, VersionRole, candidate.value(QStringLiteral("version")).toString());
            item->setData(0, StateRole, candidate.value(QStringLiteral("state")).toString());
        }

        const QJsonArray versions = detailsLoaded ? details.value(QStringLiteral("versions")).toArray() : QJsonArray{};
        for (const QJsonValue &versionValue : versions)
        {
            const QJsonObject version = versionValue.toObject();
            auto *item = new QTreeWidgetItem(root);
            item->setText(0, tr("Version %1").arg(version.value(QStringLiteral("version")).toString()));
            item->setText(1, version.value(QStringLiteral("state")).toString());
            const QString bundleHash = version.value(QStringLiteral("bundleHash")).toString();
            item->setText(2, bundleHash.left(12));
            item->setData(0, KindRole, VersionSelection);
            item->setData(0, ExtensionIdRole, extensionId);
            item->setData(0, BundleHashRole, bundleHash);
            item->setData(0, VersionRole, version.value(QStringLiteral("version")).toString());
            item->setData(0, StateRole, version.value(QStringLiteral("state")).toString());
        }
        root->setExpanded(true);
    }
    extensionTree->resizeColumnToContents(0);

    const QString restoreKey = !deferredSelectionKey.isEmpty() &&
        deferredSelectionExtensionId == selectedExtensionId ? deferredSelectionKey : selectionKey;
    QTreeWidgetItem *restore = nullptr;
    QTreeWidgetItemIterator iterator(extensionTree);
    while (*iterator)
    {
        auto *item = *iterator;
        const int kind = item->data(0, KindRole).toInt();
        QString key;
        if (kind == ExtensionSelection)
            key = QStringLiteral("extension:") + item->data(0, ExtensionIdRole).toString();
        else if (kind == CandidateSelection)
            key = QStringLiteral("candidate:") + item->data(0, CandidateIdRole).toString();
        else if (kind == VersionSelection)
            key = QStringLiteral("version:") + item->data(0, ExtensionIdRole).toString() + ":" + item->data(0, BundleHashRole).toString();
        if (!restoreKey.isEmpty() && key == restoreKey) { restore = item; break; }
        ++iterator;
    }
    if (!restore && extensionTree->topLevelItemCount() > 0) restore = extensionTree->topLevelItem(0);
    if (restore)
    {
        {
            const QSignalBlocker blocker(extensionTree);
            extensionTree->setCurrentItem(restore);
        }
        rebuildInProgress = false;
        bool detailsChanged = refreshSelectedDetailsAfterList || selectionKey.isEmpty() || currentSelectionKey() != selectionKey;
        const int restoredKind = restore->data(0, KindRole).toInt();
        if (!detailsChanged && restoredKind == ExtensionSelection)
            detailsChanged = selectedBundleHash != restore->data(0, BundleHashRole).toString() ||
                !extensionDetails.contains(restore->data(0, ExtensionIdRole).toString());
        else if (!detailsChanged && restoredKind == CandidateSelection)
            detailsChanged = selectedCandidate.isEmpty() ||
                selectedCandidate.value(QStringLiteral("state")).toString() != restore->data(0, StateRole).toString();
        else if (!detailsChanged && restoredKind == VersionSelection)
            detailsChanged = selectedVersion.isEmpty() ||
                selectedVersion.value(QStringLiteral("state")).toString() != restore->data(0, StateRole).toString();
        if (detailsChanged)
        {
            if (!currentPreview.isEmpty()) clearPreview(tr("Extension state changed. Create a new preview before applying an action."));
            requestSelectedDetails(restore);
        }
        if (currentSelectionKey() == deferredSelectionKey)
        {
            deferredSelectionKey.clear();
            deferredSelectionExtensionId.clear();
        }
        else if (deferredSelectionExtensionId == restore->data(0, ExtensionIdRole).toString())
        {
            const QJsonObject loaded = extensionDetails.value(deferredSelectionExtensionId);
            const QJsonObject rootSummary = rootExtensionSummary(deferredSelectionExtensionId);
            const bool detailsCurrent = !loaded.isEmpty() && !rootSummary.isEmpty() &&
                loaded.value(QStringLiteral("registryRevision")) == rootSummary.value(QStringLiteral("registryRevision"));
            if (detailsCurrent)
            {
                const QJsonValue next = loaded.value(QStringLiteral("nextCandidateOffset"));
                const int total = loaded.value(QStringLiteral("candidateTotal")).toInt();
                const int loadedCount = loaded.value(QStringLiteral("candidates")).toArray().size();
                if (!next.isDouble() && loadedCount >= total)
                {
                    deferredSelectionKey.clear();
                    deferredSelectionExtensionId.clear();
                }
            }
        }
        refreshSelectedDetailsAfterList = false;
    }
    else
    {
        rebuildInProgress = false;
        selectedExtensionId.clear();
        selectedCandidateId.clear();
        selectedBundleHash.clear();
        clearDetails(tr("No staged candidates or installed extension versions are available."));
        refreshSelectedDetailsAfterList = false;
    }
    syncRollbackTargets();
    status->setText(tr("Extension state refreshed at capability revision %1.")
                    .arg(result.value(QStringLiteral("capabilityRevision")).toVariant().toString()));
    updateAvailability();
}

QJsonObject ExtensionWidget::extensionDetailsEntry(const QJsonObject &result) const
{
    const QJsonArray extensions = result.value(QStringLiteral("extensions")).toArray();
    if (extensions.size() == 1) return extensions.first().toObject();
    if (result.value(QStringLiteral("extension")).isObject()) return result.value(QStringLiteral("extension")).toObject();
    if (result.contains(QStringLiteral("extensionId"))) return result;
    return {};
}

QJsonObject ExtensionWidget::rootExtensionSummary(const QString &extensionId) const
{
    for (const QJsonValue &value : extensionListResult.value(QStringLiteral("extensions")).toArray())
    {
        const QJsonObject extension = value.toObject();
        if (extension.value(QStringLiteral("extensionId")).toString() == extensionId) return extension;
    }
    return {};
}

void ExtensionWidget::requestSelectedDetails(QTreeWidgetItem *item)
{
    selectedCandidate = {};
    selectedVersion = {};
    selectedCandidateId.clear();
    selectedBundleHash.clear();
    if (!item)
    {
        selectedExtensionId.clear();
        clearDetails(tr("Select an extension or candidate to inspect its service-provided details."));
        return;
    }

    const int kind = item->data(0, KindRole).toInt();
    selectedExtensionId = item->data(0, ExtensionIdRole).toString();
    const QJsonObject summary = rootExtensionSummary(selectedExtensionId);
    const QJsonObject cachedDetails = extensionDetails.value(selectedExtensionId);
    if (!summary.isEmpty() && (cachedDetails.isEmpty() ||
        cachedDetails.value(QStringLiteral("registryRevision")) != summary.value(QStringLiteral("registryRevision"))))
        requestExtensionDetails(selectedExtensionId, 0);
    if (kind == CandidateSelection)
    {
        selectedCandidateId = item->data(0, CandidateIdRole).toString();
        selectedBundleHash = item->data(0, BundleHashRole).toString();
        clearDetails(tr("Loading candidate manifest, source, diff, tests, and validation report from the workflow service…"));
        request(QStringLiteral("extension.candidate.get"),
                {{QStringLiteral("candidateId"), selectedCandidateId}},
                PendingRequest{RequestKind::CandidateDetails, selectedExtensionId,
                               selectedCandidateId, selectedBundleHash});
    }
    else if (kind == VersionSelection)
    {
        selectedBundleHash = item->data(0, BundleHashRole).toString();
        clearDetails(tr("Loading installed version manifest, source, and validation status from the workflow service…"));
        request(QStringLiteral("extension.version.get"),
                {{QStringLiteral("extensionId"), selectedExtensionId},
                 {QStringLiteral("bundleHash"), selectedBundleHash}},
                PendingRequest{RequestKind::VersionDetails, selectedExtensionId, {}, selectedBundleHash});
    }
    else if (kind == ExtensionSelection)
    {
        const QString activeHash = item->data(0, BundleHashRole).toString();
        if (activeHash.isEmpty())
        {
            clearDetails(tr("This extension has no active version. Select a staged candidate to inspect or validate it."));
        }
        else
        {
            selectedBundleHash = activeHash;
            clearDetails(tr("Loading active version manifest, source, and validation status from the workflow service…"));
            request(QStringLiteral("extension.version.get"),
                    {{QStringLiteral("extensionId"), selectedExtensionId},
                     {QStringLiteral("bundleHash"), selectedBundleHash}},
                    PendingRequest{RequestKind::VersionDetails, selectedExtensionId, {}, selectedBundleHash});
        }
    }
    syncRollbackTargets();
}

void ExtensionWidget::renderCandidate(const QJsonObject &candidate)
{
    if (candidate.value(QStringLiteral("candidateId")).toString() != selectedCandidateId) return;
    selectedCandidate = candidate;
    manifestView->setPlainText(jsonText(candidate.value(QStringLiteral("manifest")).toObject()));
    sourceView->setPlainText(candidate.value(QStringLiteral("source")).toString());
    const QJsonObject diff = candidate.value(QStringLiteral("sourceDiff")).toObject();
    if (diff.value(QStringLiteral("format")).toString() == QStringLiteral("unified-diff"))
        diffView->setPlainText(diff.value(QStringLiteral("text")).toString());
    else
        diffView->setPlainText(tr("The service did not provide a supported source diff."));
    testsView->setPlainText(candidate.value(QStringLiteral("tests")).toString());
    const QJsonValue report = candidate.value(QStringLiteral("report"));
    if (report.isUndefined() || report.isNull())
        reportView->setPlainText(jsonText(candidate.value(QStringLiteral("validation")).toObject()));
    else
        reportView->setPlainText(jsonValueText(report));
    renderFormAndMetrics(candidate.value(QStringLiteral("manifest")).toObject());
    status->setText(tr("Candidate %1 · %2 · bundle %3").arg(candidate.value(QStringLiteral("version")).toString(),
        candidate.value(QStringLiteral("state")).toString(), candidate.value(QStringLiteral("bundleHash")).toString().left(12)));
    updateAvailability();
}

void ExtensionWidget::renderVersion(const QJsonObject &version)
{
    if (version.value(QStringLiteral("extensionId")).toString() != selectedExtensionId ||
        version.value(QStringLiteral("bundleHash")).toString() != selectedBundleHash)
        return;
    const bool sameVersionManifest = !selectedVersion.isEmpty() &&
        selectedVersion.value(QStringLiteral("bundleHash")) == version.value(QStringLiteral("bundleHash")) &&
        selectedVersion.value(QStringLiteral("manifest")) == version.value(QStringLiteral("manifest"));
    selectedVersion = version;
    manifestView->setPlainText(jsonText(version.value(QStringLiteral("manifest")).toObject()));
    sourceView->setPlainText(version.value(QStringLiteral("source")).toString());
    diffView->setPlainText(tr("Installed versions are immutable. Select a staged candidate to inspect its service-computed diff against the declared base bundle."));
    testsView->setPlainText(tr("Installed tests are summarized by the validation report and its hashes."));
    reportView->setPlainText(jsonText(version.value(QStringLiteral("validation")).toObject()));
    // A background version-detail refresh may follow a successful feature run.
    // Keep its displayed metrics while the immutable version/schema is unchanged.
    if (!sameVersionManifest)
        renderFormAndMetrics(version.value(QStringLiteral("manifest")).toObject());
    status->setText(tr("%1 · version %2 · bundle %3 · report %4")
        .arg(version.value(QStringLiteral("state")).toString(), version.value(QStringLiteral("version")).toString(),
             version.value(QStringLiteral("bundleHash")).toString().left(12),
             version.value(QStringLiteral("reportSha256")).toString().left(12)));
    updateAvailability();
}

void ExtensionWidget::renderFormAndMetrics(const QJsonObject &manifest)
{
    const QString identity = observedWorkspace + ":" + selectedExtensionId + ":" + selectedBundleHash + ":" + jsonText(manifest);
    const bool sameForm = formIdentity == identity; formIdentity = identity;
    scientificFormSupported = manifest.value("sdkVersion") == QJsonValue("range.extension/v2") &&
        manifest.value("kind") == QJsonValue("multichannel-spectral-filter") &&
        scientificGui == supportedScientificGui() && manifest.value("gui").toObject() == scientificGui;
    for (QWidget *input : {static_cast<QWidget *>(timesInput), static_cast<QWidget *>(valuesInput), static_cast<QWidget *>(timeUnitInput), static_cast<QWidget *>(valueUnitInput)})
        input->setVisible(!scientificFormSupported);
    scientificForm->setVisible(scientificFormSupported); scienceView->setVisible(scientificFormSupported);
    sciencePlots->setVisible(scientificFormSupported); scienceProvenance->setVisible(scientificFormSupported);
    if (scientificFormSupported)
    {
        featureFormSupported = true;
        if (!sameForm)
        {
            scientificSources = {}; refreshScientificSources();
            windowInput->setCurrentIndex(0); removeMeanInput->setChecked(false); filterInput->setCurrentIndex(0);
            sourceMode->setCurrentIndex(sourceMode->findData("native-active"));
            scientificGeneration.clear(); displayedScientificResult = {}; scientificJobId.clear();
            scientificPoints = {}; scientificPageIdentity = {};
            pendingScienceRows = {}; scientificChannel = 0; scientificSelection.clear();
            scienceProvenance->clear();
            while (sciencePlots->count()) delete sciencePlots->widget(0);
            while (pendingSciencePlots->count()) delete pendingSciencePlots->widget(0);
            metricsTable->setRowCount(0);
        }
        metricsTable->setColumnCount(4);
        metricsTable->setHorizontalHeaderLabels({tr("Channel"), tr("Unit"), tr("Samples"), tr("Bins")});
        return;
    }
    metricsTable->setColumnCount(3); metricsTable->setHorizontalHeaderLabels({tr("Metric"), tr("Value"), tr("Unit")});
    featureFormSupported = false;
    const QJsonObject gui = manifest.value(QStringLiteral("gui")).toObject();
    const QJsonArray form = gui.value(QStringLiteral("form")).toArray();
    bool formSupported = false;
    if (form.size() == 1)
    {
        const QJsonObject field = form.first().toObject();
        formSupported = field.value(QStringLiteral("id")).toString() == QStringLiteral("series") &&
            field.value(QStringLiteral("type")).toString() == QStringLiteral("numeric-series") &&
            field.value(QStringLiteral("required")).toBool();
    }
    const QJsonArray columns = gui.value(QStringLiteral("table")).toObject().value(QStringLiteral("columns")).toArray();
    const QMap<QString, QString> expectedUnits{{QStringLiteral("rms"), QStringLiteral("valueUnit")},
                                               {QStringLiteral("peakAbs"), QStringLiteral("valueUnit")},
                                               {QStringLiteral("peakTime"), QStringLiteral("timeUnit")}};
    bool tableSupported = columns.size() == expectedUnits.size();
    QSet<QString> columnIds;
    for (const QJsonValue &columnValue : columns)
    {
        const QJsonObject column = columnValue.toObject();
        const QString id = column.value(QStringLiteral("id")).toString();
        tableSupported = tableSupported && expectedUnits.contains(id) &&
            column.value(QStringLiteral("unit")).toString() == expectedUnits.value(id) &&
            !column.value(QStringLiteral("label")).toString().isEmpty() && !columnIds.contains(id);
        columnIds.insert(id);
    }
    featureFormSupported = formSupported && tableSupported;
    if (!featureFormSupported)
        status->setText(tr("This manifest has no supported feature form or metric table. The GUI accepts only the declared numeric-series form and RMS/peak metrics."));

    metricsTable->setRowCount(0);
    if (tableSupported)
    for (qsizetype i = 0; i < columns.size(); ++i)
    {
        const QJsonObject column = columns.at(i).toObject();
        metricsTable->insertRow(int(i));
        metricsTable->setItem(int(i), 0, new QTableWidgetItem(column.value(QStringLiteral("label")).toString()));
        metricsTable->setItem(int(i), 1, new QTableWidgetItem(tr("Not calculated")));
        metricsTable->setItem(int(i), 2, new QTableWidgetItem(column.value(QStringLiteral("unit")).toString()));
    }
}

void ExtensionWidget::validateSelectedCandidate()
{
    const QJsonObject summary = currentCandidateSummary();
    if (summary.isEmpty()) return;
    beginCommandPreview(QStringLiteral("extension.validate"),
        {{QStringLiteral("candidateId"), summary.value(QStringLiteral("candidateId"))},
         {QStringLiteral("bundleHash"), summary.value(QStringLiteral("bundleHash"))}}, QStringLiteral("validate"));
}

void ExtensionWidget::activateSelectedCandidate()
{
    const QJsonObject summary = currentCandidateSummary();
    if (summary.isEmpty() || summary.value(QStringLiteral("state")).toString() != QStringLiteral("validated")) return;
    beginCommandPreview(QStringLiteral("extension.activate"),
        {{QStringLiteral("candidateId"), summary.value(QStringLiteral("candidateId"))},
         {QStringLiteral("bundleHash"), summary.value(QStringLiteral("bundleHash"))}}, QStringLiteral("activate"));
}

void ExtensionWidget::rollbackSelectedExtension()
{
    const QJsonObject extension = currentExtensionSummary();
    const QString targetHash = rollbackTarget->currentData().toString();
    const QString activeHash = extension.value(QStringLiteral("activeBundleHash")).toString();
    if (extension.isEmpty() || activeHash.isEmpty() || targetHash.isEmpty() || targetHash == activeHash) return;
    beginCommandPreview(QStringLiteral("extension.rollback"),
        {{QStringLiteral("extensionId"), selectedExtensionId},
         {QStringLiteral("expectedActiveBundleHash"), activeHash},
         {QStringLiteral("targetBundleHash"), targetHash}}, QStringLiteral("rollback"));
}

void ExtensionWidget::runSelectedExtension()
{
    if (selectedVersion.isEmpty() || selectedVersion.value(QStringLiteral("state")).toString() != QStringLiteral("active")) return;
    QJsonObject parameters;
    QString error;
    if (!buildRunParameters(&parameters, &error))
    {
        status->setText(error);
        return;
    }
    beginCommandPreview(QStringLiteral("extension.run"), parameters, QStringLiteral("run"));
}

void ExtensionWidget::beginCommandPreview(const QString &command, const QJsonObject &parameters,
                                          const QString &intent)
{
    if (!service || !service->isReady() || commandInFlight || !currentPreview.isEmpty()) return;
    commandInFlight = true;
    pendingCommand = command;
    pendingCommandParameters = parameters;
    pendingIntent = intent;
    request(QStringLiteral("project.query"), {},
            PendingRequest{RequestKind::ProjectForPreview, selectedExtensionId,
                           selectedCandidateId, selectedBundleHash, intent});
    status->setText(tr("Reading the current workflow revision before preview…"));
    updateAvailability();
}

void ExtensionWidget::approvePreview()
{
    if (!service || !service->isReady() || commandInFlight || currentPreview.isEmpty()) return;
    commandInFlight = true;
    const QString intent = pendingIntent;
    request(QStringLiteral("host.approve"),
            {{QStringLiteral("previewId"), currentPreview.value(QStringLiteral("previewId"))},
             {QStringLiteral("previewHash"), currentPreview.value(QStringLiteral("previewHash"))}},
            PendingRequest{RequestKind::HostApproval, selectedExtensionId,
                           selectedCandidateId, selectedBundleHash, intent});
    status->setText(tr("Recording your explicit approval…"));
    updateAvailability();
}

void ExtensionWidget::pollJob()
{
    if (pendingJobId.isEmpty() || !service || !service->isReady() || !jobsRequest.isEmpty()) return;
    const QString id = request(QStringLiteral("jobs.query"), {{QStringLiteral("jobId"), pendingJobId}},
                               PendingRequest{RequestKind::JobPoll, selectedExtensionId,
                                              selectedCandidateId, selectedBundleHash, pendingIntent});
    if (!id.isEmpty()) jobsRequest = id;
}

void ExtensionWidget::renderJob(const QJsonObject &job)
{
    if (job.value(QStringLiteral("jobId")).toString() != pendingJobId) return;
    const QString state = job.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("queued") || state == QStringLiteral("running") || state == QStringLiteral("cancelling"))
    {
        status->setText(tr("Extension job %1 is %2.").arg(pendingJobId, state));
        jobPollTimer->start();
        return;
    }

    jobPollTimer->stop();
    commandInFlight = false;
    if (state == QStringLiteral("succeeded"))
    {
        const QJsonObject result = job.value(QStringLiteral("result")).toObject();
        resultView->setPlainText(jsonText(result));
        if (result.value("sdkVersion") == QJsonValue("range.extension/v2") && pendingJobCommand == "extension.run")
        {
            displayedScientificResult = result; scientificJobId = pendingJobId;
            scientificSelection = currentSelectionKey(); loadScientificView();
        }
        else renderMetrics(result);
        status->setText(tr("Extension job %1 succeeded. The service-provided result and provenance are shown below.").arg(pendingJobId));
        if (pendingJobCommand == QStringLiteral("extension.validate") && result.contains(QStringLiteral("validation")))
            reportView->setPlainText(jsonText(result.value(QStringLiteral("validation")).toObject()));
        refreshExtensions();
        refreshJobs();
        if (!selectedCandidateId.isEmpty())
        {
            request(QStringLiteral("extension.candidate.get"), {{QStringLiteral("candidateId"), selectedCandidateId}},
                    PendingRequest{RequestKind::CandidateDetails, selectedExtensionId,
                                   selectedCandidateId, selectedBundleHash});
        }
        else if (!selectedExtensionId.isEmpty() && !selectedBundleHash.isEmpty())
        {
            request(QStringLiteral("extension.version.get"),
                    {{QStringLiteral("extensionId"), selectedExtensionId},
                     {QStringLiteral("bundleHash"), selectedBundleHash}},
                    PendingRequest{RequestKind::VersionDetails, selectedExtensionId, {}, selectedBundleHash});
        }
    }
    else
    {
        const QJsonObject error = job.value(QStringLiteral("error")).toObject();
        status->setText(tr("Extension job %1 %2: %3 (%4)").arg(pendingJobId, state,
            error.value(QStringLiteral("message")).toString(), error.value(QStringLiteral("code")).toString()));
        resultView->setPlainText(jsonText(error));
        refreshExtensions();
    }
    pendingJobId.clear();
    pendingJobCommand.clear();
    pendingCommand.clear();
    pendingCommandParameters = {};
    pendingIntent.clear();
    updateAvailability();
}

void ExtensionWidget::renderMetrics(const QJsonObject &result)
{
    const QJsonObject metrics = result.value(QStringLiteral("metrics")).toObject();
    const QJsonArray columns = selectedVersion.value(QStringLiteral("manifest")).toObject()
        .value(QStringLiteral("gui")).toObject().value(QStringLiteral("table")).toObject()
        .value(QStringLiteral("columns")).toArray();
    metricsTable->setRowCount(int(columns.size()));
    for (qsizetype row = 0; row < columns.size(); ++row)
    {
        const QJsonObject column = columns.at(row).toObject();
        const QString metricId = column.value(QStringLiteral("id")).toString();
        const QJsonValue value = metrics.value(metricId);
        QString unit;
        const QString unitRef = column.value(QStringLiteral("unit")).toString();
        if (unitRef == QStringLiteral("valueUnit")) unit = metrics.value(QStringLiteral("valueUnit")).toString();
        else if (unitRef == QStringLiteral("timeUnit")) unit = metrics.value(QStringLiteral("timeUnit")).toString();
        else unit = unitRef;
        QString displayValue = tr("Not calculated");
        if (value.isDouble() && std::isfinite(value.toDouble())) displayValue = QString::number(value.toDouble(), 'g', 12);
        metricsTable->setItem(int(row), 0, new QTableWidgetItem(column.value(QStringLiteral("label")).toString()));
        metricsTable->setItem(int(row), 1, new QTableWidgetItem(displayValue));
        metricsTable->setItem(int(row), 2, new QTableWidgetItem(unit));
    }
}

void ExtensionWidget::clearPreview(const QString &message)
{
    currentPreview = {};
    if (previewView) previewView->clear();
    if (!message.isEmpty() && status) status->setText(message);
    if (approveButton) approveButton->setEnabled(false);
}

void ExtensionWidget::clearDetails(const QString &message)
{
    selectedCandidate = {};
    selectedVersion = {};
    featureFormSupported = false;
    if (manifestView) manifestView->setPlainText(message);
    if (sourceView) sourceView->clear();
    if (diffView) diffView->clear();
    if (testsView) testsView->clear();
    if (reportView) reportView->clear();
    if (metricsTable && !scientificFormSupported) metricsTable->setRowCount(0);
    updateAvailability();
}

void ExtensionWidget::updateTrustStatus()
{
    if (!trustStatus) return;
    if (!runtimeCapabilityKnown)
    {
        trustStatus->setText(tr("Runtime extension trust was not reported. Validation and feature execution remain disabled."));
        return;
    }
    const QString trustMode = activeRuntimeCapability.value(QStringLiteral("trustMode")).toString();
    const QString protocol = activeRuntimeCapability.value(QStringLiteral("protocolVersion")).toString();
    const bool sandboxed = activeRuntimeCapability.value(QStringLiteral("osSandbox")).toBool();
    trustStatus->setText(tr("Runtime trust: %1 (%2). OS sandbox: %3. Extension protocol: %4.")
        .arg(trustEnabled ? tr("trusted developer mode") : tr("disabled"), trustMode,
               sandboxed ? tr("enabled") : tr("not enabled"), protocol));
    if (!runtimeVerificationMessage.isEmpty()) trustStatus->setText(trustStatus->text() + "\n" + runtimeVerificationMessage);
}

void ExtensionWidget::startRuntimeVerification()
{
    if (!service || !service->isReady() || !trustEnabled || scientificGui.isEmpty() ||
        !runtimeVerificationAvailable || !runtimeVerificationGeneration.isEmpty()) return;
    runtimeVerificationGeneration = QUuid::createUuid().toString(QUuid::WithoutBraces);
    runtimeVerificationClock.start(); runtimeVerificationReady = false;
    runtimeVerificationMessage = tr("Checking private scientific environment…");
    PendingRequest context{RequestKind::RuntimeVerifyStart, observedWorkspace};
    context.intent = runtimeVerificationGeneration;
    runtimeVerificationRequest = request("extension.runtime.verify", {{"sdkVersion", "range.extension/v2"}, {"force", true}}, context);
    updateTrustStatus();
}

void ExtensionWidget::pollRuntimeVerification()
{
    if (!service || !service->isReady() || runtimeVerificationId.isEmpty() || !runtimeVerificationRequest.isEmpty()) return;
    if (!runtimeVerificationReady && runtimeVerificationClock.elapsed() > 360000)
    {
        runtimeVerificationTimer->stop();
        runtimeVerificationMessage = tr("Private scientific environment verification exceeded its six-minute phase budget.");
        updateTrustStatus(); updateAvailability(); return;
    }
    PendingRequest context{RequestKind::RuntimeVerifyPoll, observedWorkspace};
    context.intent = runtimeVerificationGeneration;
    runtimeVerificationRequest = request("extension.runtime.verification.query", {{"verificationId", runtimeVerificationId}}, context);
}

void ExtensionWidget::syncRollbackTargets()
{
    const QJsonObject extension = currentExtensionSummary();
    const QString previous = rollbackTarget->currentData().toString();
    const QSignalBlocker blocker(rollbackTarget);
    rollbackTarget->clear();
    for (const QJsonValue &versionValue : extension.value(QStringLiteral("versions")).toArray())
    {
        const QJsonObject version = versionValue.toObject();
        if (version.value(QStringLiteral("state")).toString() != QStringLiteral("retained")) continue;
        const QString hash = version.value(QStringLiteral("bundleHash")).toString();
        const QString versionName = version.value(QStringLiteral("version")).toString();
        rollbackTarget->addItem(tr("Restore v%1 · %2").arg(versionName, hash.left(12)), hash);
        if (hash == previous) rollbackTarget->setCurrentIndex(rollbackTarget->count() - 1);
    }
}

QString ExtensionWidget::currentSelectionKey() const
{
    const QTreeWidgetItem *item = extensionTree->currentItem();
    if (!item) return {};
    const int kind = item->data(0, KindRole).toInt();
    if (kind == ExtensionSelection)
        return QStringLiteral("extension:") + item->data(0, ExtensionIdRole).toString();
    if (kind == CandidateSelection)
        return QStringLiteral("candidate:") + item->data(0, CandidateIdRole).toString();
    if (kind == VersionSelection)
        return QStringLiteral("version:") + item->data(0, ExtensionIdRole).toString() + ":" + item->data(0, BundleHashRole).toString();
    return {};
}

QJsonObject ExtensionWidget::currentExtensionSummary() const
{
    if (selectedExtensionId.isEmpty()) return {};
    const QJsonObject summary = rootExtensionSummary(selectedExtensionId);
    const QJsonObject details = extensionDetails.value(selectedExtensionId);
    if (!details.isEmpty() && !summary.isEmpty() &&
        details.value(QStringLiteral("registryRevision")) == summary.value(QStringLiteral("registryRevision")))
        return details;
    return summary;
}

QJsonObject ExtensionWidget::currentCandidateSummary() const
{
    if (selectedCandidateId.isEmpty()) return {};
    const QJsonObject extension = currentExtensionSummary();
    for (const QJsonValue &value : extension.value(QStringLiteral("candidates")).toArray())
    {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("candidateId")).toString() == selectedCandidateId) return candidate;
    }
    return {};
}

bool ExtensionWidget::selectedCandidateValidated() const
{
    return currentCandidateSummary().value(QStringLiteral("state")).toString() == QStringLiteral("validated");
}

bool ExtensionWidget::buildRunParameters(QJsonObject *parameters, QString *error) const
{
    if (!parameters)
    {
        if (error) *error = tr("Run parameter output is unavailable.");
        return false;
    }
    const QString activeHash = currentExtensionSummary().value(QStringLiteral("activeBundleHash")).toString();
    if (selectedExtensionId.isEmpty() || activeHash.isEmpty() || !selectedVersionActive())
    {
        if (error) *error = tr("Select the active extension version before running it.");
        return false;
    }
    *parameters = {{QStringLiteral("extensionId"), selectedExtensionId},
                   {QStringLiteral("bundleHash"), activeHash}};
    if (scientificFormSupported)
    {
        if (scientificSources.isEmpty() || scientificSources.size() > 4)
        { if (error) *error = tr("Add one to four verified native channels."); return false; }
        QJsonObject filter{{"kind", filterInput->currentText()}};
        if (filterInput->currentText() == "butterworth-lowpass")
            filter = {{"kind", "butterworth-lowpass"}, {"order", 1}, {"cutoffHz", cutoffInput->value()}, {"phase", "causal"}, {"initialCondition", "zero"}};
        parameters->insert("sources", scientificSources);
        parameters->insert("options", QJsonObject{{"window", windowInput->currentText()}, {"removeMean", removeMeanInput->isChecked()}, {"filter", filter}});
        return true;
    }
    if (sourceMode->currentData().toString() == QStringLiteral("series"))
    {
        QJsonObject series;
        if (!parseNumericSeries(timesInput->toPlainText(), valuesInput->toPlainText(),
                                timeUnitInput->currentText().trimmed(), valueUnitInput->currentText().trimmed(),
                                &series, error))
            return false;
        parameters->insert(QStringLiteral("series"), series);
    }
    else if (sourceMode->currentData().toString().startsWith("native-"))
    {
        const QString jobId = sourceJob->currentData().toString();
        const QString channel = sourceSignal->currentData().toString();
        if (jobId.isEmpty() || channel.isEmpty() || !nativeChannelRequest.isEmpty()
            || nativeSourceDescriptor.value("artifactId").toString().isEmpty())
        {
            if (error) *error = tr("Choose an available native FE job and channel after its catalog finishes loading.");
            return false;
        }
        parameters->insert("provenance", QJsonObject{{"jobId", jobId},
            {"mode", sourceMode->currentData().toString().mid(7)},
            {"artifactId", nativeSourceDescriptor.value("artifactId")}, {"channelId", channel}});
    }
    else
    {
        const QString jobId = sourceJob->currentData().toString();
        const QString signal = sourceSignal->currentData().toString();
        if (jobId.isEmpty() || signal.isEmpty())
        {
            if (error) *error = tr("Choose a successful reference workflow job and an allowlisted signal.");
            return false;
        }
        parameters->insert(QStringLiteral("provenance"), QJsonObject{
            {QStringLiteral("jobId"), jobId}, {QStringLiteral("signal"), signal}});
    }
    return true;
}

void ExtensionWidget::refreshSourceSignals()
{
    const QString sourceModeId = sourceMode->currentData().toString();
    const bool native = sourceModeId.startsWith("native-");
    const QString mode = native ? sourceModeId.mid(7) : QString();
    const QString selectedJob = sourceJob->currentData().toString();
    QJsonObject descriptor;
    if (native)
        for (const auto &value : availableJobs)
        {
            const auto job = value.toObject();
            if (job.value("jobId").toString() == selectedJob)
                descriptor = job.value("result").toObject().value("nativeResults").toObject().value(mode).toObject();
        }
    const QString key = sourceModeId + ":" + selectedJob + ":" + descriptor.value("artifactId").toString();
    if (key == nativeSourceKey) return;
    if (!nativeSourceKey.isEmpty() && !currentPreview.isEmpty()
        && currentPreview.value("normalized").toObject().value("command") == QJsonValue("extension.run"))
        clearPreview(tr("Result source changed. Create a new preview before running the extension."));
    const QString previous = sourceSignal->currentData().toString();
    nativeSourceKey = key;
    nativeSourceDescriptor = descriptor;
    nativeChannelRequest.clear();
    const QSignalBlocker blocker(sourceSignal);
    sourceSignal->clear();
    if (!native)
    {
        sourceSignal->addItem(tr("Active sensor velocity (m/s)"), "reference.active.sensorVelocity");
        sourceSignal->addItem(tr("Active energy (J)"), "reference.active.energyJ");
        sourceSignal->addItem(tr("Passive energy (J)"), "reference.passive.energyJ");
        const int index = sourceSignal->findData(previous);
        if (index >= 0) sourceSignal->setCurrentIndex(index);
        return;
    }
    desiredNativeChannel = previous.startsWith("node:") ? previous : QString();
    if (!service || !service->isReady() || descriptor.isEmpty()) return;
    PendingRequest context{RequestKind::NativeChannels};
    context.intent = key;
    nativeChannelRequest = request("result.channels.query", {{"jobId", selectedJob}, {"mode", mode},
        {"artifactId", descriptor.value("artifactId")}, {"offset", 0}, {"limit", 200}}, context);
}

bool ExtensionWidget::selectedVersionActive() const
{
    return !selectedVersion.isEmpty() && selectedVersion.value(QStringLiteral("state")).toString() == QStringLiteral("active") &&
        selectedVersion.value(QStringLiteral("bundleHash")).toString() == currentExtensionSummary().value(QStringLiteral("activeBundleHash")).toString();
}

void ExtensionWidget::updateAvailability()
{
    const bool ready = service && service->isReady();
    const int kind = extensionTree && extensionTree->currentItem() ? extensionTree->currentItem()->data(0, KindRole).toInt() : 0;
    const QJsonObject candidate = currentCandidateSummary();
    const QJsonObject extension = currentExtensionSummary();
    const bool idle = ready && !commandInFlight && pendingJobId.isEmpty();
    const bool schemaReady = !manifestSchema.isEmpty() && !numericSeriesSchema.isEmpty();
    if (extensionTree) extensionTree->setEnabled(ready && !commandInFlight);
    if (refreshButton) refreshButton->setEnabled(ready);
    if (loadMoreButton)
    {
        const QJsonValue nextOffset = extension.value(QStringLiteral("nextCandidateOffset"));
        bool detailRequestInFlight = false;
        for (auto it = extensionDetailsRequests.cbegin(); it != extensionDetailsRequests.cend(); ++it)
            if (it.key().startsWith(selectedExtensionId + QLatin1Char('\n'))) { detailRequestInFlight = true; break; }
        loadMoreButton->setEnabled(idle && !selectedExtensionId.isEmpty() && nextOffset.isDouble() && !detailRequestInFlight);
    }
    const bool scientificCandidate = selectedCandidate.value("manifest").toObject().value("sdkVersion") == QJsonValue("range.extension/v2");
    if (validateButton) validateButton->setEnabled(idle && kind == CandidateSelection && trustEnabled && runtimeCapabilityKnown && schemaReady && !candidate.isEmpty() && (!scientificCandidate || runtimeVerificationReady));
    if (activateButton) activateButton->setEnabled(idle && kind == CandidateSelection && selectedCandidateValidated() &&
        candidate.value(QStringLiteral("bundleHash")).toString() != extension.value(QStringLiteral("activeBundleHash")).toString());
    if (rollbackTarget) rollbackTarget->setEnabled(idle && !extension.value(QStringLiteral("activeBundleHash")).toString().isEmpty() && rollbackTarget->count() > 0);
    if (rollbackButton) rollbackButton->setEnabled(idle && !extension.value(QStringLiteral("activeBundleHash")).toString().isEmpty() &&
        !rollbackTarget->currentData().toString().isEmpty() && rollbackTarget->currentData().toString() != extension.value(QStringLiteral("activeBundleHash")).toString());
    const bool scientificRuntimeReady = !scientificFormSupported ||
        (runtimeVerificationReady && activeRuntimeCapability.value("supportedProtocols").toArray().contains("range.extension/v2") &&
         activeRuntimeCapability.value("scientificV2").toObject().value("maximumChannels").toInt() == 4);
    const bool runEnabled = scientificRuntimeReady && idle && trustEnabled && runtimeCapabilityKnown && schemaReady && featureFormSupported && selectedVersionActive();
    if (runButton) runButton->setEnabled(runEnabled);
    if (sourceMode) sourceMode->setEnabled(runEnabled && !commandInFlight);
    scientificForm->setEnabled(runEnabled);
    addSourceButton->setEnabled(runEnabled && scientificSources.size() < 4 && sourceMode->currentData().toString().startsWith("native-") &&
        !sourceSignal->currentData().toString().isEmpty() && nativeChannelRequest.isEmpty() && !nativeSourceDescriptor.value("artifactId").toString().isEmpty());
    cutoffInput->setEnabled(runEnabled && filterInput->currentText() == "butterworth-lowpass");
    const bool manual = sourceMode && sourceMode->currentData().toString() == QStringLiteral("series");
    if (timesInput) timesInput->setEnabled(runEnabled && manual);
    if (valuesInput) valuesInput->setEnabled(runEnabled && manual);
    if (timeUnitInput) timeUnitInput->setEnabled(runEnabled && manual);
    if (valueUnitInput) valueUnitInput->setEnabled(runEnabled && manual);
    const bool provenance = runEnabled && !manual;
    if (sourceJob) sourceJob->setEnabled(provenance && sourceJob->count() > 0);
    if (sourceSignal) sourceSignal->setEnabled(provenance);
    if (approveButton) approveButton->setEnabled(ready && !commandInFlight && !currentPreview.isEmpty());
}


void ExtensionWidget::addScientificSource()
{
    if (!scientificFormSupported || scientificSources.size() >= 4 || !nativeChannelRequest.isEmpty() ||
        !sourceMode->currentData().toString().startsWith("native-") || nativeSourceDescriptor.value("artifactId").toString().isEmpty() ||
        sourceSignal->currentData().toString().isEmpty() || sourceJob->currentData().toString().isEmpty()) return;
    const QJsonObject source{{"kind", "native"}, {"jobId", sourceJob->currentData().toString()},
        {"mode", sourceMode->currentData().toString().mid(7)}, {"artifactId", nativeSourceDescriptor.value("artifactId")},
        {"channelId", sourceSignal->currentData().toString()}};
    for (const auto &value : scientificSources) if (value.toObject() == source) return;
    scientificSources.append(source); refreshScientificSources();
    clearPreview(tr("Sources changed. Create a new preview.")); updateAvailability();
}

void ExtensionWidget::resetScientificWorkspace()
{
    scientificGeneration.clear(); scientificSelection.clear(); scientificJobId.clear();
    scientificSources = {}; displayedScientificResult = {}; scientificPoints = {}; scientificPageIdentity = {};
    pendingScienceRows = {}; scientificChannel = 0; formIdentity.clear(); scientificFormSupported = false;
    while (sciencePlots->count()) delete sciencePlots->widget(0);
    while (pendingSciencePlots->count()) delete pendingSciencePlots->widget(0);
    refreshScientificSources(); metricsTable->setRowCount(0); scienceProvenance->clear();
    scientificForm->hide(); scienceView->hide(); sciencePlots->hide(); scienceProvenance->hide();
}

void ExtensionWidget::refreshScientificSources()
{
    sourcesTable->setRowCount(int(scientificSources.size()));
    const QStringList keys{"jobId", "mode", "artifactId", "channelId"};
    for (int row = 0; row < scientificSources.size(); ++row)
        for (int column = 0; column < keys.size(); ++column)
            sourcesTable->setItem(row, column, new QTableWidgetItem(scientificSources[row].toObject().value(keys[column]).toString()));
}

void ExtensionWidget::loadScientificView()
{
    if (scientificJobId.isEmpty() || displayedScientificResult.isEmpty() || scientificSelection != currentSelectionKey()) return;
    scientificGeneration = QUuid::createUuid().toString(QUuid::WithoutBraces);
    scientificChannel = 0; scientificPoints = {}; scientificPageIdentity = {};
    while (pendingSciencePlots->count()) delete pendingSciencePlots->widget(0);
    pendingScienceRows = {};
    // Keep the last complete view until its replacement has passed validation.
    requestScientificPage(0);
}

void ExtensionWidget::requestScientificPage(int offset)
{
    PendingRequest context{RequestKind::ScientificResult, scientificJobId, {}, selectedBundleHash, scientificGeneration};
    request("extension.result.query", {{"jobId", scientificJobId}, {"channel", scientificChannel},
        {"offset", offset}, {"limit", 1000}, {"view", scienceView->currentText()}}, context);
}

void ExtensionWidget::consumeScientificPage(const QJsonObject &page)
{
    auto reject = [this] {
        scientificGeneration.clear(); scientificPoints = {};
        status->setText(tr("Scientific result page is inconsistent or exceeds the 10,000 point limit; the last complete display is retained."));
    };
    if (page.value("jobId").toString() != scientificJobId || page.value("bundleHash").toString() != selectedBundleHash ||
        page.value("view").toString() != scienceView->currentText() || page.value("offset").toInt(-1) != scientificPoints.size() ||
        page.value("pointCount").toInt(-1) < 1 || page.value("pointCount").toInt() > 10000 ||
        page.value("channelCount").toInt() < 1 || page.value("channelCount").toInt() > 4 ||
        page.value("resultSha256").toString().isEmpty() || page.value("runtimeHash").toString().isEmpty() ||
        page.value("sourceSha256").toString().isEmpty()) { reject(); return; }
    const QJsonObject artifact = displayedScientificResult.value("scientificResult").toObject();
    if (!artifact.isEmpty() && (page.value("resultSha256") != artifact.value("sha256") ||
        page.value("runtimeHash") != displayedScientificResult.value("runtimeHash") ||
        page.value("sourceSha256") != displayedScientificResult.value("seriesSha256") ||
        page.value("verificationProfile") != displayedScientificResult.value("verificationProfile") ||
        page.value("sampleCount") != artifact.value("sampleCount") || page.value("binCount") != artifact.value("binCount") ||
        scientificChannel >= artifact.value("channels").toArray().size() ||
        page.value("channel") != artifact.value("channels").toArray().at(scientificChannel))) { reject(); return; }
    QJsonObject identity = page;
    for (const QString &key : {QString("offset"), QString("nextOffset"), QString("points"), QString("frequenciesHz"), QString("fft"), QString("amplitude"), QString("psd")}) identity.remove(key);
    if (!scientificPageIdentity.isEmpty() && identity != scientificPageIdentity) { reject(); return; }
    scientificPageIdentity = identity;
    const auto points = page.value("points").toArray();
    if (points.isEmpty() || points.size() > 1000 || scientificPoints.size() + points.size() > page.value("pointCount").toInt()) { reject(); return; }
    for (const auto &value : points)
    {
        const auto point = value.toObject();
        if (!point.value("x").isDouble() || !point.value("y").isDouble() || !std::isfinite(point.value("x").toDouble()) ||
            !std::isfinite(point.value("y").toDouble()) || point.value("rawOrdinal").toDouble(-1) != scientificPoints.size() ||
            (!scientificPoints.isEmpty() && point.value("x").toDouble() <= scientificPoints.last().toObject().value("x").toDouble())) { reject(); return; }
        scientificPoints.append(point);
    }
    if (!page.value("nextOffset").isNull())
    {
        if (!page.value("nextOffset").isDouble() || page.value("nextOffset").toInt(-1) != scientificPoints.size() || scientificPoints.size() >= page.value("pointCount").toInt()) { reject(); return; }
        requestScientificPage(int(scientificPoints.size())); return;
    }
    if (scientificPoints.size() != page.value("pointCount").toInt()) { reject(); return; }
    SeriesPlotWidget::Dataset dataset;
    dataset.viewKind = page.value("domain") == QJsonValue("time") ? SeriesPlotWidget::ViewKind::Time : SeriesPlotWidget::ViewKind::Spectrum;
    dataset.title = scienceView->currentText(); dataset.xLabel = page.value("xLabel").toString(); dataset.xUnit = page.value("xUnit").toString();
    dataset.yLabel = page.value("yLabel").toString(); dataset.yUnit = page.value("yUnit").toString();
    SeriesPlotWidget::Trace trace;
    trace.id = page.value("channel").toObject().value("id").toString();
    if (trace.id.isEmpty()) trace.id = QString::number(scientificChannel);
    trace.label = trace.id; trace.unit = dataset.yUnit; trace.sourceId = scientificJobId;
    const QJsonArray originalSources = page.value("inputSources").toArray();
    if (scientificChannel < originalSources.size())
    {
        const auto origin = originalSources.at(scientificChannel).toObject();
        const auto selector = origin.value("selector").toObject();
        trace.label = tr("%1/%2 · %3").arg(selector.value("jobId").toString().left(8),
            selector.value("mode").toString(), selector.value("channelId").toString());
    }
    trace.artifactSha256 = page.value("resultSha256").toString();
    // Display only: each contiguous bucket keeps its extrema in original order.
    QVector<int> ordinals;
    if (scientificPoints.size() <= 1000) for (int i = 0; i < scientificPoints.size(); ++i) ordinals.append(i);
    else
    {
        ordinals.append(0);
        const int size = int(scientificPoints.size());
        for (int bucket = 0; bucket < 499; ++bucket)
        {
            const int begin = 1 + (size - 2) * bucket / 499, end = 1 + (size - 2) * (bucket + 1) / 499;
            int minimum = begin, maximum = begin;
            for (int i = begin + 1; i < end; ++i)
            {
                const double y = scientificPoints[i].toObject().value("y").toDouble();
                if (y < scientificPoints[minimum].toObject().value("y").toDouble()) minimum = i;
                if (y > scientificPoints[maximum].toObject().value("y").toDouble()) maximum = i;
            }
            ordinals.append(std::min(minimum, maximum));
            if (minimum != maximum) ordinals.append(std::max(minimum, maximum));
        }
        ordinals.append(size - 1);
    }
    for (int ordinal : ordinals)
    {
        const auto point = scientificPoints[ordinal].toObject();
        trace.points.append({point.value("x").toDouble(), point.value("y").toDouble()});
        trace.originalOrdinals.append(quint64(point.value("rawOrdinal").toDouble()));
    }
    dataset.traces.append(trace);
    auto *plot = new SeriesPlotWidget(sciencePlots); QString error;
    if (!plot->setDataset(dataset, &error)) { delete plot; reject(); return; }
    pendingSciencePlots->addTab(plot, trace.label + " [" + trace.unit + "]");
    pendingScienceRows.append(QJsonArray{trace.label, page.value("channel").toObject().value("unit").toString(),
        QString::number(page.value("sampleCount").toInt()), QString::number(page.value("binCount").toInt())});
    const int channelCount = page.value("channelCount").toInt();
    ++scientificChannel;
    if (scientificChannel < channelCount)
    {
        scientificPoints = {}; scientificPageIdentity = {}; requestScientificPage(0); return;
    }
    while (sciencePlots->count()) delete sciencePlots->widget(0);
    while (pendingSciencePlots->count())
    {
        QWidget *complete = pendingSciencePlots->widget(0); const QString label = pendingSciencePlots->tabText(0);
        pendingSciencePlots->removeTab(0); sciencePlots->addTab(complete, label);
    }
    metricsTable->setRowCount(channelCount);
    for (int row = 0; row < pendingScienceRows.size(); ++row)
        for (int column = 0; column < 4; ++column)
            metricsTable->setItem(row, column, new QTableWidgetItem(pendingScienceRows[row].toArray()[column].toString()));
    scienceProvenance->setText(tr("Job: %1\nBundle: %2\nRuntime: %3\nSource: %4\nResult: %5\nVerification profile: %6\nHost verification covers the frozen numerical contract and accepted native frames. It does not establish arbitrary code isolation or physical model accuracy. NumPy 2.2.6 / SciPy 1.15.3; ordinary generated execution is disabled, OS sandbox is not enabled. Display reduction only; all %7 points were read before reduction.")
        .arg(scientificJobId, selectedBundleHash, page.value("runtimeHash").toString(), page.value("sourceSha256").toString(),
             page.value("resultSha256").toString(), jsonValueText(page.value("verificationProfile")), QString::number(scientificPoints.size())));
    scienceProvenance->setText(scienceProvenance->text() + tr("\nApplied input options: %1")
        .arg(jsonValueText(page.value("inputOptions"))));
    scientificPoints = {}; scientificPageIdentity = {};
}
