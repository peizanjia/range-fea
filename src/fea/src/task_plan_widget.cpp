#include "task_plan_widget.h"
#include "workflow_process.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

TaskPlanWidget::TaskPlanWidget(WorkflowProcess *process, QWidget *parent)
    : QWidget(parent), transport(process)
{
    setObjectName("taskPlanWidget");
    setAccessibleName(tr("Persistent native task plans"));
    auto *layout = new QVBoxLayout(this);
    auto *scan = new QHBoxLayout;
    scan->addWidget(new QLabel(tr("Target damping ratios [1]")));
    firstDamping = new QDoubleSpinBox;
    secondDamping = new QDoubleSpinBox;
    for (auto *field : {firstDamping, secondDamping})
    { field->setRange(0.001, 0.5); field->setDecimals(3); field->setSingleStep(0.01); scan->addWidget(field); }
    firstDamping->setObjectName("taskScanFirstDamping"); firstDamping->setValue(0.05);
    secondDamping->setObjectName("taskScanSecondDamping"); secondDamping->setValue(0.10);
    scan->addWidget(new QLabel(tr("Accepted steps per run")));
    scanSteps = new QSpinBox; scanSteps->setRange(1, 5000); scanSteps->setValue(20);
    scanSteps->setObjectName("taskScanSteps"); scan->addWidget(scanSteps);
    auto *create = new QPushButton(tr("Create two-point scan"));
    create->setObjectName("taskPlanCreateScan"); scan->addWidget(create); layout->addLayout(scan);
    connect(create, &QPushButton::clicked, this, &TaskPlanWidget::createScan);
    choice = new QComboBox; choice->setObjectName("taskPlanChoice"); layout->addWidget(choice);
    connect(choice, &QComboBox::currentIndexChanged, this, &TaskPlanWidget::showPlan);
    auto *actions = new QHBoxLayout;
    for (const auto &item : {qMakePair(tr("Start"), QString("start")), qMakePair(tr("Pause plan"), QString("pause")),
                            qMakePair(tr("Resume plan"), QString("resume")), qMakePair(tr("Cancel plan"), QString("cancel"))})
    {
        auto *button = new QPushButton(item.first); button->setObjectName("taskPlan" + item.second);
        connect(button, &QPushButton::clicked, this, [this, action = item.second] { control(action); });
        actions->addWidget(button);
    }
    auto *refreshButton = new QPushButton(tr("Refresh")); refreshButton->setObjectName("taskPlanRefresh");
    connect(refreshButton, &QPushButton::clicked, this, &TaskPlanWidget::refresh); actions->addWidget(refreshButton);
    layout->addLayout(actions);
    status = new QLabel(tr("Choose a configured native document source in Experiment, then create a bounded scan. Each write node needs separate approval."));
    status->setObjectName("taskPlanStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true);
    layout->addWidget(status);
    tree = new QTreeWidget; tree->setObjectName("taskPlanTree"); tree->setAccessibleName(tr("Task nodes, host states and submitted jobs"));
    tree->setHeaderLabels({tr("Node"), tr("Action"), tr("State"), tr("Parameters / units"), tr("Job / evidence")});
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(tree, 1);
    previewDetails = new QPlainTextEdit; previewDetails->setReadOnly(true);
    previewDetails->setObjectName("taskPlanNodePreview"); previewDetails->setMaximumHeight(150);
    previewDetails->setAccessibleName(tr("Selected node source and host parameter changes")); layout->addWidget(previewDetails);
    approve = new QPushButton(tr("Approve selected node preview"));
    approve->setObjectName("taskPlanApproveNode"); approve->setEnabled(false); layout->addWidget(approve);
    connect(tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        approve->setEnabled(transport->isReady() && selectedNode().value("state") == QJsonValue("awaiting_approval"));
        showNodePreview();
    });
    connect(approve, &QPushButton::clicked, this, [this] {
        const auto plan = selectedPlan(), node = selectedNode();
        if (plan.isEmpty() || node.value("state") != QJsonValue("awaiting_approval")) return;
        request("host.task.node.approve", {{"planId", plan.value("planId")}, {"expectedPlanSha256", plan.value("planSha256")},
            {"nodeId", node.value("id")}, {"previewHash", node.value("previewHash")}}, "control");
        approve->setEnabled(false);
    });
    auto *answer = new QPushButton(tr("Answer selected question")); answer->setObjectName("taskPlanAnswer"); layout->addWidget(answer);
    connect(answer, &QPushButton::clicked, this, [this] {
        const auto plan = selectedPlan(), node = selectedNode();
        if (node.value("state") != QJsonValue("awaiting_clarification")) return;
        bool accepted = false;
        const QString text = QInputDialog::getText(this, tr("Plan clarification"), node.value("question").toString().toHtmlEscaped(),
                                                   QLineEdit::Normal, QString(), &accepted);
        if (accepted && !text.isEmpty())
            request("task.plan.clarify", {{"planId", plan.value("planId")}, {"expectedPlanSha256", plan.value("planSha256")},
                {"nodeId", node.value("id")}, {"answer", text}}, "control");
    });
    connect(transport, &WorkflowProcess::readyChanged, this, [this, create](bool ready) {
        ++generation; pending.clear(); queryPending = false; plans.clear(); choice->clear(); tree->clear();
        previewDetails->clear();
        approve->setEnabled(false); create->setEnabled(ready);
        status->setText(ready ? tr("Workspace connected. Refresh or create a plan from its configured native source.")
                             : tr("The workflow session is unavailable. Old previews cannot be approved."));
        if (ready && isVisible()) refresh();
    });
    create->setEnabled(transport->isReady());
    connect(transport, &WorkflowProcess::replyReceived, this,
        [this](const QString &id, const QString &, const QJsonObject &result) {
        if (!pending.contains(id)) return;
        const auto route = pending.take(id);
        if (route.generation != generation || route.workspace != transport->configuration().workspace || !transport->isReady()) return;
        if (route.purpose == "query")
        {
            queryPending = false;
            const QString selected = choice->currentData().toString();
            const QSignalBlocker blocker(choice);
            plans.clear(); choice->clear();
            for (const auto &value : result.value("plans").toArray())
            {
                const auto plan = value.toObject(); const QString planId = plan.value("planId").toString();
                if (planId.isEmpty()) continue;
                plans.insert(planId, plan); choice->addItem(plan.value("title").toString(), planId);
            }
            const int index = choice->findData(selected); if (index >= 0) choice->setCurrentIndex(index);
            showPlan();
        }
        else if (route.purpose == "scan-context")
        {
            const auto config = result.value("configuration").toObject();
            if (config.value("sourceId").toString().isEmpty())
            { status->setText(tr("Configure a registered native document source in Experiment first.")); return; }
            QJsonArray nodes; QString previous;
            const double ratios[] = {firstDamping->value(), secondDamping->value()};
            for (int point = 0; point < 2; ++point)
            {
                const QString prefix = QString("point%1").arg(point+1), configureId = prefix+"Configure", runId = prefix+"Run", verifyId = prefix+"Verify";
                QJsonObject configure{{"id", configureId}, {"kind", "command"}, {"tool", "workflow.configure"},
                    {"successPredicate", "configure.matches"}, {"parameters", QJsonObject{{"dt", config.value("dt")},
                        {"steps", scanSteps->value()}, {"targetDamping", ratios[point]}, {"model", config.value("model")}, {"sourceId", config.value("sourceId")}}}};
                if (!previous.isEmpty()) configure.insert("dependsOn", QJsonArray{previous}); nodes.append(configure);
                nodes.append(QJsonObject{{"id", runId}, {"kind", "command"}, {"tool", "workflow.run"}, {"dependsOn", QJsonArray{configureId}},
                    {"successPredicate", "native.run.verified"}, {"parameters", QJsonObject{{"mode", "both"}, {"independentReference", true}}}});
                nodes.append(QJsonObject{{"id", verifyId}, {"kind", "condition"}, {"dependsOn", QJsonArray{runId}},
                    {"condition", QJsonObject{{"ref", QJsonObject{{"node", runId}, {"output", "maxDisplacementErrorM"}}},
                        {"operator", "le"}, {"value", 1e-8}, {"unit", "m"}}}});
                previous = verifyId;
            }
            proposePlan({{"schema", "range.task-plan/v1"}, {"title", tr("Two-point native damping scan")}, {"nodes", nodes}});
        }
        else { status->setText(tr("The host recorded the plan action. Submitted jobs remain independent.")); refresh(); }
    });
    connect(transport, &WorkflowProcess::requestFailed, this,
        [this](const QString &id, const QString &, const QString &code, const QString &) {
        if (!pending.contains(id)) return;
        const auto route = pending.take(id); if (route.generation != generation) return;
        if (route.purpose == "query") queryPending = false;
        status->setText(tr("The plan action was not confirmed (%1). Refresh its host state before retrying.").arg(code.left(128)));
    });
    timer.setInterval(1000); connect(&timer, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); }); timer.start();
}

