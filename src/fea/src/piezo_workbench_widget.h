#ifndef PIEZO_WORKBENCH_WIDGET_H
#define PIEZO_WORKBENCH_WIDGET_H

#include <QWidget>
#include <QPointer>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QStringList>
#include <QElapsedTimer>
#include <functional>
#include <piezo_result_series.h>

class Session;
class RModel;
class QLabel;
class QPushButton;
class QCheckBox;
class QTabWidget;
class QTableWidget;
class QVBoxLayout;
class QDoubleSpinBox;
class QComboBox;
class SeriesPlotWidget;
class QFileSystemWatcher;

//! Nonmodal editor. Only the host may publish a fully previewed experiment.
class PiezoWorkbenchWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PiezoWorkbenchWidget(QWidget *parent = nullptr);
    void setSession(Session *session);
    QJsonObject draftRecipe() const { return recipe; }
    bool hasCurrentPreview() const;
    bool exportMatrixPackageTo(const QString &path);
    bool exportAcceptedTimeSeriesTo(const QStringList &nativePaths, const QString &path);
    bool exportCurrentAcceptedTimeSeriesTo(const QString &path);
    bool showAcceptedTimeSeriesFrom(const QStringList &nativePaths, bool *sourceBusy = nullptr);
    //! Bind only the exact completed job accepted by this widget's Run+Confirm flow.
    bool bindCompletedNativeJob(const QString &jobId, bool *retryable = nullptr);
    //! Trusted application/test authority; interactive UI discovers its registered proof.
    bool showAcceptedTimeSeriesFromTrace(const PiezoResultSeries::TraceProof &proof);
    bool exportAcceptedTimeSeriesFromTrace(const PiezoResultSeries::TraceProof &proof, const QString &path);
    bool addExternalForceFunctionFrom(const QString &path, const QString &functionName,
                                      const QStringList &dependencyPaths = {});

public slots:
    void refreshContext();
    void previewExperiment();
    void applyPreview();
    void previewRun();
    void usePickedMembers();
    void computeModes();
    void exportMatrixPackage();
    void exportAcceptedTimeSeries();
    void showAcceptedTimeSeries();

signals:
    void experimentApplied();
    void runSubmitted(const QString &jobId);

