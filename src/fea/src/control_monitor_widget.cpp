#include "control_monitor_widget.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace {
double channelValue(const QJsonObject &sample, const QString &group, const QString &id, const QString &field)
{
    for (const auto &entry : sample.value(group).toArray())
    {
        const auto item = entry.toObject();
        if (item.value("id").toString() == id && item.value(field).isDouble())
            return item.value(field).toDouble();
    }
    return std::numeric_limits<double>::quiet_NaN();
}
QString csvCell(QString value)
{
    if (!value.isEmpty() && QStringLiteral("=+-@").contains(value.front())) value.prepend('\'');
    value.replace('"', "\"\"");
    return '"' + value + '"';
}
}

class ControlTraceCanvas : public QWidget
{
public:
    QJsonArray samples;
    QString sensorId, sensorUnit, actuatorId;
    double windowSeconds = 0;
    explicit ControlTraceCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName("controlTraceCanvas");
        setAccessibleName(tr("Accepted native sensor and actuator traces"));
        setMinimumSize(310, 180); setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
protected:
    QPoint mouse{-1, -1};
    void mouseMoveEvent(QMouseEvent *event) override { mouse = event->position().toPoint(); update(); }
    void leaveEvent(QEvent *) override { mouse = {-1, -1}; update(); }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        painter.setRenderHint(QPainter::Antialiasing);
        if (samples.isEmpty())
        {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(rect().adjusted(16, 12, -16, -12), Qt::AlignCenter | Qt::TextWordWrap,
                tr("Run an active control experiment to see accepted FE steps here.\nThe chart updates as the solver advances."));
            return;
        }
        const double end = samples.last().toObject().value("timeS").toDouble();
        const double begin = samples.first().toObject().value("timeS").toDouble();
        const double left = windowSeconds > 0 ? qMax(begin, end - windowSeconds) : begin;
        const double right = end > left ? end : left + qMax(std::abs(left) * 1e-6, 1e-6);
        const int gap = 36;
        const int h = qMax(40, (height() - 90) / 2);
        drawTrace(painter, QRectF(82, 20, qMax(10, width() - 105), h), left, right,
                  "sensors", sensorId, "value", sensorId + " [" + sensorUnit + "]", QColor(30, 130, 220));
        drawTrace(painter, QRectF(82, 20 + h + gap, qMax(10, width() - 105), h), left, right,
                  "actuators", actuatorId, "forceN", actuatorId + tr(" · applied force [N]"), QColor(225, 132, 35));
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(82, height() - 16, width() - 105, 16), Qt::AlignCenter,
                         tr("Accepted simulation time [s]"));
    }
    void drawTrace(QPainter &p, const QRectF &area, double left, double right,
                   const QString &group, const QString &id, const QString &field,
                   const QString &label, const QColor &color)
    {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        QList<QPointF> points;
        for (const auto &entry : samples)
        {
            const auto sample = entry.toObject();
            const double t = sample.value("timeS").toDouble();
            const double y = channelValue(sample, group, id, field);
            if (t < left || !std::isfinite(y)) continue;
            points.append({t, y}); lo = qMin(lo, y); hi = qMax(hi, y);
        }
        p.setPen(palette().color(QPalette::Mid)); p.drawRect(area);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRectF(area.left(), area.top() - 18, area.width(), 18), Qt::AlignLeft, label);
        if (points.isEmpty()) { p.drawText(area, Qt::AlignCenter, tr("Channel unavailable")); return; }
        const double padding = qMax((hi - lo) * .08, qMax(std::abs(hi) * .01, 1e-12));
        lo -= padding; hi += padding;
        for (int i = 0; i < 3; ++i)
        {
            const double y = area.top() + i * area.height() / 2;
            p.setPen(palette().color(QPalette::Midlight)); p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRectF(0, y - 8, 75, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(hi - i * (hi - lo) / 2, 'g', 4));
        }
        QPainterPath path;
        for (int i = 0; i < points.size(); ++i)
        {
            const QPointF point(area.left() + (points[i].x() - left) / (right - left) * area.width(),
                                area.bottom() - (points[i].y() - lo) / (hi - lo) * area.height());
            if (!i) path.moveTo(point); else path.lineTo(point);
            if (points.size() == 1) { p.setPen(QPen(color, 4)); p.drawPoint(point); }
        }
        p.setPen(QPen(color, 1.7)); p.drawPath(path);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRectF(area.left(), area.bottom() + 1, area.width() / 2, 16), Qt::AlignLeft, QString::number(left, 'g', 5));
        p.drawText(QRectF(area.center().x(), area.bottom() + 1, area.width() / 2, 16), Qt::AlignRight, QString::number(right, 'g', 5));
        if (area.contains(mouse))
        {
            const double time = left + (mouse.x() - area.left()) / area.width() * (right - left);
            auto nearest = points.first();
            for (const auto &point : points) if (std::abs(point.x() - time) < std::abs(nearest.x() - time)) nearest = point;
            const double x = area.left() + (nearest.x() - left) / (right - left) * area.width();
            p.setPen(QPen(color, 1, Qt::DashLine)); p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            const QString reading = tr("t=%1 s  y=%2").arg(nearest.x(), 0, 'g', 6).arg(nearest.y(), 0, 'g', 6);
            QRectF box(area.left() + 5, area.top() + 5, qMin(area.width() - 10, 245.0), 22);
            p.fillRect(box, palette().base()); p.setPen(palette().color(QPalette::Text)); p.drawText(box, Qt::AlignCenter, reading);
        }
    }
};

