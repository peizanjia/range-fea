#include "mesh_command_widget.h"
#include "mesh_job_controller.h"
#include "mesh_binding_widget.h"
#include "session.h"
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

MeshCommandWidget::MeshCommandWidget(QWidget *parent) : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    auto *pageScroll = new QScrollArea(this);
    pageScroll->setObjectName("meshCommandScroll");
    pageScroll->setWidgetResizable(true);
    auto *content = new QWidget(pageScroll);
    pageScroll->setWidget(content);
    outer->addWidget(pageScroll);
    auto *layout = new QVBoxLayout(content);
    auto *scope = new QLabel(tr("Closed, connected, convex outward TRI1 shell; one uniform material. "
        "Coordinates: m. Maximum tetrahedron volume: m^3. The source stays unchanged. "
        "Acceptance creates a new document with fresh mesh IDs and no solver task. "
        "Explicit static rebinding is required before FE."),this);
    scope->setWordWrap(true); layout->addWidget(scope);
    source = new QLabel(tr("Select one available project document; resolve numeric drafts first."),this);
    source->setObjectName("meshSource"); source->setWordWrap(true);
    source->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(source);
    auto *form = new QFormLayout;
    maxVolume = new QLineEdit("0.08",this); maxVolume->setObjectName("meshMaxVolume");
    radiusEdgeRatio = new QLineEdit("1.6",this); radiusEdgeRatio->setObjectName("meshRadiusEdgeRatio");
    form->addRow(tr("Maximum tetrahedron volume (m^3), 1e-15 to 1e18"),maxVolume);
    form->addRow(tr("Radius / shortest edge, 1.2 to 2.0"),radiusEdgeRatio);
    layout->addLayout(form);
    auto *generation = new QHBoxLayout;
    generatePreview = new QPushButton(tr("Preview generation"),this); generatePreview->setObjectName("meshPreviewGenerate");
    generateApprove = new QPushButton(tr("Approve and generate this preview"),this); generateApprove->setObjectName("meshApproveGenerate");
    generation->addWidget(generatePreview); generation->addWidget(generateApprove); layout->addLayout(generation);
    jobList = new QTableWidget(0,5,this); jobList->setObjectName("meshJobs");
    jobList->setHorizontalHeaderLabels({tr("Attempt / job ID"),tr("State"),tr("Phase"),tr("Source"),tr("Source status")});
    jobList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobList->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobList->setSelectionMode(QAbstractItemView::SingleSelection);
    jobList->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(jobList);
    auto *acceptance = new QHBoxLayout;
    acceptPreview = new QPushButton(tr("Preview selected candidate acceptance"),this); acceptPreview->setObjectName("meshPreviewAccept");
    acceptApprove = new QPushButton(tr("Approve acceptance into a new document"),this); acceptApprove->setObjectName("meshApproveAccept");
    cancelButton = new QPushButton(tr("Cancel selected job"),this); cancelButton->setObjectName("meshCancel");
    acceptance->addWidget(acceptPreview); acceptance->addWidget(acceptApprove); acceptance->addWidget(cancelButton); layout->addLayout(acceptance);
    status = new QLabel(this); status->setObjectName("meshStatus"); status->setWordWrap(true); layout->addWidget(status);
    details = new QPlainTextEdit(this); details->setObjectName("meshDetails"); details->setReadOnly(true);
    details->setPlaceholderText(tr("Read-only source identity, candidate identity, quality report, and binding status."));
    layout->addWidget(details);
    binding = new MeshBindingWidget(this);
    auto *bindingScroll = new QScrollArea(this);
    bindingScroll->setObjectName("meshBindingScroll");
    bindingScroll->setWidgetResizable(true);
    bindingScroll->setMinimumHeight(220);
    bindingScroll->setMaximumHeight(400);
    bindingScroll->setWidget(binding);
    layout->addWidget(bindingScroll);
    connect(generatePreview,&QPushButton::clicked,this,&MeshCommandWidget::previewGeneration);
    connect(generateApprove,&QPushButton::clicked,this,&MeshCommandWidget::approveGeneration);
    connect(acceptPreview,&QPushButton::clicked,this,&MeshCommandWidget::previewAcceptance);
    connect(acceptApprove,&QPushButton::clicked,this,&MeshCommandWidget::approveAcceptance);
    connect(cancelButton,&QPushButton::clicked,this,&MeshCommandWidget::cancelSelected);
    for (auto *field : {maxVolume,radiusEdgeRatio})
        connect(field,&QLineEdit::textChanged,this,[this] {
            clearApprovals(); status->setText(tr("Options changed. Preview again before approving."));
        });
    connect(jobList,&QTableWidget::itemSelectionChanged,this,[this] {
        acceptancePreview = {}; acceptApprove->setEnabled(false);
        if (jobController && !selectedAttempt().isEmpty()) showResponse(jobController->query(selectedAttempt()));
        refreshContext();
    });
    auto *timer = new QTimer(this); timer->setInterval(1000);
    connect(timer,&QTimer::timeout,this,&MeshCommandWidget::refreshContext); timer->start();
    generatePreview->setEnabled(false); generateApprove->setEnabled(false);
    acceptPreview->setEnabled(false); acceptApprove->setEnabled(false); cancelButton->setEnabled(false);
}

