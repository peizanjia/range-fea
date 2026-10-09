#ifndef RUNTIME_CONTROL_WIDGET_H
#define RUNTIME_CONTROL_WIDGET_H

#include <QJsonObject>
#include <QMap>
#include <QTimer>
#include <QWidget>

class WorkflowProcess;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class RuntimeControlWidget : public QWidget
{
    Q_OBJECT
public:
    explicit RuntimeControlWidget(WorkflowProcess *transport, QWidget *parent = nullptr);
public slots:
    void refresh();
private:
    struct Pending {
        QString workspace, purpose, jobId, proposalId, binding, identity;
        quint64 generation;
    };
    WorkflowProcess *transport;
    QMap<QString, Pending> pending;
    QTimer timer;
    quint64 generation = 0;
    QComboBox *jobs;
    QComboBox *savedProposals;
    QDoubleSpinBox *gain, *minimum, *maximum, *rate;
    QLabel *metadata, *status, *execution;
    QPlainTextEdit *preview, *history;
    QPushButton *prepare, *approve, *pause, *resume, *cancel;
    QPushButton *cancelUpdate;
    QJsonObject attempt, proposal;
    QString binding;
    bool draftLoaded = false, submitted = false, busy = false, connected = false;
    QString jobId() const;
    void send(const QString &operation, const QJsonObject &parameters, const QString &purpose);
    void selectJob();
    void invalidatePreview();
    void updateActions();
    void observe(const QJsonObject &result);
    void showProposal(const QJsonObject &record);
};
#endif