private:
    friend class TestPiezoWorkbenchWidget;
    QPointer<Session> session;
    QJsonObject recipe, savedRecipe, loadedDocument, approvedPreview, previewDocument;
    QJsonObject computedModes, computedModesInput;
    QJsonObject synchronizedTemplate;
    QJsonObject retiredAxialPorts;
    QJsonObject axialDampingPresets;
    std::shared_ptr<const PiezoResultSeries::Dataset> acceptedSeries;
    std::shared_ptr<const PiezoResultSeries::NumericDataset> acceptedNumericSeries;
    PiezoResultSeries::TraceProof acceptedTraceProof;
    QJsonObject acceptedSeriesDocument;
    QStringList acceptedSeriesPaths, acceptedSeriesHashes, acceptedSelectionPaths;
    QJsonArray acceptedSeriesFileStamps;
    QString submittedRunJobId, acceptedNativeJobId;
    QHash<QString,QJsonObject> appliedNativeResults;
    QFileSystemWatcher *acceptedNativeWatcher = nullptr;
    QStringList acceptedNativeWatchFiles, acceptedNativeWatchDirectories;
    QJsonObject acceptedNativeFileStamps;
    quint64 acceptedNativeWatchGeneration = 0;
    mutable bool externalSourceMetadataReady = false;
    mutable QJsonArray externalSourceMetadataFunctions;
    mutable QJsonObject externalSourceMetadataStamps;
    QComboBox *resultPort = nullptr, *resultQuantity = nullptr, *resultReference = nullptr;
    QComboBox *resultView = nullptr, *resultWindow = nullptr;
    QCheckBox *resultRemoveMean = nullptr;
    QString resultViewKind = "time", resultWindowKind = "hann";
    bool resultRemoveMeanValue = true;
    QLabel *resultCoverage = nullptr;
    SeriesPlotWidget *resultPlot = nullptr;
    QString previewWorkspace, previewCommand;
    QJsonArray selectedMembers, nodeCatalog;
    QLabel *source = nullptr, *status = nullptr, *receipt = nullptr;
    QPushButton *previewButton = nullptr, *applyButton = nullptr, *runButton = nullptr;
    QTabWidget *tabs = nullptr;
    QTableWidget *changes = nullptr;
    bool rebuilding = false, refreshPending = false, editorRebuildPending = false;
    bool templateSynchronized = true;
    quint64 contextGeneration = 0;
    QElapsedTimer performanceTimer;
    QString performanceRequestId, performanceOperation, performanceWorkspace, performanceEvents, performanceOrigin;
    QString performanceSource, performanceArtifact, performanceChannel;
    quint64 performanceGeneration = 0;
    bool performanceEnabled = false, performanceInvalid = false, performancePendingPaint = false;
    QJsonObject performanceBinding;
    QString lastFocusedEditorName;
    int lastFocusedEditorPage = -1;
    void beginPerformanceRequest(const QString &operation, const QJsonObject &arguments);
    void performanceEvent(const QString &event, QJsonObject fields = {});
    void invalidatePerformance(const QString &reason);
    void bindPerformancePlot();
    void bindPerformanceTrace(const PiezoResultSeries::TraceProof &proof, const QJsonObject &document);
    void acceptedDatasetPainted(quint64 generation);
    QJsonObject acceptedPerformanceIdentity() const;

    QJsonObject currentDocument(bool *temporarilyUnavailable = nullptr) const;
    void queueContextRefresh();
    void queueEditorRebuild();
    void invalidatePreview(const QString &reason);
    void loadDocument(const QJsonObject &document);
    void buildEditors();
    void buildEditors(int activePage, int previousScrollX, int previousScrollY,
                      const QString &focusedEditorName);
    void updateRunAvailability();
    void requestPreview(const QString &command, const QJsonObject &arguments);
    void showPreview(const QJsonObject &result);
    bool hasCurrentPreviewCurrent(bool verifyExternalSources, bool externalSourcesKnownCurrent = false) const;
    void configureController(const QString &kind, const QString &stateSource);
    int compiledFreeDofs() const;
    QJsonValue valueAt(const QStringList &path) const;
    void setValue(const QStringList &path, const QJsonValue &value);
    QDoubleSpinBox *numberEditor(const QStringList &path, QWidget *parent,
                                const QString &name = {}, double minimum = -1e30,
                                double maximum = 1e30);
    QComboBox *choiceEditor(const QStringList &path, const QStringList &values,
                           QWidget *parent, const QString &name = {}, bool rebuild = false);
    QComboBox *nodeEditor(const QStringList &path, QWidget *parent);
    QTableWidget *matrixEditor(const QStringList &path, int rows, int columns,
                              bool flat, QWidget *parent, const QString &name);
    void addNodeFields(QVBoxLayout *layout, const QString &field, const QString &label);
    void addOutputEditor(QVBoxLayout *layout);
    void addExcitationEditor(QVBoxLayout *layout);
    QStringList allowedForceDirections() const;
    QString defaultForceDirection() const;
    bool forceDirectionsValid() const;
    QComboBox *forceDirectionEditor(const QString &channelId, QWidget *parent, const QString &name);
    void bindForceChannelEditors(const QStringList &path, QComboBox *node, QComboBox *direction,
                                 QLabel *returnedName, QLabel *unit);
    void addExternalFunctionEditor(QVBoxLayout *layout);
    void addExternalProgramEditor(QVBoxLayout *layout,const QStringList &path,const QString &role);
    void addExternalExcitationEditors(QVBoxLayout *layout);
    void inspectExternalConnection(const QStringList &path,bool test);
    bool externalSourcesCurrent() const;
    bool externalSourcesMetadataCurrent() const;
    bool reloadExternalForceFunction(int index, const QStringList &dependencyPaths);
    void addWorkpointEditor(QVBoxLayout *layout);
    void addAcceptedResultEditor(QVBoxLayout *layout);
    QVector<PiezoResultSeries::Readout> acceptedReadouts() const;
    void populateResultQuantities();
    void renderAcceptedSeries(bool verifyRecordFiles = false);
    void clearAcceptedSeries(const QString &reason);
    void clearAcceptedNativeWatch();
    bool captureAcceptedNativeWatch(const QJsonObject &job, QString *reason);
    bool acceptedNativeWatchCurrent() const;
    bool acceptedSeriesCurrent(bool verifyRecordFiles = false, bool externalSourcesKnownCurrent = false) const;
    void synchronizeTemplate(const QJsonObject &previousTemplate);
    void rebindAxialPorts(const QJsonArray &previousMembers, const QJsonArray &defaults = {});
    bool discoverAcceptedTrace(const QStringList &paths, const QString &sourceFingerprint,
                               PiezoResultSeries::TraceProof *proof, QStringList *files, QStringList *hashes) const;
    bool certifyCompletedNativeJob(const QString &jobId, const QJsonObject &job,
                                   const QString &artifactPath, const QJsonObject &document,
                                   QString *reason) const;
    QString exportDirectory() const;
    bool savedSourceSnapshot(QJsonObject *document, RModel *model, bool *sourceBusy = nullptr);
};
#endif
