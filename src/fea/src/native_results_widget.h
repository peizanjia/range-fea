#ifndef NATIVE_RESULTS_WIDGET_H
#define NATIVE_RESULTS_WIDGET_H

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class NativeResultCanvas;
class WorkflowProcess;

//! Read-only native result view. Query handlers return raw broker responses;
//! queries carry registered opaque identities, never caller-selected file paths.
class NativeResultsWidget : public QWidget
{
    Q_OBJECT
public:
    using QueryHandler = std::function<QJsonObject(const QString &, const QJsonObject &)>;
    explicit NativeResultsWidget(QWidget *parent = nullptr);
    void setQueryHandler(QueryHandler handler);
    void setArtifact(const QString &jobId, const QString &artifactId, const QString &sourceLabel);
    void setWorkflow(WorkflowProcess *workflow);
    void setWorkflowJob(const QString &jobId, const QString &mode);
    QJsonObject displaySnapshot() const;
    bool exportCsv(const QString &path, QString *error = nullptr) const;
public slots:
    void refreshResults();
    void setHeld(bool held);
signals:
    void snapshotChanged();
    void queryFailed(const QString &message);
private:
    QueryHandler queryHandler;
    QPointer<WorkflowProcess> workflow;
    bool workflowSource = false;
    bool followLive = false;
    QString jobId, artifactId, fixedArtifactId, sourceLabel, mode, selectedChannel;
    QString pendingId, pendingOperation;
    quint64 generation = 0;
    int channelOffset = 0;
    QJsonArray channels, stagedChannels, samples;
    QJsonObject provenance, summary, heldSnapshot;
    QComboBox *channelBox, *rangeBox;
    QPushButton *holdButton, *exportButton, *fieldButton;
    QLabel *statusLabel, *sourceInfo;
    QTableWidget *table;
    NativeResultCanvas *canvas;
    QTimer *liveTimer;
    void clearSource();
    void query(const QString &operation, QJsonObject arguments = {});
    void receive(const QString &operation, const QJsonObject &response);
    void fail(const QString &message, bool retryPending = false);
    void loadChannel();
    void refreshView();
    QJsonObject currentSnapshot() const;
    void chooseExport();
    static bool writeSnapshot(const QJsonObject &snapshot, const QString &path, QString *error);
};

#endif
