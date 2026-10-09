#include "geometry_command_widget.h"

#include "model_command_host.h"
#include "session.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QStringList>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace
{
QString token() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject request(const QString &operation, const QJsonObject &parameters = {})
{
    return {{"apiVersion", "range.host/v1"}, {"requestId", token()},
            {"operation", operation}, {"parameters", parameters}};
}
QString responseMessage(const QJsonObject &response)
{
    return response.value("code").toString() + QStringLiteral(": ") + response.value("message").toString();
}
}

GeometryCommandWidget::GeometryCommandWidget(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *scope = new QLabel(tr("Geometry coordinates in m; right-handed axis-angle in deg. "
        "POINT / TRUSS1 / BEAM1 only. Rigid transforms affect the whole model. "
        "Global material and boundary directions are preserved."), this);
    scope->setWordWrap(true);
    layout->addWidget(scope);
    source = new QLabel(tr("Select one native document."), this);
    source->setObjectName("geometrySource");
    source->setWordWrap(true);
    layout->addWidget(source);
    command = new QComboBox(this);
    command->setObjectName("geometryCommand");
    command->addItem(tr("Set one physical node's XYZ coordinates"), "geometry.node_coordinates.set");
    command->addItem(tr("Transform the whole model (rotation and translation)"), "geometry.rigid_transform");
    layout->addWidget(command);
    auto *forms = new QStackedWidget(this);
    layout->addWidget(forms);
    auto *nodePage = new QWidget(forms);
    auto *nodeForm = new QFormLayout(nodePage);
    nodeId = new QLineEdit(nodePage);
    nodeId->setObjectName("geometryNodeId");
    nodeId->setPlaceholderText(tr("Stable physical node ID"));
    nodeForm->addRow(tr("Physical node ID"), nodeId);
    const auto addVector = [this](QFormLayout *form, const QString &label, const QString &prefix,
                                   std::array<QLineEdit *, 3> &fields, bool defaultZ) {
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        for (int i = 0; i < 3; ++i)
        {
            auto *field = new QLineEdit(defaultZ && i == 2 ? "1" : "0", row);
            field->setObjectName(prefix + QString::number(i));
            field->setPlaceholderText(QStringList{"X", "Y", "Z"}.at(i));
            fields[i] = field;
            rowLayout->addWidget(field);
            connect(field, &QLineEdit::textChanged, this, [this] {
                invalidatePreview(tr("Draft changed. Preview again before applying."));
            });
        }
        form->addRow(label, row);
    };
    addVector(nodeForm, tr("XYZ (m)"), "geometryCoordinates", coordinates, false);
    forms->addWidget(nodePage);
    auto *transformPage = new QWidget(forms);
    auto *transformForm = new QFormLayout(transformPage);
    addVector(transformForm, tr("Rotation center XYZ (m)"), "geometryCenter", center, false);
    addVector(transformForm, tr("Rotation axis XYZ"), "geometryAxis", axis, true);
    angle = new QLineEdit("0", transformPage);
    angle->setObjectName("geometryAngle");
    transformForm->addRow(tr("Right-handed angle (deg)"), angle);
    addVector(transformForm, tr("Translation XYZ (m)"), "geometryTranslation", translation, false);
    forms->addWidget(transformPage);
    connect(command, &QComboBox::currentIndexChanged, forms, &QStackedWidget::setCurrentIndex);
    connect(command, &QComboBox::currentIndexChanged, this, [this] {
        invalidatePreview(tr("Command changed. Preview again before applying."));
    });
    for (auto *field : {nodeId, angle})
        connect(field, &QLineEdit::textChanged, this, [this] {
            invalidatePreview(tr("Draft changed. Preview again before applying."));
        });
    auto *buttons = new QHBoxLayout;
    previewButton = new QPushButton(tr("Preview geometry change"), this);
    previewButton->setObjectName("geometryPreview");
    applyButton = new QPushButton(tr("Approve and apply this preview"), this);
    applyButton->setObjectName("geometryApproveApply");
    applyButton->setEnabled(false);
    buttons->addWidget(previewButton);
    buttons->addWidget(applyButton);
    layout->addLayout(buttons);
    status = new QLabel(this);
    status->setObjectName("geometryStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    changes = new QTableWidget(0, 7, this);
    changes->setObjectName("geometryChanges");
    changes->setHorizontalHeaderLabels({tr("Physical ID"), tr("Before X (m)"), tr("Before Y (m)"),
        tr("Before Z (m)"), tr("After X (m)"), tr("After Y (m)"), tr("After Z (m)")});
    changes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    changes->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(changes);
    receipt = new QLabel(this);
    receipt->setObjectName("geometryReceipt");
    receipt->setWordWrap(true);
    receipt->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(receipt);
    connect(previewButton, &QPushButton::clicked, this, &GeometryCommandWidget::preview);
    connect(applyButton, &QPushButton::clicked, this, &GeometryCommandWidget::approveAndApply);
    // Source files and workspace state may change without a runtime revision signal.
    auto *timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!approvedPreview.isEmpty() && !previewIsCurrent())
            invalidatePreview(tr("Document, source, or workspace changed. Preview again."));
    });
    timer->start();
    previewButton->setEnabled(false);
}

