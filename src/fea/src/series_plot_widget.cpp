#include "series_plot_widget.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QSet>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <functional>

namespace
{
constexpr int kMaximumTraces = 4;
constexpr int kMaximumDisplayedPoints = 1000;
constexpr int kMaximumTextLength = 512;
constexpr double kMinimumViewSpan = 1e-12;
constexpr double kMaximumViewSpan = 16.0;
constexpr double kMaximumViewCenter = 64.0;

bool boundedText(const QString &text, int maximum, bool required = false)
{
    return text.size() <= maximum && (!required || !text.trimmed().isEmpty());
}

bool validSha256(const QString &sha256)
{
    if (sha256.size() != 64) return false;
    for (const QChar character : sha256)
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9'))
              || (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
            return false;
    return true;
}

double safeScaleRatio(double value, double oldScale, double newScale)
{
    if (value == 0.0 || oldScale == newScale) return value;
    int valueExponent = 0, oldExponent = 0, newExponent = 0;
    const double mantissa = std::frexp(value, &valueExponent)
        * std::frexp(oldScale, &oldExponent) / std::frexp(newScale, &newExponent);
    const int exponent = valueExponent + oldExponent - newExponent;
    if (exponent > 8) return std::copysign(kMaximumViewCenter, mantissa);
    if (exponent < -1020) return 0.0;
    return std::clamp(std::ldexp(mantissa, exponent), -kMaximumViewCenter, kMaximumViewCenter);
}

double finiteProduct(double left, double right)
{
    if (left == 0.0 || right == 0.0) return 0.0;
    const double maximum = std::numeric_limits<double>::max();
    if (std::abs(left) > 1.0 && std::abs(right) > maximum / std::abs(left))
        return std::copysign(maximum, left * std::copysign(1.0, right));
    if (std::abs(right) > 1.0 && std::abs(left) > maximum / std::abs(right))
        return std::copysign(maximum, right * std::copysign(1.0, left));
    const double product = left * right;
    return std::isfinite(product) ? product : std::copysign(maximum, product);
}

QString axisValue(double normalizedValue, double scale)
{
    return QString::number(finiteProduct(normalizedValue, scale), 'g', 5);
}

QString axisTitle(const QString &label, const QString &unit)
{
    if (label.isEmpty()) return unit.isEmpty() ? QString() : QStringLiteral("[%1]").arg(unit);
    return unit.isEmpty() ? label : QStringLiteral("%1 [%2]").arg(label, unit);
}
}

class SeriesPlotCanvas : public QWidget
{
public:
    std::function<void(const QString &, quint64)> cursorObserved;
    std::function<void(quint64)> datasetPaintObserved;
    quint64 datasetGeneration = 0, paintedGeneration = 0;
    std::optional<QPointF> cursorPoint;
    explicit SeriesPlotCanvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("seriesPlotCanvas"));
        setAccessibleName(tr("Series plot canvas"));
        setMinimumSize(320, 240);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    void setDataset(const SeriesPlotWidget::Dataset *nextDataset, bool preserveView, quint64 generation)
    {
        const double oldXScale = xScale;
        const double oldYScale = yScale;
        dataset = nextDataset;
        datasetGeneration = generation;
        calculateScales();
        if (!preserveView || !hasView)
        {
            autoFit = true;
            fitBounds();
        }
        else if (autoFit)
        {
            fitBounds();
        }
        else
        {
            const double nextXMinimum = safeScaleRatio(xMinimum, oldXScale, xScale);
            const double nextXMaximum = safeScaleRatio(xMaximum, oldXScale, xScale);
            const double nextYMinimum = safeScaleRatio(yMinimum, oldYScale, yScale);
            const double nextYMaximum = safeScaleRatio(yMaximum, oldYScale, yScale);
            if (!setViewBounds(nextXMinimum, nextXMaximum, nextYMinimum, nextYMaximum))
            {
                autoFit = true;
                fitBounds();
            }
        }
        updateAccessibleDescription();
        update();
    }

    void fitToData()
    {
        autoFit = true;
        fitBounds();
        update();
    }

