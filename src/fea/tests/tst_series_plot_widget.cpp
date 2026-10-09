#include "series_plot_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QToolTip>
#include <QWheelEvent>

#include <limits>

namespace
{
SeriesPlotWidget::Dataset sampleDataset(const QString &sourceId = QStringLiteral("source-one"),
                                        const QString &title = QStringLiteral("Series"))
{
    SeriesPlotWidget::Trace trace;
    trace.id = QStringLiteral("trace-one");
    trace.label = QStringLiteral("Response");
    trace.unit = QStringLiteral("m");
    trace.sourceId = sourceId;
    trace.artifactSha256 = QString(64, QLatin1Char('a'));
    trace.points = {{0.0, 0.0}, {1.0, 1.0}, {2.0, 12.0}};
    trace.originalOrdinals = {4, 9, 21};

    SeriesPlotWidget::Dataset dataset;
    dataset.viewKind = SeriesPlotWidget::ViewKind::Time;
    dataset.title = title;
    dataset.xLabel = QStringLiteral("Time");
    dataset.xUnit = QStringLiteral("s");
    dataset.yLabel = QStringLiteral("Displacement");
    dataset.yUnit = QStringLiteral("m");
    dataset.traces = {trace};
    return dataset;
}

QWidget *plotCanvas(SeriesPlotWidget &widget)
{
    return widget.findChild<QWidget *>(QStringLiteral("seriesPlotCanvas"));
}

QImage renderCanvas(QWidget *canvas)
{
    QImage image(canvas->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    canvas->render(&image);
    return image;
}

bool samePoints(const QVector<QPointF> &left, const QVector<QPointF> &right)
{
    return left == right;
}
}

class TestSeriesPlotWidget : public QObject
{
    Q_OBJECT
private slots:
    void rejectsInvalidDatasetWithoutChangingDisplay();
    void phaseViewAllowsNonmonotoneX();
    void wheelDragFitAndHoverUseDisplayedSamples();
    void holdKeepsOnlyLatestPendingDataset();
    void fullExportButtonNeedsHostAvailability();
    void hugeFiniteValuesRenderWithoutChangingSourceSamples();
    void visibleRangeUsesRawUnitsAndRejectsUnsupportedBounds();
    void segmentsAreClippedBeforeProjection();
};

void TestSeriesPlotWidget::rejectsInvalidDatasetWithoutChangingDisplay()
{
    SeriesPlotWidget widget;
    const auto initial = sampleDataset();
    QString error;
    QVERIFY2(widget.setDataset(initial, &error), qPrintable(error));
    const QVector<QPointF> originalPoints = widget.dataset().traces.first().points;
    const QVector<quint64> originalOrdinals = widget.dataset().traces.first().originalOrdinals;

    auto rejectUnchanged = [&](const SeriesPlotWidget::Dataset &invalid) {
        QVERIFY(!widget.setDataset(invalid, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(widget.dataset().title, initial.title);
        QCOMPARE(widget.dataset().traces.first().sourceId, initial.traces.first().sourceId);
        QVERIFY(samePoints(widget.dataset().traces.first().points, originalPoints));
        QVERIFY(widget.dataset().traces.first().originalOrdinals == originalOrdinals);
    };

    auto invalid = sampleDataset(QStringLiteral("invalid-nan"), QStringLiteral("Invalid"));
    invalid.traces[0].points[1].setY(std::numeric_limits<double>::quiet_NaN());
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-units"), QStringLiteral("Invalid"));
    auto mixed = invalid.traces.first();
    mixed.id = QStringLiteral("second-trace");
    mixed.label = QStringLiteral("Mixed unit");
    mixed.unit = QStringLiteral("mm");
    mixed.sourceId = QStringLiteral("source-two");
    invalid.traces.append(mixed);
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-hash"), QStringLiteral("Invalid"));
    invalid.traces[0].artifactSha256 = QString(64, QLatin1Char('A'));
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-ordinal-count"), QStringLiteral("Invalid"));
    invalid.traces[0].originalOrdinals.removeLast();
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-duplicate-ordinal"), QStringLiteral("Invalid"));
    invalid.traces[0].originalOrdinals[2] = invalid.traces[0].originalOrdinals[1];
    rejectUnchanged(invalid);

    invalid = sampleDataset();
    invalid.traces.append(invalid.traces.first());
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-time-order"), QStringLiteral("Invalid"));
    invalid.traces[0].points[2].setX(0.5);
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-trace-count"), QStringLiteral("Invalid"));
    while (invalid.traces.size() < 5)
    {
        auto extra = invalid.traces.first();
        extra.id += QString::number(invalid.traces.size());
        extra.sourceId += QString::number(invalid.traces.size());
        invalid.traces.append(extra);
    }
    rejectUnchanged(invalid);

    invalid = sampleDataset(QStringLiteral("invalid-point-count"), QStringLiteral("Invalid"));
    invalid.traces[0].points.clear();
    invalid.traces[0].originalOrdinals.clear();
    for (quint64 index = 0; index <= 1000; ++index)
    {
        invalid.traces[0].points.append(QPointF(double(index), double(index)));
        invalid.traces[0].originalOrdinals.append(index);
    }
    rejectUnchanged(invalid);
}

void TestSeriesPlotWidget::phaseViewAllowsNonmonotoneX()
{
    SeriesPlotWidget widget;
    auto dataset = sampleDataset();
    dataset.viewKind = SeriesPlotWidget::ViewKind::Phase;
    dataset.xLabel = QStringLiteral("Velocity");
    dataset.xUnit = QStringLiteral("m/s");
    dataset.traces[0].points = {{0.0, 0.0}, {1.0, 1.0}, {0.5, 2.0}};
    QString error;
    QVERIFY2(widget.setDataset(dataset, &error), qPrintable(error));
    QCOMPARE(widget.dataset().traces.first().points[2].x(), 0.5);
}

void TestSeriesPlotWidget::wheelDragFitAndHoverUseDisplayedSamples()
{
    SeriesPlotWidget widget;
    auto dataset = sampleDataset();
    QString error;
    QVERIFY2(widget.setDataset(dataset, &error), qPrintable(error));
    widget.resize(760, 480);
    widget.show();
    QApplication::processEvents();
    QWidget *canvas = plotCanvas(widget);
    QVERIFY(canvas);
    const auto originalPoints = widget.dataset().traces.first().points;
    const auto originalOrdinals = widget.dataset().traces.first().originalOrdinals;

    const QImage fitted = renderCanvas(canvas);
    const QPoint center(canvas->width() / 2, canvas->height() / 2);
    QWheelEvent wheel(QPointF(center), QPointF(canvas->mapToGlobal(center)), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(canvas, &wheel);
    QApplication::processEvents();
    const QImage zoomed = renderCanvas(canvas);
    QVERIFY(zoomed != fitted);
    QVERIFY2(widget.setDataset(dataset, &error), qPrintable(error));
    QApplication::processEvents();
    QVERIFY(renderCanvas(canvas) == zoomed);

    auto anotherChannel = dataset;
    anotherChannel.traces[0].id = QStringLiteral("another-physical-channel");
    QVERIFY2(widget.setDataset(anotherChannel, &error), qPrintable(error));
    QApplication::processEvents();
    QVERIFY(renderCanvas(canvas) == fitted);
    QVERIFY2(widget.setDataset(dataset, &error), qPrintable(error));
    QApplication::sendEvent(canvas, &wheel);
    QApplication::processEvents();
    QVERIFY(renderCanvas(canvas) == zoomed);

    const QPoint moved(center.x() + 35, center.y() + 20);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(center), QPointF(canvas->mapToGlobal(center)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(moved), QPointF(canvas->mapToGlobal(moved)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(moved), QPointF(canvas->mapToGlobal(moved)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    QApplication::processEvents();
    const QImage panned = renderCanvas(canvas);
    QVERIFY(panned != zoomed);

    auto *fit = widget.findChild<QPushButton *>(QStringLiteral("seriesPlotFit"));
    QVERIFY(fit);
    QTest::mouseClick(fit, Qt::LeftButton);
    QApplication::processEvents();
    QVERIFY(renderCanvas(canvas) == fitted);
    QVERIFY(samePoints(widget.dataset().traces.first().points, originalPoints));
    QVERIFY(widget.dataset().traces.first().originalOrdinals == originalOrdinals);

    auto newSource = dataset;
    newSource.traces[0].sourceId = QStringLiteral("source-after-fit");
    QVERIFY2(widget.setDataset(newSource, &error), qPrintable(error));
    QApplication::processEvents();
    QVERIFY(renderCanvas(canvas) == fitted);

    auto tooltipDataset = sampleDataset(QStringLiteral("<src>&"), QStringLiteral("Tooltip series"));
    tooltipDataset.traces[0].label = QStringLiteral("<trace>");
    tooltipDataset.traces[0].points = {{3.5, -2.25}};
    tooltipDataset.traces[0].originalOrdinals = {73};
    QVERIFY2(widget.setDataset(tooltipDataset, &error), qPrintable(error));
    QTest::mouseMove(canvas, QPoint(canvas->width() / 2, canvas->height() / 2));
    QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("x = 3.5 s")));
    QVERIFY(QToolTip::text().contains(QStringLiteral("y = -2.25 m")));
    QVERIFY(QToolTip::text().contains(QStringLiteral("Raw sample index = 73")));
    QVERIFY(QToolTip::text().contains(QStringLiteral("&lt;src&gt;&amp;")));
    QVERIFY(QToolTip::text().contains(QString(64, QLatin1Char('a'))));

    tooltipDataset.viewKind = SeriesPlotWidget::ViewKind::Spectrum;
    tooltipDataset.xLabel = QStringLiteral("Frequency");
    tooltipDataset.xUnit = QStringLiteral("Hz");
    tooltipDataset.traces[0].originalOrdinals = {3};
    QVERIFY2(widget.setDataset(tooltipDataset, &error), qPrintable(error));
    QTest::mouseMove(canvas, QPoint(canvas->width() / 2 + 1, canvas->height() / 2 + 1));
    QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("Frequency bin = 3")));
    QVERIFY(!QToolTip::text().contains(QStringLiteral("Original record")));
}

void TestSeriesPlotWidget::holdKeepsOnlyLatestPendingDataset()
{
    SeriesPlotWidget widget;
    QString error;
    const auto first = sampleDataset(QStringLiteral("source-one"), QStringLiteral("First"));
    const auto second = sampleDataset(QStringLiteral("source-two"), QStringLiteral("Second"));
    const auto latest = sampleDataset(QStringLiteral("source-three"), QStringLiteral("Latest"));
    QVERIFY2(widget.setDataset(first, &error), qPrintable(error));
    widget.show();
    QApplication::processEvents();
    auto *hold = widget.findChild<QCheckBox *>(QStringLiteral("seriesPlotHold"));
    QVERIFY(hold);
    QTest::mouseClick(hold, Qt::LeftButton);
    QVERIFY(widget.isDisplayHeld());

    QVERIFY2(widget.setDataset(second, &error), qPrintable(error));
    QVERIFY2(widget.setDataset(latest, &error), qPrintable(error));
    QCOMPARE(widget.dataset().title, first.title);
    QCOMPARE(widget.dataset().traces.first().sourceId, first.traces.first().sourceId);

    auto invalid = latest;
    invalid.traces[0].artifactSha256 = QStringLiteral("bad-hash");
    QVERIFY(!widget.setDataset(invalid, &error));
    QCOMPARE(widget.dataset().title, first.title);

    QTest::mouseClick(hold, Qt::LeftButton);
    QVERIFY(!widget.isDisplayHeld());
    QCOMPARE(widget.dataset().title, latest.title);
    QCOMPARE(widget.dataset().traces.first().sourceId, latest.traces.first().sourceId);
    QVERIFY(samePoints(widget.dataset().traces.first().points, latest.traces.first().points));
}

void TestSeriesPlotWidget::fullExportButtonNeedsHostAvailability()
{
    SeriesPlotWidget widget;
    widget.show();
    QApplication::processEvents();
    auto *exportButton = widget.findChild<QPushButton *>(QStringLiteral("seriesPlotExport"));
    QVERIFY(exportButton);
    QVERIFY(!exportButton->isEnabled());
    QSignalSpy requests(&widget, &SeriesPlotWidget::exportFullSeriesRequested);
    QVERIFY(requests.isValid());
    widget.setFullExportAvailable(true);
    QVERIFY(!exportButton->isEnabled());
    QString error;
    QVERIFY2(widget.setDataset(sampleDataset(), &error), qPrintable(error));
    QVERIFY(exportButton->isEnabled());
    QTest::mouseClick(exportButton, Qt::LeftButton);
    QCOMPARE(requests.count(), 1);
    widget.setFullExportAvailable(false);
    QVERIFY(!exportButton->isEnabled());
}

void TestSeriesPlotWidget::hugeFiniteValuesRenderWithoutChangingSourceSamples()
{
    SeriesPlotWidget widget;
    auto dataset = sampleDataset();
    dataset.traces[0].points = {{-1e308, -1e308}, {0.0, 0.0}, {1e308, 1e308}};
    dataset.traces[0].originalOrdinals = {101, 208, 409};
    const auto originalPoints = dataset.traces[0].points;
    const auto originalOrdinals = dataset.traces[0].originalOrdinals;
    QString error;
    QVERIFY2(widget.setDataset(dataset, &error), qPrintable(error));
    widget.resize(720, 440);
    widget.show();
    QApplication::processEvents();

    QWidget *canvas = plotCanvas(widget);
    QVERIFY(canvas);
    const QImage image = renderCanvas(canvas);
    QVERIFY(!image.isNull());
    QImage blank(image.size(), QImage::Format_ARGB32_Premultiplied);
    blank.fill(canvas->palette().color(QPalette::Base));
    QVERIFY(image != blank);
    QVERIFY(samePoints(widget.dataset().traces.first().points, originalPoints));
    QVERIFY(widget.dataset().traces.first().originalOrdinals == originalOrdinals);
}

void TestSeriesPlotWidget::visibleRangeUsesRawUnitsAndRejectsUnsupportedBounds()
{
    SeriesPlotWidget widget;
    QString error;
    QVERIFY2(widget.setDataset(sampleDataset(), &error), qPrintable(error));

    const SeriesPlotWidget::VisibleRange requested{0.2, 1.2, 1.2, 2.4};
    QVERIFY2(widget.setVisibleRange(requested, &error), qPrintable(error));
    const auto actual = widget.visibleRange();
    QVERIFY(qAbs(actual.xMinimum - requested.xMinimum) < 1e-10);
    QVERIFY(qAbs(actual.xMaximum - requested.xMaximum) < 1e-10);
    QVERIFY(qAbs(actual.yMinimum - requested.yMinimum) < 1e-10);
    QVERIFY(qAbs(actual.yMaximum - requested.yMaximum) < 1e-10);

    const auto rejectWithoutChangingRange = [&](SeriesPlotWidget::VisibleRange invalid) {
        QVERIFY(!widget.setVisibleRange(invalid, &error));
        QVERIFY(!error.isEmpty());
        const auto unchanged = widget.visibleRange();
        QVERIFY(qAbs(unchanged.xMinimum - actual.xMinimum) < 1e-10);
        QVERIFY(qAbs(unchanged.xMaximum - actual.xMaximum) < 1e-10);
        QVERIFY(qAbs(unchanged.yMinimum - actual.yMinimum) < 1e-10);
        QVERIFY(qAbs(unchanged.yMaximum - actual.yMaximum) < 1e-10);
    };
    rejectWithoutChangingRange({1.2, 0.2, 1.2, 2.4});
    rejectWithoutChangingRange({256.0, 258.0, 1.2, 2.4});
}

void TestSeriesPlotWidget::segmentsAreClippedBeforeProjection()
{
    auto dataset = sampleDataset();
    dataset.traces[0].points = {{0.0, 0.0}, {1.0, 0.1}};
    dataset.traces[0].originalOrdinals = {4, 9};
    auto secondTrace = dataset.traces.first();
    secondTrace.id = QStringLiteral("trace-two");
    secondTrace.label = QStringLiteral("Outside view");
    secondTrace.sourceId = QStringLiteral("source-two");
    secondTrace.points = {{0.0, 1.0}, {1.0, 1.0}};
    secondTrace.originalOrdinals = {40, 90};
    dataset.traces.append(secondTrace);

    SeriesPlotWidget widget;
    QString error;
    QVERIFY2(widget.setDataset(dataset, &error), qPrintable(error));
    widget.resize(720, 440);
    widget.show();
    QApplication::processEvents();
    QWidget *canvas = plotCanvas(widget);
    QVERIFY(canvas);

    const SeriesPlotWidget::VisibleRange range{0.49, 0.51, 0.055, 0.075};
    QVERIFY2(widget.setVisibleRange(range, &error), qPrintable(error));
    QApplication::processEvents();
    const QImage weakLine = renderCanvas(canvas);

    auto emptyFirstTrace = dataset;
    emptyFirstTrace.traces[0].points.clear();
    emptyFirstTrace.traces[0].originalOrdinals.clear();
    QVERIFY2(widget.setDataset(emptyFirstTrace, &error), qPrintable(error));
    QApplication::processEvents();
    const QImage withoutFirstTrace = renderCanvas(canvas);
    QVERIFY(weakLine == withoutFirstTrace);

    auto crossingLine = dataset;
    crossingLine.traces[0].points = {{0.0, 0.0}, {1.0, 0.14}};
    QVERIFY2(widget.setDataset(crossingLine, &error), qPrintable(error));
    QApplication::processEvents();
    QVERIFY(renderCanvas(canvas) != withoutFirstTrace);
    QVERIFY(samePoints(widget.dataset().traces.first().points, crossingLine.traces.first().points));
    QVERIFY(widget.dataset().traces.first().originalOrdinals == crossingLine.traces.first().originalOrdinals);
}

int runSeriesPlotWidgetTests(int argc, char **argv)
{
    TestSeriesPlotWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_series_plot_widget.moc"