void GeometryCommandWidget::setSession(Session *value)
{
    if (session == value) return;
    if (session) disconnect(session, nullptr, this, nullptr);
    ++contextRefreshGeneration;
    contextRefreshPending = false;
    session = value;
    invalidatePreview(tr("Select one native document, enter geometry, and preview."));
    if (session)
    {
        const auto changed = [this] {
            invalidatePreview(tr("Document or selection changed. Preview again."));
            queueContextRefresh();
        };
        connect(session, &Session::modelSelectionChanged, this, changed);
        connect(session, &Session::modelAdded, this, changed);
        connect(session, &Session::modelRemoved, this, changed);
        connect(session, &Session::documentRevisionChanged, this, changed);
        connect(session, &Session::runtimeDocumentRemoved, this, changed);
        connect(session, &QObject::destroyed, this, changed);
    }
    refreshContext();
}

void GeometryCommandWidget::queueContextRefresh()
{
    if (contextRefreshPending) return;
    contextRefreshPending = true;
    const auto generation = contextRefreshGeneration;
    // A read-only context query can flush pending project changes. Let the
    // complete legacy mutation finish before observing its committed context.
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != contextRefreshGeneration) return;
        contextRefreshPending = false;
        refreshContext();
    });
}

QJsonObject GeometryCommandWidget::currentDocument() const
{
    if (!session || session->getSelectedModelIDs().size() != 1) return {};
    const uint modelId = session->getSelectedModelIDs().first();
    const auto stamp = session->modelCommandStamp(modelId);
    auto *host = session->getModelCommandHost();
    const QJsonObject target{{"documentId", stamp.first}, {"sessionEpoch", host->sessionEpoch()}};
    const auto response = host->handleReadOnly(request("context.query", {{"target", target}}));
    const auto context = response.value("result").toObject();
    const auto documents = context.value("documents").toArray();
    if (response.value("status") != QJsonValue("ok") || context.value("editingBlocked").toBool()
        || documents.size() != 1) return {};
    const auto document = documents.first().toObject();
    if (document.value("documentId").toString() != stamp.first
        || document.value("documentRevision").toDouble() != double(stamp.second)
        || document.value("stateAvailability") != QJsonValue("live")) return {};
    return document;
}

void GeometryCommandWidget::refreshContext()
{
    const auto document = currentDocument();
    previewButton->setEnabled(!document.isEmpty());
    if (!approvedPreview.isEmpty() && !previewIsCurrent())
        invalidatePreview(tr("Document or selection changed. Preview again."));
    source->setText(document.isEmpty() ? tr("Select one available native document.")
        : tr("Source: %1 | document %2 | revision %3 | session %4")
            .arg(document.value("modelName").toString(), document.value("documentId").toString(),
                 QString::number(document.value("documentRevision").toDouble(), 'f', 0),
                 document.value("sessionEpoch").toString()));
}

QJsonObject GeometryCommandWidget::arguments(QString *error) const
{
    const auto readVector = [error](const std::array<QLineEdit *, 3> &fields, double bound) {
        QJsonArray result;
        for (auto *field : fields)
        {
            bool ok = false;
            const double value = field->text().trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(value) || std::abs(value) > bound)
            {
                *error = tr("Enter finite XYZ values within the stated range (+/-1000000 m for coordinates).");
                return QJsonArray{};
            }
            result.append(value);
        }
        return result;
    };
    QJsonObject result{{"unit", "m"}, {"directionPolicy", "preserve_global"}};
    if (command->currentIndex() == 0)
    {
        bool ok = false;
        const QString text = nodeId->text().trimmed();
        const qulonglong id = text.toULongLong(&ok, 10);
        if (!ok || id == 0 || id >= std::numeric_limits<uint>::max())
        {
            *error = tr("Enter a stable unsigned physical node ID between 1 and 4294967294.");
            return {};
        }
        const auto xyz = readVector(coordinates, 1e6);
        if (xyz.isEmpty()) return {};
        result.insert("nodes", QJsonArray{QJsonObject{{"physicalNodeId", double(id)}, {"coordinates", xyz}}});
        return result;
    }
    const auto c = readVector(center, 1e6);
    const auto a = readVector(axis, std::numeric_limits<double>::max());
    const auto t = readVector(translation, 1e6);
    bool ok = false;
    const double degrees = angle->text().trimmed().toDouble(&ok);
    if (c.isEmpty() || a.isEmpty() || t.isEmpty()) return {};
    if (!ok || !std::isfinite(degrees) || std::abs(degrees) > 360
        || (a[0].toDouble() == 0 && a[1].toDouble() == 0 && a[2].toDouble() == 0))
    {
        *error = tr("Use a nonzero finite rotation axis and an angle within +/-360 deg.");
        return {};
    }
    result.insert("angleUnit", "deg");
    result.insert("center", c); result.insert("axis", a);
    result.insert("angle", degrees); result.insert("translation", t);
    return result;
}

