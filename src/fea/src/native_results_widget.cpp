#include "native_results_widget.h"
#include "workflow_process.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
constexpr int MaxSamples = 5000;
constexpr int MaxChannels = 5000;
constexpr int PageSize = 200;
bool number(const QJsonValue &value) { return value.isDouble() && std::isfinite(value.toDouble()); }
bool integer(const QJsonValue &value, int minimum, int maximum)
{
    return number(value) && value.toDouble() >= minimum && value.toDouble() <= maximum &&
        std::floor(value.toDouble()) == value.toDouble();
}
QString cell(QString value)
{
    if (!value.isEmpty() && QStringLiteral("=+-@").contains(value.front())) value.prepend('\'');
    value.replace('"', "\"\"");
    return '"' + value + '"';
}
QString jsonText(const QJsonObject &object)
{ return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
QString jsonValueText(const QJsonValue &value)
{
    const QByteArray wrapped = QJsonDocument(QJsonArray{value.isUndefined() ? QJsonValue(QJsonValue::Null) : value})
        .toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}
}

class NativeResultCanvas : public QWidget
{
public:
    QJsonObject snapshot;
    explicit NativeResultCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName("nativeResultCanvas");
        setAccessibleName(tr("Native result channel chart"));
        setMinimumSize(310, 180);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        painter.setRenderHint(QPainter::Antialiasing);
        const auto points = snapshot.value("samples").toArray();
        painter.setPen(palette().color(QPalette::Text));
        if (points.isEmpty()) { painter.drawText(rect(), Qt::AlignCenter, tr("No native samples available")); return; }
        double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
        for (const auto &entry : points)
        {
            const double value = entry.toObject().value("value").toDouble();
            minimum = qMin(minimum, value); maximum = qMax(maximum, value);
        }
        const double start = points.first().toObject().value("timeS").toDouble();
        const double end = points.last().toObject().value("timeS").toDouble();
        // Normalize before subtraction so finite extreme values cannot overflow.
        const double scale = qMax(qMax(std::abs(minimum), std::abs(maximum)), 1e-300);
        double lo = minimum / scale, hi = maximum / scale;
        const double padding = qMax((hi - lo) * .08, .01);
        lo -= padding; hi += padding;
        const QRectF area = QRectF(rect()).adjusted(80, 28, -18, -42);
        painter.setPen(palette().color(QPalette::Mid)); painter.drawRect(area);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(8, 3, width() - 16, 22), Qt::AlignCenter,
                         snapshot.value("channelId").toString() + " [" + snapshot.value("unit").toString() + "]");
        painter.drawText(QRectF(0, area.top(), 74, 20), Qt::AlignRight, QString::number(maximum, 'g', 5));
        painter.drawText(QRectF(0, area.bottom() - 20, 74, 20), Qt::AlignRight, QString::number(minimum, 'g', 5));
        painter.drawText(QRectF(area.left(), area.bottom() + 4, area.width(), 30), Qt::AlignCenter,
                         tr("Native time [s]: %1 – %2").arg(start, 0, 'g', 6).arg(end, 0, 'g', 6));
        QPainterPath path;
        for (int index = 0; index < points.size(); ++index)
        {
            const auto point = points[index].toObject();
            const double fraction = end > start ? (point.value("timeS").toDouble() - start) / (end - start) : .5;
            const QPointF pixel(area.left() + fraction * area.width(),
                area.bottom() - (point.value("value").toDouble() / scale - lo) / (hi - lo) * area.height());
            const bool gap = index > 0 && point.value("recordNo").toInt() !=
                points[index - 1].toObject().value("recordNo").toInt() + 1;
            if (index == 0 || gap) path.moveTo(pixel); else path.lineTo(pixel);
            painter.setPen(QPen(QColor(30, 130, 220), 3)); painter.drawPoint(pixel);
            if (points.size() == 1) { painter.setPen(QPen(QColor(30, 130, 220), 4)); painter.drawPoint(pixel); }
        }
        painter.setPen(QPen(QColor(30, 130, 220), 1.7)); painter.drawPath(path);
    }
};