    bool setVisibleRange(SeriesPlotWidget::VisibleRange range)
    {
        const double normalizedXMinimum = range.xMinimum / xScale;
        const double normalizedXMaximum = range.xMaximum / xScale;
        const double normalizedYMinimum = range.yMinimum / yScale;
        const double normalizedYMaximum = range.yMaximum / yScale;
        return std::isfinite(normalizedXMinimum) && std::isfinite(normalizedXMaximum)
            && std::isfinite(normalizedYMinimum) && std::isfinite(normalizedYMaximum)
            && normalizedXMinimum < normalizedXMaximum && normalizedYMinimum < normalizedYMaximum
            && setNormalizedVisibleRange(normalizedXMinimum, normalizedXMaximum,
                                         normalizedYMinimum, normalizedYMaximum);
    }

    SeriesPlotWidget::VisibleRange visibleRange() const
    {
        return {finiteProduct(xMinimum, xScale), finiteProduct(xMaximum, xScale),
                finiteProduct(yMinimum, yScale), finiteProduct(yMaximum, yScale)};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor text = palette().color(QPalette::Text);
        const QColor grid = palette().color(QPalette::Midlight);
        const QRectF area = plotRect();

        if (!dataset)
        {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                             tr("No verified series dataset is displayed."));
            return;
        }

        painter.setPen(text);
        if (!dataset->title.isEmpty())
            painter.drawText(QRectF(8, 3, width() - 16, 22), Qt::AlignCenter,
                             QFontMetrics(font()).elidedText(dataset->title, Qt::ElideMiddle, qMax(1, width() - 24)));
        drawLegend(painter, text);