ControlMonitorWidget::ControlMonitorWidget(WorkflowProcess *transport, QWidget *parent)
    : QWidget(parent), service(transport)
{
    setObjectName("controlMonitor");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(6, 4, 6, 4);
    auto *toolbar = new QHBoxLayout;
    sensor = new QComboBox; sensor->setObjectName("controlSensor"); sensor->setAccessibleName(tr("Sensor channel"));
    actuator = new QComboBox; actuator->setObjectName("controlActuator"); actuator->setAccessibleName(tr("Actuator channel"));
    timeRange = new QComboBox; timeRange->setObjectName("controlTimeRange"); timeRange->setAccessibleName(tr("Visible simulation time"));
    timeRange->addItem(tr("All time"), 0.0); timeRange->addItem(tr("Last 0.5 s"), .5);
    timeRange->addItem(tr("Last 1 s"), 1.0); timeRange->addItem(tr("Last 5 s"), 5.0);
    hold = new QPushButton(tr("Hold display")); hold->setObjectName("controlHold"); hold->setCheckable(true);
    hold->setToolTip(tr("Freeze this view while the solver and data collection continue."));
    auto *exportButton = new QPushButton(tr("Export CSV")); exportButton->setObjectName("controlExport");
    exportButton->setToolTip(tr("Export all buffered points for the selected channels; while held, export the frozen snapshot."));
    toolbar->addWidget(sensor, 1); toolbar->addWidget(actuator, 1); toolbar->addWidget(timeRange);
    toolbar->addWidget(hold); toolbar->addWidget(exportButton); layout->addLayout(toolbar);
    canvas = new ControlTraceCanvas(this); layout->addWidget(canvas, 1);
    status = new QLabel(tr("Select a control calculation. Native accepted-step data only."));
    status->setObjectName("controlTelemetryStatus"); status->setWordWrap(true); layout->addWidget(status);
    for (auto *combo : {sensor, actuator, timeRange}) connect(combo, &QComboBox::currentIndexChanged, this, &ControlMonitorWidget::refreshPlot);
    connect(hold, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) heldSamples = samples; else heldSamples = {};
        hold->setText(checked ? tr("Resume display") : tr("Hold display")); refreshPlot();
    });
    connect(exportButton, &QPushButton::clicked, this, &ControlMonitorWidget::exportCsv);
    pollTimer.setInterval(300); connect(&pollTimer, &QTimer::timeout, this, &ControlMonitorWidget::poll); pollTimer.start();
    connect(service, &WorkflowProcess::replyReceived, this, [this](const QString &id, const QString &operation, const QJsonObject &result) {
        if (id != pendingRequest || operation != "telemetry.query") return;
        pendingRequest.clear(); receive(result);
    });
    connect(service, &WorkflowProcess::requestFailed, this, [this](const QString &id, const QString &, const QString &, const QString &message) {
        if (id != pendingRequest) return;
        pendingRequest.clear(); faulted = true; status->setText(tr("Telemetry unavailable: %1").arg(message));
    });
    connect(service, &WorkflowProcess::readyChanged, this, [this](bool ready) {
        if (!ready) { pendingRequest.clear(); status->setText(tr("Workspace disconnected · retained trace is historical.")); }
    });
}

