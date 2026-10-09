#ifndef TASK_PLAN_WIDGET_H
#define TASK_PLAN_WIDGET_H

#include <QJsonObject>
#include <QMap>
#include <QTimer>
#include <QWidget>

class WorkflowProcess;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QTreeWidget;
class QLabel;
class QPushButton;
class QPlainTextEdit;

class TaskPlanWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TaskPlanWidget(WorkflowProcess *transport, QWidget *parent = nullptr);
    void proposePlan(const QJsonObject &plan);

public slots:
    void refresh();

private:
    struct Pending { QString workspace; quint64 generation; QString purpose; };
    WorkflowProcess *transport;
    QMap<QString, Pending> pending;
    QMap<QString, QJsonObject> plans;
    QTimer timer;
    quint64 generation = 0;
    bool queryPending = false;
    QComboBox *choice;
    QTreeWidget *tree;
    QLabel *status;
    QDoubleSpinBox *firstDamping, *secondDamping;
    QSpinBox *scanSteps;
    QPushButton *approve;
    QPlainTextEdit *previewDetails;
    void showNodePreview();
    void request(const QString &operation, const QJsonObject &parameters, const QString &purpose);
    void showPlan();
    void control(const QString &action);
    void createScan();
    QJsonObject selectedPlan() const;
    QJsonObject selectedNode() const;
};

#endif