        const QString horizontalTitle = axisTitle(dataset->xLabel, dataset->xUnit);
        const QString verticalTitle = axisTitle(dataset->yLabel, dataset->yUnit);
        for (int tick = 0; tick <= 4; ++tick)
        {
            const double fraction = tick / 4.0;
            const double normalizedX = xMinimum + fraction * (xMaximum - xMinimum);
            const double normalizedY = yMaximum - fraction * (yMaximum - yMinimum);
            const qreal px = area.left() + fraction * area.width();
            const qreal py = area.top() + fraction * area.height();
            painter.setPen(QPen(grid, 1, Qt::DotLine));
            painter.drawLine(QPointF(px, area.top()), QPointF(px, area.bottom()));
            painter.drawLine(QPointF(area.left(), py), QPointF(area.right(), py));
            painter.setPen(text);
            painter.drawText(QRectF(32, py - 8, area.left() - 41, 16), Qt::AlignRight | Qt::AlignVCenter,
                             axisValue(normalizedY, yScale));
            painter.drawText(QRectF(px - 42, area.bottom() + 4, 84, 16), Qt::AlignHCenter | Qt::AlignVCenter,
                             axisValue(normalizedX, xScale));
        }
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(area);
        painter.setPen(text);
        painter.drawText(QRectF(area.left(), height() - 21, area.width(), 18), Qt::AlignCenter, horizontalTitle);
        painter.save();
        painter.translate(15, area.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-area.height() / 2, -10, area.height(), 20), Qt::AlignCenter, verticalTitle);
        painter.restore();

        const bool drewSamples = hasDisplayedPoints();
        const quint64 generation = datasetGeneration;
        if (drewSamples) drawTraces(painter, area);
        else
        {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(area, Qt::AlignCenter, tr("No displayed samples"));
        }
        if (cursorPoint && pointInView(*cursorPoint))
        {
            const auto point = screenPoint(normalizedPoint(*cursorPoint), area);
            painter.setPen(QPen(palette().color(QPalette::Text), 1, Qt::DashLine));
            painter.drawLine(QPointF(point.x(), area.top()), QPointF(point.x(), area.bottom()));
            painter.drawEllipse(point, 4, 4);
        }
        // Finish the actual trace painter before observers can inspect or replace
        // the dataset. Repaints, cursor movement and empty generations do not emit.
        const bool ended = painter.end();
        if (ended && drewSamples && generation != paintedGeneration)
        {
            paintedGeneration = generation;
            if (datasetPaintObserved) datasetPaintObserved(generation);
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const QRectF area = plotRect();
        if (!dataset || !area.contains(event->position()) || event->angleDelta().y() == 0)
        {
            event->ignore();
            return;
        }

        const double steps = event->angleDelta().y() / 120.0;
        const double factor = std::clamp(std::pow(1.18, -steps), 0.1, 10.0);
        const double xFraction = std::clamp((event->position().x() - area.left()) / area.width(), 0.0, 1.0);
        const double yFraction = std::clamp((area.bottom() - event->position().y()) / area.height(), 0.0, 1.0);
        const double xAnchor = xMinimum + xFraction * (xMaximum - xMinimum);
        const double yAnchor = yMinimum + yFraction * (yMaximum - yMinimum);
        const double nextXSpan = std::clamp((xMaximum - xMinimum) * factor, kMinimumViewSpan, kMaximumViewSpan);
        const double nextYSpan = std::clamp((yMaximum - yMinimum) * factor, kMinimumViewSpan, kMaximumViewSpan);
        const double nextXMinimum = xAnchor - xFraction * nextXSpan;
        const double nextXMaximum = nextXMinimum + nextXSpan;
        const double nextYMinimum = yAnchor - yFraction * nextYSpan;
        const double nextYMaximum = nextYMinimum + nextYSpan;
        if (!setViewBounds(nextXMinimum, nextXMaximum, nextYMinimum, nextYMaximum))
        {
            event->ignore();
            return;
        }
        autoFit = false;
        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && plotRect().contains(event->position()))
        {
            panning = true;
            panOrigin = event->position();
            panXMinimum = xMinimum; panXMaximum = xMaximum;
            panYMinimum = yMinimum; panYMaximum = yMaximum;
            setCursor(Qt::ClosedHandCursor);
            QToolTip::hideText();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QRectF area = plotRect();
        if (panning && (event->buttons() & Qt::LeftButton))
        {
            const double dx = -(event->position().x() - panOrigin.x()) / area.width()
                * (panXMaximum - panXMinimum);
            const double dy = (event->position().y() - panOrigin.y()) / area.height()
                * (panYMaximum - panYMinimum);
            if (!setViewBounds(panXMinimum + dx, panXMaximum + dx,
                               panYMinimum + dy, panYMaximum + dy))
            {
                event->ignore();
                return;
            }
            autoFit = false;
            update();
            event->accept();
            return;
        }

        if (area.contains(event->position()))
        {
            setCursor(Qt::OpenHandCursor);
            showNearestSample(event->position().toPoint());
        }
        else
        {
            unsetCursor();
            QToolTip::hideText();
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && panning)
        {
            panning = false;
            if (plotRect().contains(event->position())) setCursor(Qt::OpenHandCursor);
            else unsetCursor();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (!panning)
        {
            unsetCursor();
            QToolTip::hideText();
        }
        QWidget::leaveEvent(event);
    }

private:
    const SeriesPlotWidget::Dataset *dataset = nullptr;
    double xScale = 1.0;
    double yScale = 1.0;
    double xMinimum = -1.0;
    double xMaximum = 1.0;
    double yMinimum = -1.0;
    double yMaximum = 1.0;
    bool hasView = false;
    bool autoFit = true;
    bool panning = false;
    QPointF panOrigin;
    double panXMinimum = -1.0, panXMaximum = 1.0;
    double panYMinimum = -1.0, panYMaximum = 1.0;

    QRectF plotRect() const
    {
        const QFontMetrics metrics(font());
        qreal tickWidth = 0;
        for (int tick=0;tick<=4;++tick)
            tickWidth=qMax(tickWidth,qreal(metrics.horizontalAdvance(axisValue(
                yMaximum+(tick/4.0)*(yMinimum-yMaximum),yScale))));
        const qreal left=qMax(78.0,tickWidth+44.0);
        constexpr qreal right = 16.0, top = 52.0, bottom = 58.0;
        return QRectF(left, top, qMax(1.0, width() - left - right), qMax(1.0, height() - top - bottom));
    }

    void updateAccessibleDescription()
    {
        if (!dataset) return;
        QStringList descriptions;
        for (const auto &trace : dataset->traces)
            descriptions.append(QStringLiteral("%1, %2 points, source %3")
                                    .arg(trace.label.isEmpty() ? trace.id : trace.label)
                                    .arg(trace.points.size()).arg(trace.sourceId));
        setAccessibleDescription(QStringLiteral("%1. %2")
                                     .arg(dataset->title, descriptions.join(QStringLiteral("; "))));
    }

    void calculateScales()
    {
        xScale = 0.0; yScale = 0.0;
        if (dataset)
            for (const auto &trace : dataset->traces)
                for (const QPointF &point : trace.points)
                {
                    xScale = qMax(xScale, std::abs(point.x()));
                    yScale = qMax(yScale, std::abs(point.y()));
                }
        if (xScale == 0.0) xScale = 1.0;
        if (yScale == 0.0) yScale = 1.0;
    }

    bool hasDisplayedPoints() const
    {
        if (!dataset) return false;
        for (const auto &trace : dataset->traces)
            if (!trace.points.isEmpty()) return true;
        return false;
    }

    void fitBounds()
    {
        double xMin = std::numeric_limits<double>::infinity();
        double xMax = -xMin;
        double yMin = std::numeric_limits<double>::infinity();
        double yMax = -yMin;
        if (dataset)
            for (const auto &trace : dataset->traces)
                for (const QPointF &point : trace.points)
                {
                    const double x = point.x() / xScale;
                    const double y = point.y() / yScale;
                    xMin = qMin(xMin, x); xMax = qMax(xMax, x);
                    yMin = qMin(yMin, y); yMax = qMax(yMax, y);
                }

        if (!std::isfinite(xMin)) { xMin = -1.0; xMax = 1.0; }
        if (!std::isfinite(yMin)) { yMin = -1.0; yMax = 1.0; }
        const auto paddedRange = [](double minimum, double maximum) {
            const double span = maximum - minimum;
            if (span <= kMinimumViewSpan)
            {
                const double center = minimum + span * 0.5;
                return std::pair<double, double>{center - 0.5, center + 0.5};
            }
            const double padding = qMax(span * 0.06, kMinimumViewSpan);
            return std::pair<double, double>{minimum - padding, maximum + padding};
        };
        const auto xRange = paddedRange(xMin, xMax);
        const auto yRange = paddedRange(yMin, yMax);
        if (!setViewBounds(xRange.first, xRange.second, yRange.first, yRange.second))
        {
            xMinimum = -1.0; xMaximum = 1.0;
            yMinimum = -1.0; yMaximum = 1.0;
        }
        hasView = true;
    }

    bool setViewBounds(double nextXMinimum, double nextXMaximum,
                       double nextYMinimum, double nextYMaximum)
    {
        const auto clampRange = [](double minimum, double maximum, double &boundedMinimum, double &boundedMaximum) {
            if (!std::isfinite(minimum) || !std::isfinite(maximum)) return false;
            double span = maximum - minimum;
            if (!std::isfinite(span) || span <= 0.0) return false;
            span = std::clamp(span, kMinimumViewSpan, kMaximumViewSpan);
            double center = minimum + (maximum - minimum) * 0.5;
            if (!std::isfinite(center)) return false;
            center = std::clamp(center, -kMaximumViewCenter, kMaximumViewCenter);
            boundedMinimum = center - span * 0.5;
            boundedMaximum = center + span * 0.5;
            return std::isfinite(boundedMinimum) && std::isfinite(boundedMaximum);
        };
        double boundedXMinimum = 0.0, boundedXMaximum = 0.0;
        double boundedYMinimum = 0.0, boundedYMaximum = 0.0;
        if (!clampRange(nextXMinimum, nextXMaximum, boundedXMinimum, boundedXMaximum)
            || !clampRange(nextYMinimum, nextYMaximum, boundedYMinimum, boundedYMaximum))
            return false;
        xMinimum = boundedXMinimum;
        xMaximum = boundedXMaximum;
        yMinimum = boundedYMinimum;
        yMaximum = boundedYMaximum;
        return true;
    }

    bool setNormalizedVisibleRange(double nextXMinimum, double nextXMaximum,
                                   double nextYMinimum, double nextYMaximum)
    {
        const auto withinViewLimits = [](double minimum, double maximum) {
            if (!std::isfinite(minimum) || !std::isfinite(maximum)) return false;
            const double span = maximum - minimum;
            if (!std::isfinite(span) || span < kMinimumViewSpan || span > kMaximumViewSpan)
                return false;
            const double center = minimum + span * 0.5;
            return std::isfinite(center) && std::abs(center) <= kMaximumViewCenter;
        };
        if (!withinViewLimits(nextXMinimum, nextXMaximum)
            || !withinViewLimits(nextYMinimum, nextYMaximum)
            || !setViewBounds(nextXMinimum, nextXMaximum, nextYMinimum, nextYMaximum))
            return false;
        hasView = true;
        autoFit = false;
        update();
        return true;
    }

    QPointF normalizedPoint(const QPointF &point) const
    {
        return {point.x() / xScale, point.y() / yScale};
    }

    bool pointInView(const QPointF &point) const
    {
        const QPointF normalized = normalizedPoint(point);
        return normalized.x() >= xMinimum && normalized.x() <= xMaximum
            && normalized.y() >= yMinimum && normalized.y() <= yMaximum;
    }

    bool clipSegmentToView(const QPointF &from, const QPointF &to,
                           QPointF &clippedFrom, QPointF &clippedTo) const
    {
        const double dx = to.x() - from.x();
        const double dy = to.y() - from.y();
        double entering = 0.0;
        double leaving = 1.0;
        const auto clip = [&entering, &leaving](double p, double q) {
            if (p == 0.0) return q >= 0.0;
            const double ratio = q / p;
            if (p < 0.0)
            {
                if (ratio > leaving) return false;
                entering = qMax(entering, ratio);
            }
            else
            {
                if (ratio < entering) return false;
                leaving = qMin(leaving, ratio);
            }
            return true;
        };
        if (!clip(-dx, from.x() - xMinimum)
            || !clip(dx, xMaximum - from.x())
            || !clip(-dy, from.y() - yMinimum)
            || !clip(dy, yMaximum - from.y())
            || entering > leaving)
            return false;
        clippedFrom = {from.x() + entering * dx, from.y() + entering * dy};
        clippedTo = {from.x() + leaving * dx, from.y() + leaving * dy};
        return true;
    }

    QPointF screenPoint(const QPointF &normalizedPoint, const QRectF &area) const
    {
        const double xFraction = (normalizedPoint.x() - xMinimum) / (xMaximum - xMinimum);
        const double yFraction = (yMaximum - normalizedPoint.y()) / (yMaximum - yMinimum);
        return {area.left() + xFraction * area.width(), area.top() + yFraction * area.height()};
    }

    std::array<QColor, kMaximumTraces> traceColors() const
    {
        if (palette().color(QPalette::Base).lightness() < 128)
            return {QColor("#56b4e9"), QColor("#e69f00"), QColor("#009e73"), QColor("#cc79a7")};
        return {QColor("#0072b2"), QColor("#d55e00"), QColor("#009e73"), QColor("#882255")};
    }

    void drawLegend(QPainter &painter, const QColor &text) const
    {
        if (!dataset || dataset->traces.isEmpty()) return;
        const auto colors = traceColors();
        const int count = dataset->traces.size();
        const qreal itemWidth = qMax(1.0, (width() - 20.0) / count);
        for (int index = 0; index < count; ++index)
        {
            const qreal left = 10.0 + index * itemWidth;
            painter.setPen(QPen(colors[index], 2));
            painter.drawLine(QPointF(left, 38), QPointF(left + 15, 38));
            painter.setPen(text);
            const QString label = dataset->traces[index].label.isEmpty()
                ? dataset->traces[index].id : dataset->traces[index].label;
            const QString withUnit = dataset->traces[index].unit.isEmpty()
                ? label : QStringLiteral("%1 [%2]").arg(label, dataset->traces[index].unit);
            const int available = qMax(1, int(itemWidth) - 24);
            painter.drawText(QRectF(left + 20, 29, available, 18), Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(font()).elidedText(withUnit, Qt::ElideRight, available));
        }
    }

    void drawTraces(QPainter &painter, const QRectF &area) const
    {
        if (!dataset) return;
        const auto colors = traceColors();
        painter.save();
        painter.setClipRect(area);
        for (int traceIndex = 0; traceIndex < dataset->traces.size(); ++traceIndex)
        {
            const auto &trace = dataset->traces[traceIndex];
            if (trace.points.isEmpty()) continue;
            QPainterPath path;
            for (int pointIndex = 1; pointIndex < trace.points.size(); ++pointIndex)
            {
                const QPointF from = normalizedPoint(trace.points[pointIndex - 1]);
                const QPointF to = normalizedPoint(trace.points[pointIndex]);
                QPointF clippedFrom, clippedTo;
                if (!clipSegmentToView(from, to, clippedFrom, clippedTo)) continue;
                path.moveTo(screenPoint(clippedFrom, area));
                path.lineTo(screenPoint(clippedTo, area));
            }
            painter.setPen(QPen(colors[traceIndex], 1.8));
            painter.drawPath(path);
            if (trace.points.size() == 1)
            {
                if (pointInView(trace.points.first()))
                {
                    painter.setBrush(colors[traceIndex]);
                    painter.setPen(Qt::NoPen);
                    painter.drawEllipse(screenPoint(normalizedPoint(trace.points.first()), area), 3.5, 3.5);
                }
            }
            else if (trace.points.size() <= 64)
            {
                painter.setBrush(colors[traceIndex]);
                painter.setPen(Qt::NoPen);
                for (const QPointF &point : trace.points)
                    if (pointInView(point))
                        painter.drawEllipse(screenPoint(normalizedPoint(point), area), 2.5, 2.5);
            }
        }
        painter.restore();
    }

    void showNearestSample(const QPoint &cursorPosition)
    {
        if (!dataset) { QToolTip::hideText(); return; }
        const QRectF area = plotRect();
        double nearestDistance = std::numeric_limits<double>::infinity();
        int nearestTrace = -1, nearestPoint = -1;
        for (int traceIndex = 0; traceIndex < dataset->traces.size(); ++traceIndex)
        {
            const auto &trace = dataset->traces[traceIndex];
            for (int pointIndex = 0; pointIndex < trace.points.size(); ++pointIndex)
            {
                const QPointF &point = trace.points[pointIndex];
                if (!pointInView(point)) continue;
                const QPointF screen = screenPoint(normalizedPoint(point), area);
                const double dx = screen.x() - cursorPosition.x();
                const double dy = screen.y() - cursorPosition.y();
                const double distance = dx * dx + dy * dy;
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearestTrace = traceIndex;
                    nearestPoint = pointIndex;
                }
            }
        }
        if (nearestTrace < 0)
        {
            QToolTip::hideText();
            return;
        }

        const auto &trace = dataset->traces[nearestTrace];
        if (cursorObserved) cursorObserved(trace.id, trace.originalOrdinals[nearestPoint]);
        const QPointF &point = trace.points[nearestPoint];
        const QString ordinalName = dataset->viewKind == SeriesPlotWidget::ViewKind::Spectrum
            ? tr("Frequency bin") : tr("Raw sample index");
        const QString html = QStringLiteral("<b>")
            + (trace.label.isEmpty() ? trace.id : trace.label).toHtmlEscaped()
            + QStringLiteral("</b><br>x = ") + QString::number(point.x(), 'g', 10).toHtmlEscaped()
            + QStringLiteral(" ") + dataset->xUnit.toHtmlEscaped()
            + QStringLiteral("<br>y = ") + QString::number(point.y(), 'g', 10).toHtmlEscaped()
            + QStringLiteral(" ") + trace.unit.toHtmlEscaped()
            + QStringLiteral("<br>") + ordinalName.toHtmlEscaped()
            + QStringLiteral(" = ") + QString::number(trace.originalOrdinals[nearestPoint])
            + QStringLiteral("<br>source = ") + trace.sourceId.toHtmlEscaped()
            + QStringLiteral("<br>artifact SHA-256 = ") + trace.artifactSha256.toHtmlEscaped();
        QToolTip::showText(mapToGlobal(cursorPosition), html, this);
    }
};