void ControlMonitorWidget::setJob(const QString &id, const QString &state)
{
    jobState = state;
    if (jobId == id) { refreshPlot(); return; }
    jobId = id; pendingRequest.clear(); runId.clear(); availabilityMessage.clear(); cursor = 0; samples = {}; heldSamples = {}; faulted = false;
    hold->setChecked(false); sensor->clear(); actuator->clear(); refreshPlot(); poll();
}

void ControlMonitorWidget::poll()
{
    if (!service->isReady() || jobId.isEmpty() || !pendingRequest.isEmpty() || faulted) return;
    pendingRequest = service->request("telemetry.query", {{"jobId", jobId}, {"mode", "active"},
        {"afterSeq", double(cursor)}, {"limit", 256}});
}

void ControlMonitorWidget::receive(const QJsonObject &result)
{
    if (result.value("jobId").toString() != jobId || result.value("mode").toString() != "active"
        || result.value("source").toString() != "native-accepted-control") return;
    const QString returnedRun = result.value("runId").toString();
    if (!runId.isEmpty() && !returnedRun.isEmpty() && runId != returnedRun)
    { faulted = true; status->setText(tr("Native run changed. Select the task again to reload its trace.")); return; }
    if (!returnedRun.isEmpty()) runId = returnedRun;
    const double next = result.value("nextSeq").toDouble(-1);
    const auto failPage = [this] {
        faulted = true; status->setText(tr("Telemetry page is inconsistent; the last valid trace is retained."));
    };
    if (!result.value("nextSeq").isDouble() || !std::isfinite(next) || next < double(cursor)
        || next > 9007199254740991.0 || std::floor(next) != next || !result.value("samples").isArray())
    { failPage(); return; }
    const auto page = result.value("samples").toArray();
    if (page.size() > 512) { failPage(); return; }
    double previousSeq = double(cursor);
    double previousTime = samples.isEmpty() ? -1.0 : samples.last().toObject().value("timeS").toDouble();
    // Validate the entire page before changing the cursor or the displayed data.
    for (const auto &entry : page)
    {
        const auto sample = entry.toObject();
        const double sequence = sample.value("seq").toDouble();
        const double time = sample.value("timeS").toDouble();
        if (!sample.value("seq").isDouble() || !std::isfinite(sequence) || std::floor(sequence) != sequence
            || sequence <= previousSeq || sequence > next || !sample.value("timeS").isDouble()
            || !std::isfinite(time) || time < 0 || time <= previousTime || time > 1e100
            || !sample.value("sensors").isArray() || !sample.value("actuators").isArray())
        { failPage(); return; }
        previousSeq = sequence; previousTime = time;
    }
    for (const auto &entry : page)
    {
        const auto sample = entry.toObject();
        samples.append(sample);
        if (samples.size() > 5000) samples.removeFirst();
        const QSignalBlocker sensorBlock(sensor), actuatorBlock(actuator);
        for (const auto &value : sample.value("sensors").toArray())
        {
            const auto channel = value.toObject(); const QString id = channel.value("id").toString();
            if (sensor->findData(id) < 0)
            {
                sensor->addItem(id + " [" + channel.value("unit").toString() + "]", id);
                sensor->setItemData(sensor->count() - 1, channel.value("unit").toString(), Qt::UserRole + 1);
            }
        }
        for (const auto &value : sample.value("actuators").toArray())
        {
            const QString id = value.toObject().value("id").toString();
            if (actuator->findData(id) < 0) actuator->addItem(id, id);
        }
    }
    cursor = quint64(next); jobState = result.value("state").toString();
    availabilityMessage = result.value("message").toString(tr("Waiting for accepted native control steps."));
    refreshPlot();
    emit samplesUpdated(jobId, samples.size(), jobState);
    if (result.value("hasMore").toBool()) QTimer::singleShot(0, this, &ControlMonitorWidget::poll);
}

