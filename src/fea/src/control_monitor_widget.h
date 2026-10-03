#ifndef CONTROL_MONITOR_WIDGET_H
#define CONTROL_MONITOR_WIDGET_H

#include <QWidget>
#include <QJsonArray>
#include <QTimer>
#include "workflow_process.h"

class QLabel;
class QComboBox;
class QPushButton;
class ControlTraceCanvas;

//! A read-only view of accepted native control steps. Holding the display never
//! pauses the solver; full-fidelity samples continue arriving in the buffer.
class ControlMonitorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ControlMonitorWidget(WorkflowProcess *service, QWidget *parent = nullptr);
    void setJob(const QString &jobId, const QString &state);
    int sampleCount() const { return samples.size(); }
    QString currentJob() const { return jobId; }
signals:
    void samplesUpdated(const QString &jobId, int count, const QString &state);
private:
    WorkflowProcess *service;
    QTimer pollTimer;
    QString jobId, jobState, pendingRequest, runId, availabilityMessage;
    quint64 cursor = 0;
    QJsonArray samples, heldSamples;
    QLabel *status;
    QComboBox *sensor, *actuator, *timeRange;
    QPushButton *hold;
    ControlTraceCanvas *canvas;
    bool faulted = false;
    void poll();
    void receive(const QJsonObject &result);
    void refreshPlot();
    void exportCsv();
};

#endif
