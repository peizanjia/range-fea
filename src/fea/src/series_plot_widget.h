#ifndef SERIES_PLOT_WIDGET_H
#define SERIES_PLOT_WIDGET_H

#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <optional>

class QCheckBox;
class QPushButton;
class SeriesPlotCanvas;

//! A bounded, read-only plot of host-verified displayed series samples.
class SeriesPlotWidget : public QWidget
{
    Q_OBJECT
public:
    enum class ViewKind { Time, Phase, Spectrum };

    struct Trace
    {
        QString id;
        QString label;
        QString unit;
        QString sourceId;
        QString artifactSha256;
        QVector<QPointF> points;
        // Time/Phase store zero-based raw sample indices; Spectrum stores FFT-bin indices.
        QVector<quint64> originalOrdinals;
    };

    struct Dataset
    {
        ViewKind viewKind = ViewKind::Time;
        QString title;
        QString xLabel;
        QString xUnit;
        QString yLabel;
        QString yUnit;
        QVector<Trace> traces;
    };

    //! Visible axis bounds in the dataset's raw x/y units.
    struct VisibleRange
    {
        double xMinimum = 0.0;
        double xMaximum = 0.0;
        double yMinimum = 0.0;
        double yMaximum = 0.0;
    };
    struct Cursor { QString traceId; quint64 ordinal = 0; };

    explicit SeriesPlotWidget(QWidget *parent = nullptr);

    //! Validate a complete replacement before changing the visible or pending dataset.
    bool setDataset(const Dataset &dataset, QString *error = nullptr);
    const Dataset &dataset() const { return displayedDataset; }
    bool hasDataset() const { return datasetAvailable; }
    quint64 datasetGeneration() const { return displayedGeneration; }
    bool isDisplayHeld() const { return displayHeld; }

    //! Set a finite, increasing visible range in raw dataset units.
    bool setVisibleRange(VisibleRange range, QString *error = nullptr);
    //! Return the canvas's current visible range in raw dataset units.
    VisibleRange visibleRange() const;
    std::optional<Cursor> cursor() const { return displayedCursor; }
    bool setCursor(const Cursor &cursor, QString *error = nullptr);

    //! The caller must verify that a complete raw-series export is available.
    void setFullExportAvailable(bool available);

public slots:
    void setDisplayHeld(bool held);
    void fitToData();

signals:
    //! One notification per presented generation after nonempty canvas painting.
    void datasetPainted(quint64 datasetGeneration);
    void exportFullSeriesRequested();
    void displayHoldChanged(bool held);
    void cursorChanged(const QString &traceId, quint64 ordinal);

private:
    Dataset displayedDataset;
    quint64 displayedGeneration = 0;
    std::optional<Dataset> pendingDataset;
    std::optional<Cursor> displayedCursor;
    bool datasetAvailable = false;
    bool displayHeld = false;
    bool fullExportAvailable = false;
    QCheckBox *holdCheck = nullptr;
    QPushButton *fitButton = nullptr;
    QPushButton *exportButton = nullptr;
    SeriesPlotCanvas *canvas = nullptr;

    static bool validateDataset(const Dataset &dataset, QString *error);
    static bool samePlotIdentity(const Dataset &left, const Dataset &right);
    void presentDataset(const Dataset &dataset);
    void updateExportAvailability();
};

#endif