void MeshCommandWidget::setSession(Session *value)
{
    if (session == value) return;
    if (session) disconnect(session,nullptr,this,nullptr);
    session = value; clearApprovals();
    binding->setSession(value);
    if (session)
    {
        const auto changed = [this] { clearApprovals(); refreshContext(); };
        connect(session,&Session::modelSelectionChanged,this,changed);
        connect(session,&Session::documentRevisionChanged,this,changed);
        connect(session,&Session::runtimeDocumentRemoved,this,changed);
        connect(session,&Session::modelAdded,this,[this] { refreshContext(); });
        connect(session,&QObject::destroyed,this,changed);
    }
    refreshContext();
}

void MeshCommandWidget::setController(MeshJobController *value)
{
    if (jobController == value) return;
    if (jobController) disconnect(jobController,nullptr,this,nullptr);
    jobController = value; clearApprovals();
    if (jobController)
    {
        connect(jobController,&MeshJobController::jobsChanged,this,&MeshCommandWidget::refreshContext);
        connect(jobController,&MeshJobController::previewsInvalidated,this,[this] {
            generationPreview = {}; acceptancePreview = {};
            generateApprove->setEnabled(false); acceptApprove->setEnabled(false);
        });
        connect(jobController,&QObject::destroyed,this,[this] { clearApprovals(); refreshContext(); });
    }
    refreshContext();
}
MeshJobController *MeshCommandWidget::controller() const { return jobController; }

void MeshCommandWidget::clearApprovals()
{
    generationPreview = {}; acceptancePreview = {};
    generateApprove->setEnabled(false); acceptApprove->setEnabled(false);
    if (jobController) jobController->invalidatePreviews();
}

QString MeshCommandWidget::selectedAttempt() const
{
    const int row = jobList->currentRow();
    return row >= 0 && jobList->item(row,0) ? jobList->item(row,0)->text() : QString();
}
void MeshCommandWidget::showResponse(const QJsonObject &response)
{
    status->setText(response.value("code").toString()+": "+response.value("message").toString());
    details->setPlainText(QString::fromUtf8(QJsonDocument(response.value("result").toObject()).toJson(QJsonDocument::Indented)));
}