void TaskPlanWidget::request(const QString &operation, const QJsonObject &parameters, const QString &purpose)
{
    if (!transport->isReady()) return;
    const QString id = transport->request(operation, parameters);
    if (!id.isEmpty()) pending.insert(id, {transport->configuration().workspace, generation, purpose});
}

void TaskPlanWidget::refresh()
{
    if (!transport->isReady() || queryPending) return;
    request("task.plan.query", {}, "query"); queryPending = !pending.isEmpty();
}

void TaskPlanWidget::proposePlan(const QJsonObject &plan) { request("task.plan.propose", {{"plan", plan}}, "propose"); }
void TaskPlanWidget::createScan() { request("project.query", {}, "scan-context"); }
QJsonObject TaskPlanWidget::selectedPlan() const { return plans.value(choice->currentData().toString()); }
QJsonObject TaskPlanWidget::selectedNode() const
{
    const auto *item = tree->currentItem(); if (!item) return {};
    for (const auto &value : selectedPlan().value("nodes").toArray())
        if (value.toObject().value("id").toString() == item->data(0, Qt::UserRole).toString()) return value.toObject();
    return {};
}
void TaskPlanWidget::control(const QString &action)
{
    const auto plan = selectedPlan(); if (plan.isEmpty()) return;
    request("task.plan."+action, {{"planId", plan.value("planId")}, {"expectedPlanSha256", plan.value("planSha256")}}, "control");
}
void TaskPlanWidget::showPlan()
{
    const QString oldNode = tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const int oldScroll = tree->verticalScrollBar()->value(); tree->clear();
    const auto plan = selectedPlan();
    if (plan.isEmpty()) { approve->setEnabled(false); previewDetails->clear(); return; }
    const auto source = plan.value("sourceRef").toObject();
    const QMap<QString, QString> states{{"validated", tr("Ready to start")}, {"active", tr("Running plan")},
        {"awaiting_approval", tr("Needs approval")}, {"awaiting_clarification", tr("Needs your answer")},
        {"waiting_job", tr("Waiting for calculation")}, {"pending", tr("Waiting for prerequisite")},
        {"completed", tr("Completed")}, {"failed", tr("Failed verification")}, {"paused", tr("Paused")},
        {"cancelled", tr("Cancelled")}, {"conflict", tr("Source or input conflict")},
        {"outcome_unknown", tr("Outcome needs checking")}, {"blocked_dependency", tr("Blocked by prerequisite")}};
    status->setText(tr("%1 · %2 · native source %3. Pausing this plan stops new nodes; submitted jobs keep their own state.")
        .arg(plan.value("title").toString(), states.value(plan.value("state").toString(), plan.value("state").toString()), source.value("sourceId").toString()));
    for (const auto &value : plan.value("nodes").toArray())
    {
        const auto node = value.toObject(), parameters = node.value("parameters").toObject();
        QStringList details;
        if (parameters.contains("dt")) details << tr("dt %1 s").arg(parameters.value("dt").toDouble());
        if (parameters.contains("steps")) details << tr("%1 steps").arg(parameters.value("steps").toInt());
        if (parameters.contains("targetDamping")) details << tr("damping %1 [1]").arg(parameters.value("targetDamping").toDouble());
        if (parameters.contains("mode")) details << parameters.value("mode").toString();
        const QMap<QString, QString> actions{{"workflow.configure", tr("Configure experiment")},
            {"workflow.run", tr("Run native comparison")}, {"condition", tr("Verify numerical condition")},
            {"clarification", tr("Ask for clarification")}, {"jobs.query", tr("Check submitted job")},
            {"results.query", tr("Check accepted results")}, {"result.summary.query", tr("Check native summary")}};
        const QString action = node.value("tool").toString(node.value("kind").toString());
        auto *item = new QTreeWidgetItem(tree, {node.value("id").toString(), actions.value(action, action),
            states.value(node.value("state").toString(), node.value("state").toString()), details.join(" · "), node.value("jobId").toString(node.value("failure").toString())});
        item->setData(0, Qt::UserRole, node.value("id").toString());
        item->setData(2, Qt::UserRole, node.value("state").toString());
        item->setToolTip(0, tr("Source SHA-256: %1").arg(source.value("modelSha256").toString()));
        if (node.value("id").toString() == oldNode) tree->setCurrentItem(item);
    }
    tree->verticalScrollBar()->setValue(oldScroll);
    approve->setEnabled(transport->isReady() && selectedNode().value("state") == QJsonValue("awaiting_approval"));
    showNodePreview();
}