SeriesPlotWidget::SeriesPlotWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("seriesPlotWidget"));
    setAccessibleName(tr("Verified series plot"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);
    auto *toolbar = new QHBoxLayout;

    holdCheck = new QCheckBox(tr("Hold display"), this);
    holdCheck->setObjectName(QStringLiteral("seriesPlotHold"));
    holdCheck->setAccessibleName(tr("Hold the displayed series dataset"));
    holdCheck->setToolTip(tr("Freeze the complete displayed dataset while newer updates wait. Releasing shows the latest update."));
    fitButton = new QPushButton(tr("Fit"), this);
    fitButton->setObjectName(QStringLiteral("seriesPlotFit"));
    fitButton->setAccessibleName(tr("Fit all displayed series samples"));
    exportButton = new QPushButton(tr("Export full series"), this);
    exportButton->setObjectName(QStringLiteral("seriesPlotExport"));
    exportButton->setAccessibleName(tr("Request export of the full raw series"));
    exportButton->setToolTip(tr("Available only when the host confirms a full raw-series export source."));
    exportButton->setEnabled(false);
    toolbar->addWidget(holdCheck);
    toolbar->addStretch(1);
    toolbar->addWidget(fitButton);
    toolbar->addWidget(exportButton);
    layout->addLayout(toolbar);

    canvas = new SeriesPlotCanvas(this);
    canvas->datasetPaintObserved = [this](quint64 generation) { emit datasetPainted(generation); };
    canvas->cursorObserved = [this](const QString &id, quint64 ordinal) { setCursor({id, ordinal}); };
    layout->addWidget(canvas, 1);
    connect(holdCheck, &QCheckBox::toggled, this, &SeriesPlotWidget::setDisplayHeld);
    connect(fitButton, &QPushButton::clicked, this, &SeriesPlotWidget::fitToData);
    connect(exportButton, &QPushButton::clicked, this, &SeriesPlotWidget::exportFullSeriesRequested);
}