void ControlMonitorWidget::refreshPlot()
{
    canvas->samples = hold->isChecked() ? heldSamples : samples;
    canvas->sensorId = sensor->currentData().toString(); canvas->sensorUnit = sensor->currentData(Qt::UserRole + 1).toString();
    canvas->actuatorId = actuator->currentData().toString(); canvas->windowSeconds = timeRange->currentData().toDouble(); canvas->update();
    if (!service->isReady() && !samples.isEmpty())
        status->setText(tr("Workspace disconnected · retained trace is historical."));
    else if (!faulted && !jobId.isEmpty() && samples.isEmpty())
        status->setText(tr("%1 · %2").arg(jobState, availabilityMessage.isEmpty()
            ? tr("Waiting for accepted native control steps.") : availabilityMessage));
    else if (!faulted)
        status->setText(jobId.isEmpty() ? tr("Select a control calculation. Native accepted-step data only.")
            : tr("%1 · native FE feedback · %2 steps · t = %3 s%4")
                .arg(jobState).arg(samples.size()).arg(samples.isEmpty() ? 0.0 : samples.last().toObject().value("timeS").toDouble(), 0, 'g', 7)
                .arg(hold->isChecked() ? tr(" · DISPLAY HELD at %1 s (%2 steps); collection continues")
                    .arg(heldSamples.isEmpty() ? 0.0 : heldSamples.last().toObject().value("timeS").toDouble(), 0, 'g', 7)
                    .arg(heldSamples.size()) : QString()));
}

void ControlMonitorWidget::exportCsv()
{
    const QJsonArray exported = hold->isChecked() ? heldSamples : samples;
    if (exported.isEmpty()) return;
    const QString exportJob = jobId, exportRun = runId;
    const QString sensorId = sensor->currentData().toString(), sensorUnit = sensor->currentData(Qt::UserRole + 1).toString();
    const QString actuatorId = actuator->currentData().toString();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export native control samples"), {}, tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;
    QByteArray data = "job_id,run_id,event_seq,time_s,interval_end_s,sensor_id,sensor_unit,sensor_value,actuator_id,applied_force_n,power_w,work_j\n";
    for (const auto &entry : exported)
    {
        const auto sample = entry.toObject(); QStringList row{csvCell(exportJob), csvCell(exportRun), QString::number(sample.value("seq").toDouble(), 'f', 0),
            QString::number(sample.value("timeS").toDouble(), 'g', 17),
            sample.contains("nextTimeS") ? QString::number(sample.value("nextTimeS").toDouble(), 'g', 17) : QString(),
            csvCell(sensorId), csvCell(sensorUnit)};
        const auto add = [&](const QString &group, const QString &id, const QString &field) {
            const double value = channelValue(sample, group, id, field);
            row.append(std::isfinite(value) ? QString::number(value, 'g', 17) : QString());
        };
        add("sensors", sensorId, "value"); row.append(csvCell(actuatorId));
        for (const auto &field : {QString("forceN"), QString("powerW"), QString("workJ")}) add("actuators", actuatorId, field);
        data += row.join(',').toUtf8() + '\n';
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        status->setText(tr("The trace could not be exported."));
    else status->setText(tr("Exported %1 native samples for the selected channels.").arg(exported.size()));
}
