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

//! A user-facing host of the actual persisted rod/beam workflow service.
class WorkflowWidget : public QWidget
{
    Q_OBJECT
public:
    explicit WorkflowWidget(QWidget *parent = nullptr);
    WorkflowProcess *transport() { return &service; }
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
    WorkflowProcess service;
    QTimer refreshTimer;
    QLineEdit *workspaceEdit;
    QCheckBox *trustedExtensions;
    QComboBox *modelChoice;
    QDoubleSpinBox *dt;
    QDoubleSpinBox *damping;
    QSpinBox *steps;
    QPushButton *connectButton;
    QPushButton *configureButton;
    QPushButton *runButton;
    QPushButton *resumeButton;
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
    QJsonObject preview, pendingArguments;
    QString pendingCommand, displayedResult, jobsRequest, foregroundJob;
    bool commandInFlight = false;
    void onReply(const QString &id, const QString &operation, const QJsonObject &result);
    void showResult(const QJsonObject &result);
    void showFailure(const QString &message);
    void setActionAvailability();
    QString request(const QString &operation, const QJsonObject &parameters = {});
};

#endif