bool SeriesPlotWidget::setDataset(const Dataset &nextDataset, QString *error)
{
    if (!validateDataset(nextDataset, error)) return false;
    if (error) error->clear();
    if (displayHeld)
    {
        pendingDataset = nextDataset;
        return true;
    }
    presentDataset(nextDataset);
    return true;
}

bool SeriesPlotWidget::setVisibleRange(VisibleRange range, QString *error)
{
    const auto reject = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!std::isfinite(range.xMinimum) || !std::isfinite(range.xMaximum)
        || !std::isfinite(range.yMinimum) || !std::isfinite(range.yMaximum)
        || range.xMinimum >= range.xMaximum || range.yMinimum >= range.yMaximum)
        return reject(QStringLiteral("Visible axis bounds must be finite and strictly increasing."));
    if (!datasetAvailable)
        return reject(QStringLiteral("A visible range requires a displayed dataset."));

    if (!canvas->setVisibleRange(range))
        return reject(QStringLiteral("The visible range exceeds the supported normalized view limits."));
    if (error) error->clear();
    return true;
}

SeriesPlotWidget::VisibleRange SeriesPlotWidget::visibleRange() const
{
    return canvas->visibleRange();
}

bool SeriesPlotWidget::setCursor(const Cursor &next, QString *error)
{
    for (const auto &trace : displayedDataset.traces)
        if (trace.id == next.traceId)
            for (qsizetype i = 0; i < trace.originalOrdinals.size(); ++i)
                if (trace.originalOrdinals[i] == next.ordinal)
                {
                    const bool changed = !displayedCursor || displayedCursor->traceId != next.traceId || displayedCursor->ordinal != next.ordinal;
                    displayedCursor = next; canvas->cursorPoint = trace.points[i]; canvas->update();
                    if (error) error->clear();
                    if (changed) emit cursorChanged(next.traceId, next.ordinal);
                    return true;
                }
    if (error) *error = QStringLiteral("The cursor does not identify a displayed raw sample or frequency bin.");
    return false;
}