void MeshCommandWidget::previewGeneration()
{
    clearApprovals();
    if (!session || !jobController || session->getSelectedModelIDs().size() != 1) return;
    bool volumeOk = false, ratioOk = false;
    const double volume = maxVolume->text().trimmed().toDouble(&volumeOk);
    const double ratio = radiusEdgeRatio->text().trimmed().toDouble(&ratioOk);
    if (!volumeOk || !ratioOk || !std::isfinite(volume) || !std::isfinite(ratio)
        || volume < 1e-15 || volume > 1e18 || ratio < 1.2 || ratio > 2.0)
    { status->setText(tr("Enter finite volume and radius/edge values within the stated bounds.")); return; }
    const auto response = jobController->previewGenerate(session->getSelectedModelIDs().first(),
        {{"unit","m"},{"volumeUnit","m^3"},{"maxVolume",volume},{"radiusEdgeRatio",ratio}});
    showResponse(response);
    if (response.value("valid").toBool())
    { generationPreview = response.value("result").toObject(); generateApprove->setEnabled(true); }
}
void MeshCommandWidget::approveGeneration()
{
    if (!jobController || generationPreview.isEmpty() || !generateApprove->isEnabled()) return;
    const QString id = generationPreview.value("previewId").toString();
    generationPreview = {}; generateApprove->setEnabled(false);
    showResponse(jobController->approveGenerate(id)); refreshContext();
}
void MeshCommandWidget::previewAcceptance()
{
    acceptancePreview = {}; acceptApprove->setEnabled(false);
    if (!jobController || selectedAttempt().isEmpty()) return;
    const auto response = jobController->previewAccept(selectedAttempt()); showResponse(response);
    if (response.value("valid").toBool())
    { acceptancePreview = response.value("result").toObject(); acceptApprove->setEnabled(true); }
}
void MeshCommandWidget::approveAcceptance()
{
    if (!jobController || acceptancePreview.isEmpty() || !acceptApprove->isEnabled()) return;
    const QString id = acceptancePreview.value("previewId").toString();
    acceptancePreview = {}; acceptApprove->setEnabled(false);
    showResponse(jobController->approveAccept(id)); refreshContext();
}
void MeshCommandWidget::cancelSelected()
{
    if (!jobController || selectedAttempt().isEmpty()) return;
    clearApprovals(); showResponse(jobController->cancel(selectedAttempt())); refreshContext();
}

void MeshCommandWidget::refreshContext()
{
    const bool oneSource = session && session->getSelectedModelIDs().size() == 1;
    generatePreview->setEnabled(jobController && oneSource);
    if (oneSource)
    {
        const uint id = session->getSelectedModelIDs().first();
        const auto stamp = session->modelCommandStamp(id);
        source->setText(tr("Read-only source: %1 | document %2 | revision %3 | m, m^3")
            .arg(session->getModel(id).getName(),stamp.first,QString::number(stamp.second)));
    }
    else source->setText(tr("Select one available project document; resolve numeric drafts first."));
    const QString selected = selectedAttempt();
    const auto rows = jobController ? jobController->jobs() : QJsonArray{};
    QSignalBlocker blocker(jobList);
    jobList->setRowCount(rows.size());
    QString selectedState; bool selectedCurrent = false;
    for (int row=0;row<rows.size();++row)
    {
        const auto job = rows[row].toObject();
        const QStringList cells{job.value("attemptId").toString(),job.value("status").toString(),
            job.value("phase").toString(),job.value("sourceName").toString(),job.value("sourceStatus").toString()};
        for (int column=0;column<cells.size();++column) jobList->setItem(row,column,new QTableWidgetItem(cells[column]));
        if (cells[0] == selected)
        { jobList->selectRow(row); selectedState = cells[1]; selectedCurrent = job.value("sourceCurrent").toBool(); }
    }
    acceptPreview->setEnabled(jobController && selectedState == "candidate_ready" && selectedCurrent);
    cancelButton->setEnabled(jobController && (selectedState == "queued" || selectedState == "running"));
    if (!acceptancePreview.isEmpty() && (selectedState != "candidate_ready" || !selectedCurrent))
    { acceptancePreview = {}; acceptApprove->setEnabled(false); if (jobController) jobController->invalidatePreviews(); }
}