bool GeometryCommandWidget::previewIsCurrent() const
{
    if (!session || approvedPreview.isEmpty()
        || session->getModelCommandHost()->projectWorkspace() != previewWorkspace) return false;
    const auto document = currentDocument();
    if (document.isEmpty()) return false;
    for (const auto &key : {"documentId", "sessionEpoch", "documentRevision", "project", "draft"})
        if (document.value(key) != previewDocument.value(key)) return false;
    return true;
}

void GeometryCommandWidget::invalidatePreview(const QString &reason)
{
    approvedPreview = {}; previewDocument = {}; previewWorkspace.clear();
    if (applyButton) applyButton->setEnabled(false);
    if (status) status->setText(reason);
}

void GeometryCommandWidget::preview()
{
    invalidatePreview(tr("Preparing preview..."));
    changes->setRowCount(0); receipt->clear();
    const auto document = currentDocument();
    if (document.isEmpty()) { status->setText(tr("Select one available native document.")); return; }
    QString error;
    const auto values = arguments(&error);
    if (values.isEmpty()) { status->setText(error); return; }
    auto *host = session->getModelCommandHost();
    const auto response = host->handle(request("command.preview", {
        {"target", QJsonObject{{"documentId", document.value("documentId")}, {"sessionEpoch", document.value("sessionEpoch")}}},
        {"expectedDocumentRevision", document.value("documentRevision")},
        {"command", command->currentData().toString()}, {"commandVersion", 1}, {"arguments", values}}));
    if (response.value("status") != QJsonValue("preview")) { status->setText(responseMessage(response)); return; }
    approvedPreview = response.value("result").toObject();
    previewDocument = document;
    previewWorkspace = host->projectWorkspace();
    const auto before = approvedPreview.value("before").toObject().value("nodes").toArray();
    const auto after = approvedPreview.value("after").toObject().value("nodes").toArray();
    changes->setRowCount(before.size());
    for (int row = 0; row < before.size(); ++row)
    {
        const auto b = before[row].toObject();
        const auto a = after[row].toObject();
        changes->setItem(row, 0, new QTableWidgetItem(QString::number(b.value("physicalNodeId").toDouble(), 'f', 0)));
        for (int i = 0; i < 3; ++i)
        {
            changes->setItem(row, i + 1, new QTableWidgetItem(QString::number(b.value("coordinates").toArray()[i].toDouble(), 'g', 17)));
            changes->setItem(row, i + 4, new QTableWidgetItem(QString::number(a.value("coordinates").toArray()[i].toDouble(), 'g', 17)));
        }
    }
    status->setText(before.isEmpty() ? tr("Preview: no coordinate changes; applying adds no history.")
        : tr("Preview: %1 changed physical nodes. Review before/after values, then approve.").arg(before.size()));
    receipt->setText(tr("Preview %1 | source revision %2 | candidate SHA-256 %3")
        .arg(approvedPreview.value("previewId").toString(),
             QString::number(document.value("documentRevision").toDouble(), 'f', 0),
             approvedPreview.value("candidateFingerprint").toString()));
    applyButton->setEnabled(previewIsCurrent());
}

void GeometryCommandWidget::approveAndApply()
{
    if (!previewIsCurrent()) { invalidatePreview(tr("Preview is stale. Preview the current document again.")); return; }
    auto *host = session->getModelCommandHost();
    const QString previewId = approvedPreview.value("previewId").toString();
    applyButton->setEnabled(false);
    if (!host->approvePreview(previewId)) { invalidatePreview(tr("Approval rejected. Preview again.")); return; }
    const auto response = host->handle(request("command.execute", {{"previewId", previewId}, {"idempotencyKey", token()}}));
    invalidatePreview(response.value("status") == QJsonValue("accepted") ? tr("Geometry operation accepted.") : responseMessage(response));
    const auto result = response.value("result").toObject();
    receipt->setText(tr("Receipt: %1 | operation %2 | revision %3 -> %4 | history %5 | core journal %6")
        .arg(response.value("code").toString(), result.value("operationId").toString(),
             QString::number(result.value("beforeRevision").toDouble(), 'f', 0),
             QString::number(result.value("afterRevision").toDouble(), 'f', 0),
             result.value("history").toString(), result.value("coreJournalRevision").toString()));
    refreshContext();
}