void SeriesPlotWidget::setFullExportAvailable(bool available)
{
    fullExportAvailable = available;
    updateExportAvailability();
}

void SeriesPlotWidget::updateExportAvailability()
{
    const bool hasSamples = datasetAvailable && std::any_of(displayedDataset.traces.cbegin(),
        displayedDataset.traces.cend(), [](const Trace &trace) { return !trace.points.isEmpty(); });
    exportButton->setEnabled(fullExportAvailable && hasSamples);
}

void SeriesPlotWidget::setDisplayHeld(bool held)
{
    if (displayHeld == held) return;
    displayHeld = held;
    if (holdCheck->isChecked() != held)
    {
        const QSignalBlocker blocker(holdCheck);
        holdCheck->setChecked(held);
    }
    if (!held && pendingDataset)
    {
        Dataset latest = std::move(*pendingDataset);
        pendingDataset.reset();
        presentDataset(latest);
    }
    emit displayHoldChanged(held);
}

void SeriesPlotWidget::fitToData()
{
    canvas->fitToData();
}

bool SeriesPlotWidget::validateDataset(const Dataset &candidate, QString *error)
{
    const auto reject = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    switch (candidate.viewKind)
    {
    case ViewKind::Time:
    case ViewKind::Phase:
    case ViewKind::Spectrum:
        break;
    default:
        return reject(QStringLiteral("The plot view kind is invalid."));
    }
    if (!boundedText(candidate.title, kMaximumTextLength)
        || !boundedText(candidate.xLabel, kMaximumTextLength)
        || !boundedText(candidate.yLabel, kMaximumTextLength)
        || !boundedText(candidate.xUnit, kMaximumTextLength)
        || !boundedText(candidate.yUnit, kMaximumTextLength))
        return reject(QStringLiteral("A dataset title or axis label exceeds the display limit."));
    if (candidate.traces.size() > kMaximumTraces)
        return reject(QStringLiteral("A plot can display at most four traces."));

    QSet<QString> traceIds;
    for (const auto &trace : candidate.traces)
    {
        if (traceIds.contains(trace.id))
            return reject(QStringLiteral("Trace identities must be unique within a dataset."));
        traceIds.insert(trace.id);
        if (!boundedText(trace.id, kMaximumTextLength, true)
            || !boundedText(trace.label, kMaximumTextLength)
            || !boundedText(trace.unit, kMaximumTextLength)
            || !boundedText(trace.sourceId, kMaximumTextLength, true))
            return reject(QStringLiteral("A trace identity, label, unit, or source ID is invalid or too long."));
        if (trace.unit != candidate.yUnit)
            return reject(QStringLiteral("All traces must use the dataset y-axis unit."));
        if (!validSha256(trace.artifactSha256))
            return reject(QStringLiteral("A trace artifact SHA-256 must contain 64 lowercase hexadecimal characters."));
        if (trace.points.size() > kMaximumDisplayedPoints)
            return reject(QStringLiteral("A trace can display at most 1000 samples."));
        if (trace.originalOrdinals.size() != trace.points.size())
            return reject(QStringLiteral("Each displayed sample must have one original ordinal."));
        QSet<quint64> ordinals;
        double previousX = 0.0;
        for (int index = 0; index < trace.points.size(); ++index)
        {
            const QPointF &point = trace.points[index];
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                return reject(QStringLiteral("Displayed sample coordinates must be finite."));
            if (ordinals.contains(trace.originalOrdinals[index]))
                return reject(QStringLiteral("Original sample ordinals must be unique within each trace."));
            ordinals.insert(trace.originalOrdinals[index]);
            if (candidate.viewKind != ViewKind::Phase && index > 0 && point.x() <= previousX)
                return reject(QStringLiteral("Time and spectrum samples must have strictly increasing x values."));
            previousX = point.x();
        }
    }
    return true;
}

bool SeriesPlotWidget::samePlotIdentity(const Dataset &left, const Dataset &right)
{
    if (left.viewKind != right.viewKind || left.xUnit != right.xUnit || left.yUnit != right.yUnit
        || left.traces.size() != right.traces.size())
        return false;
    for (int index = 0; index < left.traces.size(); ++index)
        if (left.traces[index].sourceId != right.traces[index].sourceId
            || left.traces[index].id != right.traces[index].id) return false;
    return true;
}

void SeriesPlotWidget::presentDataset(const Dataset &nextDataset)
{
    const bool preserveView = datasetAvailable && samePlotIdentity(displayedDataset, nextDataset);
    displayedDataset = nextDataset;
    datasetAvailable = true;
    ++displayedGeneration;
    canvas->setDataset(&displayedDataset, preserveView, displayedGeneration);
    if (!preserveView || (displayedCursor && !setCursor(*displayedCursor)))
    {
        displayedCursor.reset(); canvas->cursorPoint.reset(); canvas->update();
    }
    updateExportAvailability();
}