void TaskPlanWidget::showNodePreview()
{
    const auto node = selectedNode(); const auto preview = node.value("preview").toObject();
    if (preview.isEmpty()) { previewDetails->clear(); return; }
    const auto before = preview.value("beforeConfiguration").toObject();
    const auto after = preview.value("effectiveConfiguration").toObject();
    QStringList lines;
    lines << tr("Native source: %1").arg(selectedPlan().value("sourceRef").toObject().value("sourceId").toString());
    const auto field = [&lines, &before, &after, this](const QString &key, const QString &label, const QString &unit) {
        lines << tr("%1: %2 → %3 %4").arg(label, before.value(key).toVariant().toString(), after.value(key).toVariant().toString(), unit);
    };
    field("dt", tr("Time step"), "s"); field("steps", tr("Accepted steps"), "");
    field("targetDamping", tr("Target damping ratio"), "[1]");
    if (node.value("tool") == QJsonValue("workflow.run"))
    {
        const auto parameters = preview.value("normalized").toObject().value("parameters").toObject();
        const QString mode = parameters.value("mode").toString();
        lines << (mode == "both" ? tr("Run active and passive FE and independently verify their accepted results.")
            : mode == "active" ? tr("Run active FE and independently verify its accepted result.")
                               : tr("Run passive FE and independently verify its accepted result."));
    }
    lines << tr("This approval covers only the selected host preview. Submitted jobs are checked separately.");
    previewDetails->setPlainText(lines.join('\n'));
}
