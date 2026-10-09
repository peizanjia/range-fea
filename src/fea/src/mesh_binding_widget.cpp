#include "mesh_binding_widget.h"
#include "model_command_host.h"
#include "session.h"
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace {
QJsonObject request(const QString &operation, const QJsonObject &parameters)
{
    return {{"apiVersion","range.host/v1"},{"requestId",QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"operation",operation},{"parameters",parameters}};
}
}

MeshBindingWidget::MeshBindingWidget(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *scope = new QLabel(tr("Bind the selected unbound tetrahedral document for static stress. "
        "Use its new physical node IDs. Enter explicit global XYZ supports and forces; "
        "blank support axes stay free. Previous mesh targets are never mapped automatically."),this);
    scope->setWordWrap(true); layout->addWidget(scope);
    layout->addWidget(new QLabel(tr("Selected document nodes: physical ID and XYZ coordinates (m)"),this));
    nodes=new QTableWidget(0,4,this); nodes->setObjectName("meshBindingNodes");
    nodes->setHorizontalHeaderLabels({tr("Physical node ID"),tr("X (m)"),tr("Y (m)"),tr("Z (m)")});
    nodes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    nodes->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    nodes->setMaximumHeight(160); layout->addWidget(nodes);
    auto *form = new QFormLayout;
    young = new QLineEdit(this); young->setObjectName("meshBindingYoung");
    poisson = new QLineEdit(this); poisson->setObjectName("meshBindingPoisson");
    density = new QLineEdit(this); density->setObjectName("meshBindingDensity");
    form->addRow(tr("Young modulus (Pa)"),young);
    form->addRow(tr("Poisson ratio"),poisson);
    form->addRow(tr("Density (kg/m^3)"),density); layout->addLayout(form);
    const auto table = [this,layout](const QString &name, const QString &label) {
        layout->addWidget(new QLabel(label,this));
        auto *value = new QTableWidget(0,4,this); value->setObjectName(name);
        value->setHorizontalHeaderLabels({tr("Physical node ID"),tr("X"),tr("Y"),tr("Z")});
        value->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        value->setMaximumHeight(160); layout->addWidget(value);
        auto *add = new QPushButton(tr("Add row"),this); add->setObjectName(name+"Add"); layout->addWidget(add);
        connect(add,&QPushButton::clicked,this,[this,value] {
            if(value->rowCount() >= 1000) return;
            const int row=value->rowCount(); value->insertRow(row);
            for(int column=0;column<4;++column) value->setItem(row,column,new QTableWidgetItem);
            invalidate();
        });
        connect(value,&QTableWidget::itemChanged,this,[this] { invalidate(); });
        return value;
    };
    supports=table("meshBindingSupports",tr("Displacement supports (m); blank XYZ axes are free"));
    forces=table("meshBindingForces",tr("Nodal forces (N); enter all XYZ axes, including explicit zeros"));
    previewButton=new QPushButton(tr("Preview static binding"),this); previewButton->setObjectName("meshBindingPreview");
    applyButton=new QPushButton(tr("Approve and apply this binding"),this); applyButton->setObjectName("meshBindingApply");
    layout->addWidget(previewButton); layout->addWidget(applyButton);
    status=new QLabel(this); status->setObjectName("meshBindingStatus"); status->setWordWrap(true); layout->addWidget(status);
    details=new QPlainTextEdit(this); details->setObjectName("meshBindingDetails"); details->setReadOnly(true); layout->addWidget(details);
    connect(previewButton,&QPushButton::clicked,this,&MeshBindingWidget::preview);
    connect(applyButton,&QPushButton::clicked,this,&MeshBindingWidget::apply);
    for(auto *field:{young,poisson,density}) connect(field,&QLineEdit::textChanged,this,[this] { invalidate(); });
    auto *timer=new QTimer(this); timer->setInterval(1000);
    connect(timer,&QTimer::timeout,this,[this] {
        nodes->setProperty("bindingContextPolls",nodes->property("bindingContextPolls").toInt()+1);
        const auto current=document(); previewButton->setEnabled(!current.isEmpty());
        if(current!=displayedDocument) refreshNodes();
        if(!previewId.isEmpty() && (current!=previewDocument || !session ||
            session->getModelCommandHost()->projectWorkspace()!=previewWorkspace)) invalidate();
    }); timer->start();
    previewButton->setEnabled(false); applyButton->setEnabled(false);
}

