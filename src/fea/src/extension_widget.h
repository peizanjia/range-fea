#ifndef EXTENSION_WIDGET_H
#define EXTENSION_WIDGET_H

#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;
class WorkflowProcess;

//! GUI for inspecting and operating service-managed Range workflow extensions.
//! It shares the active WorkflowProcess and never owns or starts the service.
class ExtensionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ExtensionWidget(WorkflowProcess *service, QWidget *parent = nullptr);

    //! Parse the UI's bounded CSV/newline numeric-series editor into its wire form.
    static bool parseNumericSeries(const QString &timesText, const QString &valuesText,
                                   const QString &timeUnit, const QString &valueUnit,
                                   QJsonObject *series, QString *error = nullptr);

public slots:
    void refreshExtensions();

private slots:
    void refreshJobs();
    void approvePreview();
    void validateSelectedCandidate();
    void activateSelectedCandidate();
    void rollbackSelectedExtension();
    void runSelectedExtension();
    void loadMoreCandidates();

private:
    enum class RequestKind
    {
        Capabilities,
        ManifestSchema,
        NumericSeriesSchema,
        ExtensionList,
        ExtensionDetails,
        CandidateDetails,
        VersionDetails,
        Jobs,
        NativeChannels,
        ProjectForPreview,
        CommandPreview,
        HostApproval,
        CommandExecute,
        JobPoll
    };

    struct PendingRequest
    {
        RequestKind kind = RequestKind::Capabilities;
        QString extensionId;
        QString candidateId;
        QString bundleHash;
        QString intent;
        QString operation;
        int candidateOffset = 0;
    };

    QPointer<WorkflowProcess> service;
    QTimer *refreshTimer = nullptr;
    QTimer *jobPollTimer = nullptr;
    QTreeWidget *extensionTree = nullptr;
    QLabel *status = nullptr;
    QLabel *trustStatus = nullptr;
    QTabWidget *detailTabs = nullptr;
    QPlainTextEdit *manifestView = nullptr;
    QPlainTextEdit *sourceView = nullptr;
    QPlainTextEdit *diffView = nullptr;
    QPlainTextEdit *testsView = nullptr;
    QPlainTextEdit *reportView = nullptr;
    QComboBox *rollbackTarget = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *loadMoreButton = nullptr;
    QPushButton *validateButton = nullptr;
    QPushButton *activateButton = nullptr;
    QPushButton *rollbackButton = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *approveButton = nullptr;
    QPlainTextEdit *timesInput = nullptr;
    QPlainTextEdit *valuesInput = nullptr;
    QComboBox *timeUnitInput = nullptr;
    QComboBox *valueUnitInput = nullptr;
    QComboBox *sourceMode = nullptr;
    QComboBox *sourceJob = nullptr;
    QComboBox *sourceSignal = nullptr;
    QTableWidget *metricsTable = nullptr;
    QPlainTextEdit *previewView = nullptr;
    QPlainTextEdit *resultView = nullptr;

    QMap<QString, PendingRequest> pendingRequests;
    QJsonObject extensionListResult;
    QMap<QString, QJsonObject> extensionDetails;
    QMap<QString, QString> extensionDetailsRequests;
    QJsonObject selectedCandidate;
    QJsonObject selectedVersion;
    QJsonObject activeRuntimeCapability;
    QJsonObject manifestSchema;
    QJsonObject numericSeriesSchema;
    QJsonObject currentPreview;
    QJsonObject pendingCommandParameters;
    QJsonArray availableJobs;
    QString selectedExtensionId;
    QString selectedCandidateId;
    QString selectedBundleHash;
    QString deferredSelectionKey;
    QString deferredSelectionExtensionId;
    QString pendingCommand;
    QString pendingIntent;
    QString pendingJobId;
    QString pendingJobCommand;
    QString observedWorkspace;
    QString extensionListRequest;
    QString jobsRequest;
    QString nativeChannelRequest;
    QString nativeSourceKey;
    QString desiredNativeChannel;
    QJsonObject nativeSourceDescriptor;
    bool commandInFlight = false;
    bool trustEnabled = false;
    bool runtimeCapabilityKnown = false;
    bool featureFormSupported = false;
    bool rebuildInProgress = false;
    bool refreshSelectedDetailsAfterList = false;

    QString request(const QString &operation, const QJsonObject &parameters,
                    const PendingRequest &context);
    void onReply(const QString &requestId, const QString &operation, const QJsonObject &result);
    void onRequestFailed(const QString &requestId, const QString &operation,
                         const QString &code, const QString &message);
    void handleReadyChanged(bool ready);
    void requestSelectedDetails(QTreeWidgetItem *item);
    void renderExtensionList(const QJsonObject &result);
    void requestExtensionDetails(const QString &extensionId, int candidateOffset);
    QJsonObject extensionDetailsEntry(const QJsonObject &result) const;
    QJsonObject rootExtensionSummary(const QString &extensionId) const;
    QString extensionDetailsRequestKey(const QString &extensionId, int candidateOffset) const;
    void renderCandidate(const QJsonObject &candidate);
    void renderVersion(const QJsonObject &version);
    void renderFormAndMetrics(const QJsonObject &manifest);
    void renderMetrics(const QJsonObject &result);
    void renderJob(const QJsonObject &job);
    void beginCommandPreview(const QString &command, const QJsonObject &parameters,
                             const QString &intent);
    void pollJob();
    void clearPreview(const QString &message = QString());
    void clearDetails(const QString &message = QString());
    void updateAvailability();
    void updateTrustStatus();
    void syncRollbackTargets();
    void refreshSourceSignals();
    QString currentSelectionKey() const;
    QJsonObject currentExtensionSummary() const;
    QJsonObject currentCandidateSummary() const;
    bool selectedCandidateValidated() const;
    bool selectedVersionActive() const;
    bool buildRunParameters(QJsonObject *parameters, QString *error) const;
};

#endif // EXTENSION_WIDGET_H