NativeResultsWidget::NativeResultsWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("nativeResultsWidget");
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    channelBox = new QComboBox; channelBox->setObjectName("nativeResultChannel");
    channelBox->setAccessibleName(tr("Native result channel"));
    rangeBox = new QComboBox; rangeBox->setObjectName("nativeResultRange");
    rangeBox->addItem(tr("All native samples"), 0.0);
    rangeBox->addItem(tr("Last 0.5 s"), .5); rangeBox->addItem(tr("Last 1 s"), 1.0); rangeBox->addItem(tr("Last 5 s"), 5.0);
    holdButton = new QPushButton(tr("Hold snapshot")); holdButton->setCheckable(true); holdButton->setObjectName("nativeResultHold");
    exportButton = new QPushButton(tr("Export CSV")); exportButton->setObjectName("nativeResultExport");
    auto *refresh = new QPushButton(tr("Refresh")); refresh->setObjectName("nativeResultRefresh");
    fieldButton = new QPushButton(tr("Show terminal result field")); fieldButton->setObjectName("nativeResultDisplayField");
    fieldButton->setToolTip(tr("Open the verified terminal result and locate this physical node in its native field."));
    toolbar->addWidget(channelBox, 1); toolbar->addWidget(rangeBox); toolbar->addWidget(holdButton);
    toolbar->addWidget(exportButton); toolbar->addWidget(refresh); layout->addLayout(toolbar);
    layout->addWidget(fieldButton, 0, Qt::AlignRight);
    sourceInfo = new QLabel; sourceInfo->setWordWrap(true); sourceInfo->setObjectName("nativeResultSource"); layout->addWidget(sourceInfo);
    sourceInfo->setTextFormat(Qt::PlainText);
    canvas = new NativeResultCanvas(this); layout->addWidget(canvas, 1);
    table = new QTableWidget(0, 4); table->setObjectName("nativeResultTable");
    table->setHorizontalHeaderLabels({tr("Record"), tr("Time [s]"), tr("Value"), tr("Origin")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(table, 1);
    statusLabel = new QLabel(tr("Select a registered native result.")); statusLabel->setWordWrap(true);
    statusLabel->setObjectName("nativeResultStatus"); layout->addWidget(statusLabel);
    statusLabel->setTextFormat(Qt::PlainText);
    connect(refresh, &QPushButton::clicked, this, &NativeResultsWidget::refreshResults);
    connect(holdButton, &QPushButton::toggled, this, &NativeResultsWidget::setHeld);
    connect(exportButton, &QPushButton::clicked, this, &NativeResultsWidget::chooseExport);
    connect(fieldButton, &QPushButton::clicked, this, [this] {
        const auto snapshot = displaySnapshot();
        if (workflowSource || !queryHandler || snapshot.value("provenance").toObject().value("completed") != QJsonValue(true)) return;
        const auto handler = queryHandler;
        QJsonObject result;
        try { result = handler("native.result.display", {{"jobId", snapshot.value("jobId")},
            {"artifactId", snapshot.value("artifactId")}, {"channelId", snapshot.value("channelId")}}); }
        catch (...) { statusLabel->setText(tr("The native result field could not be opened.")); return; }
        if (snapshot.value("jobId") != QJsonValue(jobId)) return;
        statusLabel->setText(result.value("valid").toBool()
            ? tr("Displayed terminal %1 magnitude [%2] at physical node %3.")
                .arg(result.value("variableId").toString().mid(4).replace('_', ' '), result.value("unit").toString(),
                     result.value("nodePhysicalId").toVariant().toString())
            : result.value("message").toString(tr("The native result field is unavailable.")));
    });
    connect(rangeBox, &QComboBox::currentIndexChanged, this, &NativeResultsWidget::refreshView);
    connect(channelBox, &QComboBox::currentIndexChanged, this, [this] {
        selectedChannel = channelBox->currentData().toString(); loadChannel();
    });
    liveTimer = new QTimer(this); liveTimer->setObjectName("nativeResultLiveTimer"); liveTimer->setInterval(1000);
    connect(liveTimer, &QTimer::timeout, this, [this] {
        if (isVisible() && fixedArtifactId.isEmpty() && followLive &&
            pendingOperation.isEmpty() && pendingId.isEmpty()) refreshResults();
    });
    liveTimer->start();
    refreshView();
}

void NativeResultsWidget::clearSource()
{
    ++generation; pendingId.clear(); pendingOperation.clear(); followLive = false;
    selectedChannel.clear(); channels = {}; stagedChannels = {}; samples = {}; provenance = {}; summary = {}; heldSnapshot = {};
    const QSignalBlocker channelBlock(channelBox), holdBlock(holdButton);
    channelBox->clear(); holdButton->setChecked(false); refreshView();
}

void NativeResultsWidget::setQueryHandler(QueryHandler handler)
{
    queryHandler = std::move(handler);
    if (!workflowSource) { clearSource(); if (!jobId.isEmpty()) refreshResults(); }
}

void NativeResultsWidget::setArtifact(const QString &job, const QString &artifact, const QString &label)
{
    if (!workflowSource && jobId == job && fixedArtifactId == artifact && sourceLabel == label) return;
    clearSource(); workflowSource = false; jobId = job; artifactId = artifact; fixedArtifactId = artifact; sourceLabel = label; mode.clear();
    followLive = !job.isEmpty() && artifact.isEmpty();
    refreshView(); refreshResults();
}

void NativeResultsWidget::setWorkflow(WorkflowProcess *service)
{
    if (workflow == service) return;
    if (workflow) disconnect(workflow, nullptr, this, nullptr);
    workflow = service;
    if (workflowSource) clearSource();
    if (!workflow) return;
    connect(workflow, &WorkflowProcess::replyReceived, this,
        [this](const QString &id, const QString &operation, const QJsonObject &response) {
        if (!workflowSource || id != pendingId || operation != QStringLiteral("result.") + pendingOperation + ".query") return;
        const QString kind = pendingOperation; pendingId.clear(); pendingOperation.clear(); receive(kind, response);
    });
    connect(workflow, &WorkflowProcess::requestFailed, this,
        [this](const QString &id, const QString &, const QString &code, const QString &message) {
        if (workflowSource && id == pendingId) fail(message, code == "RESULT_PENDING");
    });
    connect(workflow, &WorkflowProcess::readyChanged, this, [this](bool ready) {
        if (!workflowSource) return;
        if (ready) refreshResults(); else fail(tr("Workspace disconnected; retained snapshot is historical."));
    });
    if (workflowSource) refreshResults();
}

void NativeResultsWidget::setWorkflowJob(const QString &job, const QString &resultMode)
{
    if (workflowSource && jobId == job && mode == resultMode) return;
    clearSource(); workflowSource = true; jobId = job; mode = resultMode; artifactId.clear(); fixedArtifactId.clear();
    followLive = !job.isEmpty();
    sourceLabel = tr("Control workflow · %1").arg(mode); refreshView(); refreshResults();
}

void NativeResultsWidget::query(const QString &operation, QJsonObject arguments)
{
    arguments.insert("jobId", jobId);
    if (!artifactId.isEmpty()) arguments.insert("artifactId", artifactId);
    if (workflowSource)
    {
        if (!workflow || !workflow->isReady()) { fail(tr("Workflow service is unavailable.")); return; }
        arguments.insert("mode", mode); pendingOperation = operation;
        pendingId = workflow->request("result." + operation + ".query", arguments);
        return;
    }
    if (!queryHandler) { fail(tr("Native result service is unavailable.")); return; }
    const auto handler = queryHandler;
    const quint64 requestGeneration = generation;
    pendingOperation = operation;
    // Defer host callbacks so long paginated reads yield between bounded pages.
    QTimer::singleShot(0, this, [this, handler, operation, arguments, requestGeneration] {
        if (requestGeneration != generation) return;
        QJsonObject response;
        try { response = handler("native.result." + operation + ".query", arguments); }
        catch (const std::exception &error) { if (requestGeneration == generation) fail(QString::fromUtf8(error.what())); return; }
        catch (...) { if (requestGeneration == generation) fail(tr("Native result query failed.")); return; }
        if (requestGeneration != generation) return;
        pendingOperation.clear(); receive(operation, response);
    });
}

void NativeResultsWidget::refreshResults()
{
    if (jobId.isEmpty()) return;
    ++generation; pendingId.clear(); pendingOperation.clear(); stagedChannels = {}; channelOffset = 0;
    // Auto-selected live artifacts pin one pagination sequence only. A new
    // refresh discovers the latest generation; explicit caller artifacts stay fixed.
    if (fixedArtifactId.isEmpty() && provenance.value("live").toBool())
    { artifactId.clear(); samples = {}; summary = {}; provenance = {}; refreshView(); }
    statusLabel->setText(tr("Reading native result catalog…"));
    query("catalog", {{"offset", 0}, {"limit", PageSize}});
}

void NativeResultsWidget::fail(const QString &message, bool retryPending)
{
    ++generation; pendingId.clear(); pendingOperation.clear();
    followLive = retryPending && fixedArtifactId.isEmpty();
    statusLabel->setText(message.isEmpty() ? tr("Native result query failed.") : message);
    emit queryFailed(statusLabel->text());
}

void NativeResultsWidget::receive(const QString &operation, const QJsonObject &response)
{
    if (response.value("valid") != true)
    { fail(response.value("message").toString(), response.value("code").toString() == "RESULT_PENDING"); return; }
    const QString returnedArtifact = response.value("artifactId").toString();
    if (returnedArtifact.isEmpty() || (!artifactId.isEmpty() && returnedArtifact != artifactId) ||
        response.value("operation").toString() != operation ||
        (response.contains("jobId") && response.value("jobId").toString() != jobId))
    { fail(tr("Native result identity changed; select the result again.")); return; }
    if (operation == "catalog")
    {
        if (!integer(response.value("frameCount"), 0, MaxSamples))
        { fail(tr("Native result frame count exceeds the supported 5000 samples.")); return; }
        artifactId = returnedArtifact; provenance = response; provenance.remove("frames");
        followLive = fixedArtifactId.isEmpty() && response.value("live").toBool();
        refreshView();
        if (response.value("analysis").toObject().value("type").toString() != "structural-dynamics")
        { samples = {}; summary = {}; refreshView(); fail(tr("This result is %1; native time series require structural dynamics.")
               .arg(response.value("analysis").toObject().value("type").toString())); return; }
        query("channels", {{"offset", 0}, {"limit", PageSize}}); return;
    }
    if (operation == "channels")
    {
        if (!response.value("channels").isArray() || !integer(response.value("channelCount"), 0, MaxChannels) ||
            !integer(response.value("offset"), channelOffset, channelOffset))
        { fail(tr("Native channel catalog page is inconsistent.")); return; }
        const auto page = response.value("channels").toArray();
        const int total = response.value("channelCount").toInt();
        if (page.size() > PageSize || channelOffset + page.size() > total)
        { fail(tr("Native channel catalog page is inconsistent.")); return; }
        for (const auto &entry : page)
        {
            const auto channel = entry.toObject(); const QString id = channel.value("channelId").toString();
            if (id.isEmpty() || !channel.value("unit").isString()) { fail(tr("Native channel identity or unit is missing.")); return; }
            for (const auto &previous : stagedChannels)
                if (previous.toObject().value("channelId").toString() == id) { fail(tr("Duplicate native channel identity.")); return; }
            stagedChannels.append(channel);
        }
        channelOffset += page.size();
        if (channelOffset < total)
        {
            const QString cursor = response.value("nextCursor").toString();
            if (page.isEmpty() || cursor.isEmpty()) { fail(tr("Native channel page has no continuation.")); return; }
            query("channels", {{"cursor", cursor}, {"limit", PageSize}}); return;
        }
        channels = stagedChannels;
        const QSignalBlocker block(channelBox); channelBox->clear();
        for (const auto &entry : channels)
        {
            const auto channel = entry.toObject(); const QString id = channel.value("channelId").toString();
            channelBox->addItem(id + " [" + channel.value("unit").toString() + "]", id);
            channelBox->setItemData(channelBox->count() - 1, channel.value("unit").toString(), Qt::UserRole + 1);
        }
        const int previous = channelBox->findData(selectedChannel);
        channelBox->setCurrentIndex(previous >= 0 ? previous : (channelBox->count() > 0 ? 0 : -1));
        selectedChannel = channelBox->currentData().toString(); loadChannel(); return;
    }
    if (response.value("channelId").toString() != selectedChannel ||
        response.value("unit").toString() != channelBox->currentData(Qt::UserRole + 1).toString())
    { fail(tr("Native channel identity or unit changed.")); return; }
    if (operation == "series")
    {
        if (!response.value("samples").isArray() || !integer(response.value("offset"), samples.size(), samples.size()))
        { fail(tr("Native sample page is inconsistent.")); return; }
        const auto page = response.value("samples").toArray();
        if (page.size() > PageSize || samples.size() + page.size() > MaxSamples)
        { fail(tr("Native sample page exceeds the supported limit.")); return; }
        double previousTime = samples.isEmpty() ? -std::numeric_limits<double>::infinity() : samples.last().toObject().value("timeS").toDouble();
        int previousRecord = samples.isEmpty() ? 0 : samples.last().toObject().value("recordNo").toInt();
        for (const auto &entry : page)
        {
            const auto point = entry.toObject();
            if (!integer(point.value("recordNo"), previousRecord + 1, 1000000) || !number(point.value("timeS")) ||
                point.value("timeS").toDouble() <= previousTime || !number(point.value("value")))
            { fail(tr("Native sample contains missing or inconsistent values.")); return; }
            const QString originKind = point.value("originKind").toString();
            if ((originKind != "produced" && originKind != "reused-history" && originKind != "legacy-history-unattributed") ||
                (originKind == "legacy-history-unattributed" ? !point.value("origin").isNull() : !point.value("origin").isObject()) ||
                (originKind != "produced" && !point.value("historyProof").isObject()))
            { fail(tr("Native sample origin or history proof is missing.")); return; }
            previousRecord = point.value("recordNo").toInt(); previousTime = point.value("timeS").toDouble();
        }
        for (const auto &entry : page) samples.append(entry);
        refreshView();
        const QString cursor = response.value("nextCursor").toString();
        if (!cursor.isEmpty())
        {
            if (page.isEmpty() || samples.size() == MaxSamples) { fail(tr("Native series continuation exceeds the supported limit.")); return; }
            query("series", {{"channelId", selectedChannel}, {"cursor", cursor}, {"limit", PageSize}}); return;
        }
        if (samples.size() != provenance.value("frameCount").toInt())
        { fail(tr("Native series does not cover its declared frames.")); return; }
        query("summary", {{"channelId", selectedChannel}}); return;
    }
    if (operation == "summary")
    {
        if (!integer(response.value("sampleCount"), samples.size(), samples.size()))
        { fail(tr("Native summary sample count differs from the series.")); return; }
        for (const auto &key : {"mean", "rms", "min", "max"})
            if (!number(response.value(key))) { fail(tr("Native summary is unavailable.")); return; }
        summary = response; refreshView();
    }
}

void NativeResultsWidget::loadChannel()
{
    ++generation; pendingId.clear(); pendingOperation.clear(); samples = {}; summary = {};
    refreshView();
    if (!selectedChannel.isEmpty()) query("series", {{"channelId", selectedChannel}, {"offset", 0}, {"limit", PageSize}});
}

QJsonObject NativeResultsWidget::currentSnapshot() const
{
    const double window = rangeBox->currentData().toDouble();
    QJsonArray visible;
    const double end = samples.isEmpty() ? 0 : samples.last().toObject().value("timeS").toDouble();
    for (const auto &entry : samples)
        if (window == 0 || entry.toObject().value("timeS").toDouble() >= end - window) visible.append(entry);
    QJsonObject range{{"windowSeconds", window}, {"startTimeS", QJsonValue::Null}, {"endTimeS", QJsonValue::Null}};
    if (!visible.isEmpty()) { range.insert("startTimeS", visible.first().toObject().value("timeS")); range.insert("endTimeS", visible.last().toObject().value("timeS")); }
    bool sparse = provenance.value("sparse").toBool();
    for (int index = 1; index < samples.size(); ++index)
        if (samples[index].toObject().value("recordNo").toInt() != samples[index - 1].toObject().value("recordNo").toInt() + 1) sparse = true;
    return {{"jobId", jobId}, {"artifactId", artifactId}, {"sourceLabel", sourceLabel},
        {"channelId", selectedChannel}, {"unit", channelBox->currentData(Qt::UserRole + 1).toString()},
        {"range", range}, {"provenance", provenance}, {"samples", visible}, {"sparse", sparse},
        {"complete", !selectedChannel.isEmpty() && samples.size() == provenance.value("frameCount").toInt()},
        {"summary", summary}, {"summaryScope", "all-native-frames"}};
}

QJsonObject NativeResultsWidget::displaySnapshot() const
{ return holdButton->isChecked() ? heldSnapshot : currentSnapshot(); }

void NativeResultsWidget::setHeld(bool held)
{
    if (holdButton->isChecked() != held) { holdButton->setChecked(held); return; }
    if (held && !heldSnapshot.isEmpty()) return;
    if (held) heldSnapshot = currentSnapshot(); else heldSnapshot = {};
    refreshView();
}

void NativeResultsWidget::refreshView()
{
    const auto snapshot = displaySnapshot(); const auto points = snapshot.value("samples").toArray();
    channelBox->setEnabled(!holdButton->isChecked()); rangeBox->setEnabled(!holdButton->isChecked());
    holdButton->setText(holdButton->isChecked() ? tr("Resume display") : tr("Hold snapshot"));
    exportButton->setEnabled(!points.isEmpty()); canvas->snapshot = snapshot; canvas->update();
    fieldButton->setEnabled(!workflowSource && bool(queryHandler) && !snapshot.value("channelId").toString().isEmpty()
                           && snapshot.value("provenance").toObject().value("completed") == QJsonValue(true));
    table->setRowCount(points.size());
    table->setHorizontalHeaderLabels({tr("Record"), tr("Time [s]"), tr("Value [%1]").arg(snapshot.value("unit").toString()), tr("Origin")});
    for (int row = 0; row < points.size(); ++row)
    {
        const auto point = points[row].toObject();
        for (int column = 0; column < 3; ++column)
        {
            const auto value = point.value(column == 0 ? "recordNo" : column == 1 ? "timeS" : "value");
            table->setItem(row, column, new QTableWidgetItem(QString::number(value.toDouble(), 'g', 17)));
        }
        auto *originItem = new QTableWidgetItem(point.value("originKind").toString());
        originItem->setToolTip(tr("Original source: %1\nHistory proof: %2")
            .arg(jsonValueText(point.value("origin")), jsonValueText(point.value("historyProof"))));
        table->setItem(row, 3, originItem);
    }
    const auto origin = snapshot.value("provenance").toObject();
    const QString coverage = snapshot.value("sparse").toBool() ? tr("Sparse native records") : tr("Actual native records");
    const QString lifecycle = origin.value("live").toBool() ? tr("Live snapshot") : tr("Published snapshot");
    const auto source = origin.value("sourceSnapshotRef").toObject();
    const QString version = source.value("sourceRevision").toString(tr("Template snapshot"));
    const QString modelSha = source.value("modelSha256").toString(source.value("sourceSha256").toString());
    sourceInfo->setText(tr("%1 · %2 frames · %3 · %4\nSource version: %5 · model %6 · %7 history frames reused")
        .arg(snapshot.value("sourceLabel").toString()).arg(origin.value("frameCount").toInt())
        .arg(coverage, lifecycle, version, modelSha.left(12)).arg(origin.value("reusedHistoryCount").toInt()));
    sourceInfo->setToolTip(jsonText(origin));
    const auto stats = snapshot.value("summary").toObject();
    statusLabel->setText(tr("%1 displayed native samples%2%3").arg(points.size())
        .arg(holdButton->isChecked() ? tr(" · snapshot held") : QString())
        .arg(stats.isEmpty() ? QString() : tr(" · all-frame min %1, max %2, RMS %3 [%4]")
             .arg(stats.value("min").toDouble(), 0, 'g', 7).arg(stats.value("max").toDouble(), 0, 'g', 7)
             .arg(stats.value("rms").toDouble(), 0, 'g', 7).arg(snapshot.value("unit").toString())));
    emit snapshotChanged();
}

bool NativeResultsWidget::writeSnapshot(const QJsonObject &snapshot, const QString &path, QString *error)
{
    const auto points = snapshot.value("samples").toArray();
    if (points.isEmpty()) { if (error) *error = tr("No native samples to export."); return false; }
    QByteArray csv = "job_id,artifact_id,source_label,channel_id,unit,range_json,provenance_json,complete,sparse,record_no,time_s,value,origin_kind,origin_json,history_proof_json\n";
    const QStringList prefix{cell(snapshot.value("jobId").toString()), cell(snapshot.value("artifactId").toString()),
        cell(snapshot.value("sourceLabel").toString()), cell(snapshot.value("channelId").toString()), cell(snapshot.value("unit").toString()),
        cell(jsonText(snapshot.value("range").toObject())), cell(jsonText(snapshot.value("provenance").toObject())),
        snapshot.value("complete").toBool() ? "true" : "false", snapshot.value("sparse").toBool() ? "true" : "false"};
    for (const auto &entry : points)
    {
        const auto point = entry.toObject(); QStringList row = prefix;
        for (const auto &key : {"recordNo", "timeS", "value"}) row.append(QString::number(point.value(key).toDouble(), 'g', 17));
        row.append(cell(point.value("originKind").toString())); row.append(cell(jsonValueText(point.value("origin"))));
        row.append(cell(jsonValueText(point.value("historyProof"))));
        csv += row.join(',').toUtf8() + '\n';
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(csv) != csv.size() || !file.commit())
    { if (error) *error = tr("Could not export the frozen native snapshot."); return false; }
    return true;
}

bool NativeResultsWidget::exportCsv(const QString &path, QString *error) const
{ return writeSnapshot(displaySnapshot(), path, error); }

void NativeResultsWidget::chooseExport()
{
    // Capture before opening the modal dialog; late pages cannot alter this CSV.
    const auto snapshot = displaySnapshot();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export native result snapshot"), {}, tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;
    QString error;
    if (!writeSnapshot(snapshot, path, &error)) statusLabel->setText(error);
    else statusLabel->setText(tr("Exported %1 frozen native samples.").arg(snapshot.value("samples").toArray().size()));
}