void MeshBindingWidget::setSession(Session *value)
{
    if(session==value) return;
    if(session) disconnect(session,nullptr,this,nullptr);
    session=value; invalidate(); details->clear();
    if(session) {
        const auto changed=[this] { invalidate(); };
        connect(session,&Session::modelSelectionChanged,this,changed);
        connect(session,&Session::documentRevisionChanged,this,changed);
        connect(session,&Session::runtimeDocumentRemoved,this,changed);
    }
    previewButton->setEnabled(!document().isEmpty());
    refreshNodes();
}
void MeshBindingWidget::refreshNodes()
{
    nodes->setProperty("bindingNodeRefreshes",nodes->property("bindingNodeRefreshes").toInt()+1);
    displayedDocument=document(); nodes->setRowCount(0);
    if(displayedDocument.isEmpty() || !session || session->getSelectedModelIDs().size()!=1) return;
    const auto &model=session->getModel(session->getSelectedModelIDs().first());
    if(model.getNNodes()>10000) return;
    nodes->setRowCount(int(model.getNNodes()));
    for(uint row=0;row<model.getNNodes();++row) {
        const auto &node=model.getNode(row);
        const QStringList values{QString::number(model.getNodePhysicalId(row)),
            QString::number(node.getX(),'g',17),QString::number(node.getY(),'g',17),QString::number(node.getZ(),'g',17)};
        for(int column=0;column<4;++column) nodes->setItem(int(row),column,new QTableWidgetItem(values[column]));
    }
}
QJsonObject MeshBindingWidget::document() const
{
    nodes->setProperty("bindingContextGate","no-session");
    if(!session) return {};
    const auto selected=session->getSelectedModelIDs();
    QVariantList selectedIds;
    for(const auto id:selected) selectedIds.append(id);
    nodes->setProperty("bindingSelectedModels",selectedIds);
    nodes->setProperty("bindingContextGate","selection-count");
    if(selected.size()!=1) return {};
    const auto stamp=session->modelCommandStamp(selected.first());
    auto *host=session->getModelCommandHost();
    const auto reply=host->handleReadOnly(request("context.query",{{"target",QJsonObject{
        {"documentId",stamp.first},{"sessionEpoch",host->sessionEpoch()}}}}));
    const auto context=reply.value("result").toObject();
    nodes->setProperty("bindingContextReply",QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
    const auto documents=context.value("documents").toArray();
    nodes->setProperty("bindingContextGate","host-context");
    if(reply.value("status")!=QJsonValue("ok") || context.value("editingBlocked").toBool() || documents.size()!=1) return {};
    const auto current=documents.first().toObject();
    nodes->setProperty("bindingContextGate","live-identity-revision");
    if(current.value("stateAvailability")!=QJsonValue("live") || current.value("documentId")!=QJsonValue(stamp.first)
        || current.value("documentRevision").toDouble()!=double(stamp.second)) return {};
    nodes->setProperty("bindingContextGate","available");
    return current;
}
void MeshBindingWidget::invalidate()
{
    previewId.clear(); previewDocument={}; previewWorkspace.clear(); applyButton->setEnabled(false);
    status->setText(tr("Review explicit material, new node IDs, supports and forces, then preview."));
}
void MeshBindingWidget::preview()
{
    invalidate(); details->clear(); const auto current=document();
    if(current.isEmpty()) return;
    QJsonObject material;
    const QList<QPair<QString,QLineEdit*>> fields{{"youngModulusPa",young},{"poissonRatio",poisson},{"densityKgM3",density}};
    for(const auto &field:fields) {
        bool ok=false; const double value=field.second->text().trimmed().toDouble(&ok);
        if(!ok || !std::isfinite(value)) { status->setText(tr("Enter finite material values.")); return; }
        material.insert(field.first,value);
    }
    QString error;
    const auto rows=[&error](QTableWidget *table,bool nullable) {
        QJsonArray result;
        for(int row=0;row<table->rowCount();++row) {
            const auto text=[table,row](int column) { return table->item(row,column)?table->item(row,column)->text().trimmed():QString(); };
            bool empty=true; for(int column=0;column<4;++column) empty=empty && text(column).isEmpty();
            if(empty) continue;
            bool ok=false; const qulonglong id=text(0).toULongLong(&ok);
            if(!ok || id==0 || id>=std::numeric_limits<uint>::max()) { error=QObject::tr("Use an existing new physical node ID."); return QJsonArray{}; }
            QJsonArray xyz;
            for(int column=1;column<4;++column) {
                if(nullable && text(column).isEmpty()) { xyz.append(QJsonValue::Null); continue; }
                const double value=text(column).toDouble(&ok);
                if(!ok || !std::isfinite(value)) { error=QObject::tr("Enter finite XYZ values; only support axes may be blank."); return QJsonArray{}; }
                xyz.append(value);
            }
            result.append(QJsonObject{{"nodePhysicalId",double(id)},{nullable?"values":"forceN",xyz}});
        }
        if(result.isEmpty()) error=QObject::tr("Enter at least one support and one force row.");
        return result;
    };
    const auto constraints=rows(supports,true); if(!error.isEmpty()) { status->setText(error); return; }
    const auto loads=rows(forces,false); if(!error.isEmpty()) { status->setText(error); return; }
    const QJsonObject arguments{{"unitSI",true},{"staticStress",true},{"material",material},{"constraints",constraints},{"loads",loads}};
    auto *host=session->getModelCommandHost();
    const auto reply=host->handle(request("command.preview",{{"target",QJsonObject{
        {"documentId",current.value("documentId")},{"sessionEpoch",current.value("sessionEpoch")}}},
        {"expectedDocumentRevision",current.value("documentRevision")},{"command","mesh.rebind.static"},
        {"commandVersion",1},{"arguments",arguments}}));
    details->setPlainText(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Indented)));
    if(reply.value("status")!=QJsonValue("preview")) { status->setText(reply.value("message").toString()); return; }
    previewId=reply.value("result").toObject().value("previewId").toString();
    previewDocument=current; previewWorkspace=host->projectWorkspace();
    applyButton->setEnabled(!previewId.isEmpty()); status->setText(tr("Review the binding preview and approve explicitly."));
}
void MeshBindingWidget::apply()
{
    if(!session || previewId.isEmpty() || document()!=previewDocument ||
       session->getModelCommandHost()->projectWorkspace()!=previewWorkspace) { invalidate(); return; }
    auto *host=session->getModelCommandHost(); const QString approved=previewId;
    invalidate();
    if(!host->approvePreview(approved)) { status->setText(tr("Approval is stale. Preview again.")); return; }
    const auto reply=host->handle(request("command.execute",{{"previewId",approved},
        {"idempotencyKey",QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
    details->setPlainText(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Indented)));
    status->setText(reply.value("status")==QJsonValue("accepted")?tr("Static binding accepted. Native static analysis is available."):reply.value("message").toString());
}
