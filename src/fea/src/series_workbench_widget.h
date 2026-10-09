#ifndef SERIES_WORKBENCH_WIDGET_H
#define SERIES_WORKBENCH_WIDGET_H

#include "series_plot_widget.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QWidget>
#include <memory>
#include <optional>

class WorkflowProcess;
class QComboBox;
class QCheckBox;
class QLabel;
class QListWidget;
class QSplitter;
class QSaveFile;

//! Read-only authenticated results. Only a trusted host file dialog chooses export destinations.
class SeriesWorkbenchWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SeriesWorkbenchWidget(WorkflowProcess *transport, QWidget *parent = nullptr);
    ~SeriesWorkbenchWidget() override;
    const QJsonObject &displayedDto() const { return displayed; }
    SeriesPlotWidget *plotWidget() const { return plot; }
    static bool parsePlotDto(const QJsonObject &dto, SeriesPlotWidget::Dataset &dataset, QString *error = nullptr);
    // Trusted GUI host API, never exposed as an RPC destination parameter.
    void exportToFile(const QString &destination);
public slots:
    void refresh();
    void queryPlot();
    void saveLayout();
    void restoreLayout();
private:
    struct Pending { QString workspace, purpose, job, mode; quint64 generation; QJsonArray sources; QString view; QJsonObject options; };
    WorkflowProcess *transport;
    QMap<QString, Pending> requests;
    quint64 generation = 0;
    QComboBox *jobs, *modes, *channels, *views, *window;
    QCheckBox *removeMean;
    QListWidget *selected;
    QLabel *status, *caption;
    QSplitter *splitter;
    SeriesPlotWidget *plot;
    QJsonObject catalog, displayed;
    QJsonObject displayedOptions;
    std::optional<QJsonObject> pendingDisplay;
    std::optional<QJsonObject> pendingOptions;
    QString projectId, workspace, layoutKey;
    QJsonArray restorePins;
    std::optional<SeriesPlotWidget::VisibleRange> restoreRange;
    std::optional<SeriesPlotWidget::Cursor> restoreCursor;
    bool restoreHold = false;
    bool reconnectValidation = false;
    std::unique_ptr<QSaveFile> exportFile;
    std::unique_ptr<QCryptographicHash> exportHash;
    QJsonObject exportManifest;
    QJsonArray exportSources;
    qint64 exportOffset = 0;
    quint64 exportGeneration = 0;
    QString jobId() const;
    QString mode() const;
    QJsonArray sourceRefs() const;
    void send(const QString &operation, const QJsonObject &parameters, const QString &purpose);
    void invalidate(bool workspaceChanged = false);
    void selectCatalog();
    void applyCatalog(const QJsonObject &result);
    void addSource(bool reference);
    void adopt(const QJsonObject &dto, const QJsonObject &options, bool restoring = false);
    void updateDisplayMetadata();
    void receive(const Pending &route, const QJsonObject &result);
    void abortExport(const QString &message);
    void nextChunk();
};
#endif
