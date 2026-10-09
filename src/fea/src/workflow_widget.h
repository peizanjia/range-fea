#ifndef WORKFLOW_WIDGET_H
#define WORKFLOW_WIDGET_H

#include <QWidget>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTimer>
#include "workflow_process.h"

class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QLabel;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QCheckBox;
class QTabWidget;
class ControlMonitorWidget;
class NativeResultsWidget;
class GeometryCommandWidget;
class MeshCommandWidget;
class Session;

//! A user-facing host of the actual persisted rod/beam workflow service.
class WorkflowWidget : public QWidget
{
    Q_OBJECT
public:
    explicit WorkflowWidget(QWidget *parent = nullptr);
    WorkflowProcess *transport() { return &service; }
    void setSession(Session *session);
    void openWorkspace(const WorkflowProcess::Configuration &configuration);
    QJsonObject currentPreview() const { return preview; }

public slots:
    void previewCommand(const QString &command, const QJsonObject &arguments);
    void applyPreview();
    void refreshJobs();
    void showDocumentResult(const QString &documentId, quint64 sourceRevision,
                            const QString &artifactPath, bool applied);

signals:
    void previewAvailable(const QJsonObject &preview);
    void jobAccepted(const QString &jobId);
    void resultDisplayed(const QJsonObject &result);
    void openResultRequested(const QString &artifactPath);

private:
    GeometryCommandWidget *geometryCommands = nullptr;
    MeshCommandWidget *meshCommands = nullptr;
    WorkflowProcess service;
    QTimer refreshTimer;
    QTabWidget *pages;
    ControlMonitorWidget *monitor;
    NativeResultsWidget *workflowNativeResults;
    NativeResultsWidget *documentNativeResults;
    QComboBox *nativeMode;
    QComboBox *nativeJobChoice;
    QPushButton *nativeRecoveryButton, *nativeRecoveryApply;
    QLabel *nativeRecoveryStatus;
    QString nativeRecoveryPreviewId, nativeRecoveryJobId;
    QTimer nativeJobTimer;
    QLineEdit *workspaceEdit;
    QCheckBox *trustedExtensions;
    QCheckBox *runtimeUpdates;
    QCheckBox *initialRuntimePause;
    QComboBox *modelChoice;
    QDoubleSpinBox *dt;
    QDoubleSpinBox *damping;
    QSpinBox *steps;
    QPushButton *connectButton;
    QPushButton *sourceButton;
    QPushButton *configureButton;
    QPushButton *runButton;
    QPushButton *resumeButton;
    QPushButton *recoverButton;
    QPushButton *cancelButton;
    QPushButton *applyButton;
    QLabel *status;
    QTextBrowser *previewText;
    QTreeWidget *jobTree;
    QTreeWidget *documentResults;
    QTableWidget *results;
    QTextBrowser *audit;
    QMap<QString, QTreeWidgetItem *> jobRows;
    QMap<QString, QString> continuations;
    QSet<QString> ownedRequests;
    struct DocumentSourceChoice
    {
        QJsonObject registration;
        QJsonObject snapshotRef;
        QString runtimeDocumentId;
        QString sessionEpoch;
        QString catalogRoot;
        quint64 runtimeRevision = 0;
    };
    Session *session = nullptr;
    QMap<QString, DocumentSourceChoice> documentSources;
    DocumentSourceChoice pendingDocumentSource;
    QString sourceRegisterRequest;
    QJsonObject preview, pendingArguments;
    QString pendingCommand, displayedResult, jobsRequest, foregroundJob;
    QString recoveryRequest;
    bool commandInFlight = false;
    bool sourceRegistrationInFlight = false;
    bool sourceContextRefreshPending = false;
    quint64 sourceContextRefreshGeneration = 0;
    void invalidateStaleSourceActions();
    void queueSourceContextRefresh();
    void useCurrentDocumentSnapshot();
    QString selectedSourceId() const;
    bool sourceRuntimeStampMatches(const DocumentSourceChoice &source, QString *reason = nullptr) const;
    bool sourceRuntimeIsCurrent(const DocumentSourceChoice &source, QString *reason = nullptr) const;
    QString sourceDescription(const DocumentSourceChoice &source, bool includePorts) const;
    QString modelBindingSummary(const QJsonObject &binding) const;
    void updateSourceChoiceLabel(const QString &sourceId);
    void updateSelectedSourceStatus();
    void refreshNativeJobs();
    void checkNativeRecovery();
    void applyNativeRecovery();
    bool previewSourceIsCurrent() const;
    QString previewSourceId() const;
    void onReply(const QString &id, const QString &operation, const QJsonObject &result);
    void showResult(const QJsonObject &result);
    void showFailure(const QString &message);
    void setActionAvailability();
    QString request(const QString &operation, const QJsonObject &parameters = {});
};

#endif
