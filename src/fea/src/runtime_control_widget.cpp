#include "runtime_control_widget.h"
#include "workflow_process.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QVBoxLayout>
#include <utility>
#include <cmath>

namespace {
QString value(const QJsonObject &object, const QString &key)
{
    const auto item = object.value(key);
    return item.isString() ? item.toString() : QString::number(item.toDouble(), 'g', 12);
}
QString fingerprint(const QJsonObject &record)
{
    return QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact));
}
}

RuntimeControlWidget::RuntimeControlWidget(WorkflowProcess *process, QWidget *parent)
    : QWidget(parent), transport(process)
{
    setObjectName("runtimeControlWidget");
    setAccessibleName(tr("Native runtime velocity feedback control"));
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll); scroll->setWidget(body); outer->addWidget(scroll);
    auto *layout = new QVBoxLayout(body);
    jobs = new QComboBox; jobs->setObjectName("runtimeJobChoice");
    jobs->setAccessibleName(tr("Running native job")); layout->addWidget(jobs);
    savedProposals = new QComboBox;
    savedProposals->setObjectName("runtimeProposalChoice");
    savedProposals->setAccessibleName(tr("Saved parameter update proposals"));
    savedProposals->addItem(tr("Select a saved parameter update"), QString());
    layout->addWidget(savedProposals);
    metadata = new QLabel; metadata->setObjectName("runtimeIdentity");
    metadata->setTextFormat(Qt::PlainText); metadata->setWordWrap(true); layout->addWidget(metadata);
    execution = new QLabel(tr("Native execution state unavailable"));
    execution->setObjectName("runtimeExecutionState"); execution->setTextFormat(Qt::PlainText);
    execution->setWordWrap(true); layout->addWidget(execution);
    auto *form = new QFormLayout; form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto field = [form](const QString &label, const char *name, double lo, double hi) {
        auto *spin = new QDoubleSpinBox; spin->setObjectName(name); spin->setDecimals(9);
        spin->setRange(lo, hi); spin->setAccessibleName(label); form->addRow(label, spin); return spin;
    };
    gain = field(tr("Velocity gain [N*s/m]"), "runtimeGain", 1e-9, 1e12);
    minimum = field(tr("Minimum force [N]"), "runtimeMinForce", -1e12, 1e12);
    maximum = field(tr("Maximum force [N]"), "runtimeMaxForce", -1e12, 1e12);
    rate = field(tr("Force rate [N/s] (0: unlimited source only)"), "runtimeForceRate", 0, 1e12);
    layout->addLayout(form);
    auto *actions = new QGridLayout;
    auto button = [actions](const QString &text, const char *name, int row, int column) {
        auto *result = new QPushButton(text); result->setObjectName(name);
        actions->addWidget(result, row, column); return result;
    };
    prepare = button(tr("Prepare update"), "runtimePrepare", 0, 0);
    approve = button(tr("Approve + submit"), "runtimeApproveSubmit", 0, 1);
    pause = button(tr("Pause run"), "runtimePause", 1, 0);
    resume = button(tr("Resume run"), "runtimeResume", 1, 1);
    cancel = button(tr("Cancel run"), "runtimeCancel", 2, 0);
    auto *refreshButton = button(tr("Refresh"), "runtimeRefresh", 2, 1);
    cancelUpdate = button(tr("Cancel update"), "runtimeCancelUpdate", 3, 0);
    cancelUpdate->setToolTip(tr("Withdraw a queued parameter update while preserving the run. A frozen step may already be accepting it."));
    layout->addLayout(actions);
    status = new QLabel(tr("Select a running job with a ready native runtime channel."));
    status->setObjectName("runtimeStatus"); status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true); layout->addWidget(status);
    preview = new QPlainTextEdit; preview->setObjectName("runtimePreview"); preview->setReadOnly(true);
    preview->setAccessibleName(tr("Before and after runtime parameter preview")); layout->addWidget(preview);
    history = new QPlainTextEdit; history->setObjectName("runtimeAcceptedHistory"); history->setReadOnly(true);
    history->setAccessibleName(tr("Accepted runtime parameter history")); layout->addWidget(history);
    connect(jobs, &QComboBox::currentIndexChanged, this, &RuntimeControlWidget::selectJob);
    connect(savedProposals, &QComboBox::currentIndexChanged, this, [this] {
        if (busy || submitted) return;
        const auto selected = savedProposals->currentData().toString();
        if (selected.isEmpty()) { invalidatePreview(); return; }
        const auto record = attempt.value("proposals").toObject().value(selected).toObject();
        if (record.isEmpty() || record.value("identity") != attempt.value("identity")) return;
        const auto after = record.value("parameters").toObject();
        const auto limits = after.value("actuatorLimits").toArray();
        if (limits.size() != 1) return;
        const QSignalBlocker a(gain), b(minimum), c(maximum), d(rate);
        gain->setValue(after.value("velocityGainNsm").toDouble());
        const auto force = limits.first().toObject();
        minimum->setValue(force.value("minN").toDouble()); maximum->setValue(force.value("maxN").toDouble());
        rate->setValue(force.value("rateNPerS").toDouble());
        showProposal(record);
    });
    for (auto *spin : {gain, minimum, maximum, rate})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { if (!submitted) invalidatePreview(); });
    connect(refreshButton, &QPushButton::clicked, this, &RuntimeControlWidget::refresh);
    connect(prepare, &QPushButton::clicked, this, [this] {
        if (!prepare->isEnabled()) return;
        const auto limits = attempt.value("acceptedState").toObject().value("parameters").toObject().value("actuatorLimits").toArray();
        if (limits.size() != 1 || minimum->value() > maximum->value()) {
            status->setText(tr("Minimum force must not exceed maximum force.")); return;
        }
        invalidatePreview(); busy = true; updateActions();
        send("runtime.parameters.preview", {{"jobId", jobId()}, {"velocityGainNsm", gain->value()},
            {"actuatorLimits", QJsonArray{QJsonObject{{"actuatorId", limits.first().toObject().value("actuatorId")},
                {"minN", minimum->value()}, {"maxN", maximum->value()}, {"rateNPerS", rate->value()}}}}}, "preview");
    });
    connect(approve, &QPushButton::clicked, this, [this] {
        if (!approve->isEnabled()) return;
        busy = true; updateActions();
        send("host.runtime.parameters.approve", {{"jobId", jobId()}, {"proposalId", proposal.value("proposalId")}}, "approve");
    });
    connect(cancelUpdate, &QPushButton::clicked, this, [this] {
        if (!cancelUpdate->isEnabled()) return;
        send("runtime.parameters.cancel", {{"jobId", jobId()}, {"proposalId", proposal.value("proposalId")}}, "lookup");
        status->setText(tr("Update cancellation requested; waiting for the native receipt."));
    });
    for (auto item : {qMakePair(pause, QString("pause")), qMakePair(resume, QString("resume")), qMakePair(cancel, QString("cancel"))})
        connect(item.first, &QPushButton::clicked, this, [this, action = item.second] {
            send("runtime.execution." + action, {{"jobId", jobId()}}, "execution");
            status->setText(tr("Run command requested; waiting for native observation."));
        });
    connect(transport, &WorkflowProcess::readyChanged, this, [this](bool ready) {
        ++generation; pending.clear(); jobs->clear(); selectJob();
        status->setText(ready ? tr("Workspace connected. Select a running native job.") : tr("Workflow unavailable; pending approvals cleared."));
        if (ready) refresh();
    });
    connect(transport, &WorkflowProcess::replyReceived, this, [this](const QString &id, const QString &, const QJsonObject &result) {
        if (!pending.contains(id)) return;
        const auto route = pending.take(id);
        if (!transport->isReady() || route.generation != generation || route.workspace != transport->configuration().workspace) return;
        if (route.purpose != "jobs" && route.jobId != jobId()) return;
        if (route.purpose == "jobs") {
            const QString selected = jobId(); const QSignalBlocker blocker(jobs);
            jobs->clear();
            for (const auto &entry : result.value("jobs").toArray()) {
                const auto job = entry.toObject();
                const auto jobKey = job.value("jobId").toString(); if (jobKey.isEmpty()) continue;
                const auto attempts = job.value("runtimeControl").toObject().value("attempts").toObject();
                bool ready = false;
                for (auto it = attempts.begin(); it != attempts.end(); ++it) ready |= it.value().toObject().value("status") == "ready";
                if ((job.value("state") != "running" || !ready) && (jobKey != selected || attempts.isEmpty())) continue;
                jobs->addItem(tr("Native job %1 (%2)").arg(jobKey, job.value("state").toString()), jobKey);
                jobs->setItemData(jobs->count()-1, job, Qt::UserRole+1);
                if (jobs->count() >= 128) break;
            }
            const int index = jobs->findData(selected); if (index >= 0) jobs->setCurrentIndex(index);
            if (jobId() != selected) selectJob();
            else if (!jobId().isEmpty()) send("runtime.parameters.query", {{"jobId", jobId()}}, "state");
        } else if (route.purpose == "state") observe(result);
        else if (route.purpose == "lookup") {
            if (route.identity == fingerprint(attempt.value("identity").toObject())
                && route.proposalId == proposal.value("proposalId").toString()) showProposal(result);
        } else if (route.purpose == "execution") refresh();
        else {
            if (route.binding != binding || route.proposalId != proposal.value("proposalId").toString()) return;
            busy = false;
            if (route.purpose == "preview") {
                if (result.value("identity").toObject() != attempt.value("identity").toObject()
                    || result.value("beforeParameters").toObject() != attempt.value("acceptedState").toObject().value("parameters").toObject()) {
                    updateActions(); return;
                }
                showProposal(result);
            } else if (route.purpose == "approve") {
                if (result.value("proposalId") != proposal.value("proposalId")
                    || result.value("previewSha256") != proposal.value("previewSha256")
                    || result.value("approvalToken").toString().isEmpty()) { invalidatePreview(); return; }
                busy = true; submitted = true;
                send("runtime.parameters.submit", {{"jobId", jobId()}, {"proposalId", proposal.value("proposalId")},
                    {"approvalToken", result.value("approvalToken")}}, "submit");
            } else if (route.purpose == "submit") {
                status->setText(tr("Submission recorded; waiting for an accepted native receipt."));
                send("runtime.parameters.query", {{"jobId", jobId()}, {"proposalId", proposal.value("proposalId")}}, "lookup");
            }
        }
        updateActions();
    });
    connect(transport, &WorkflowProcess::requestFailed, this, [this](const QString &id, const QString &, const QString &, const QString &) {
        if (!pending.contains(id)) return;
        const auto route = pending.take(id);
        if (route.generation != generation || route.workspace != transport->configuration().workspace || route.jobId != jobId()) return;
        busy = false; status->setText(tr("Runtime request failed. Refresh the native state before retrying."));
        if (route.purpose == "approve" || route.purpose == "preview") invalidatePreview();
        updateActions();
    });
    timer.setInterval(1000); connect(&timer, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); }); timer.start();
    updateActions(); if (transport->isReady()) refresh();
}

QString RuntimeControlWidget::jobId() const { return jobs->currentData().toString(); }
void RuntimeControlWidget::send(const QString &operation, const QJsonObject &parameters, const QString &purpose)
{
    if (!transport->isReady()) return;
    for (const auto &route : std::as_const(pending)) if (route.purpose == purpose) return;
    const auto id = transport->request(operation, parameters);
    pending.insert(id, {transport->configuration().workspace, purpose, jobId(), proposal.value("proposalId").toString(), binding,
        fingerprint(attempt.value("identity").toObject()), generation});
}
void RuntimeControlWidget::refresh()
{
    if (!transport->isReady()) return;
    send("jobs.query", {}, "jobs");
    if (submitted && !proposal.isEmpty()) send("runtime.parameters.query", {{"jobId", jobId()}, {"proposalId", proposal.value("proposalId")}}, "lookup");
}
void RuntimeControlWidget::selectJob()
{
    ++generation; pending.clear(); attempt = {}; binding.clear(); draftLoaded = false; connected = false;
    submitted = false; invalidatePreview(); metadata->clear(); history->clear();
    { const QSignalBlocker blocker(savedProposals); savedProposals->clear();
      savedProposals->addItem(tr("Select a saved parameter update"), QString()); }
    execution->setText(tr("Native execution state unavailable"));
    if (!jobId().isEmpty()) send("runtime.parameters.query", {{"jobId", jobId()}}, "state");
    updateActions();
}
void RuntimeControlWidget::invalidatePreview()
{
    if (submitted) return;
    proposal = {}; busy = false; preview->clear();
    { const QSignalBlocker blocker(savedProposals); savedProposals->setCurrentIndex(0); }
    // Discard in-flight preview and approval callbacks when the user edits the draft.
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->purpose == "preview" || it->purpose == "approve") it = pending.erase(it); else ++it;
    }
    updateActions();
}
void RuntimeControlWidget::updateActions()
{
    const bool live = transport->isReady() && connected && !attempt.isEmpty() && attempt.value("status") == "ready";
    prepare->setEnabled(live && !busy && !submitted);
    approve->setEnabled(live && !busy && !submitted && proposal.value("status") == "previewed");
    savedProposals->setEnabled(!busy && !submitted && !attempt.isEmpty());
    cancelUpdate->setEnabled(live && !proposal.isEmpty() &&
        (proposal.value("status") == "previewed" || proposal.value("status") == "intent" ||
         proposal.value("status") == "queued" || proposal.value("status") == "frozen"));
    for (auto *button : {pause, resume, cancel}) button->setEnabled(live);
    for (auto *spin : {gain, minimum, maximum, rate}) spin->setEnabled(live && !busy && !submitted);
}
void RuntimeControlWidget::observe(const QJsonObject &result)
{
    if (result.value("jobId").toString() != jobId()) return;
    const auto attempts = result.value("runtimeControl").toObject().value("attempts").toObject();
    QJsonObject next;
    for (auto it = attempts.begin(); it != attempts.end(); ++it) {
        const auto candidate = it.value().toObject();
        if (!attempt.isEmpty() && candidate.value("identity") == attempt.value("identity")) next = candidate;
        if (candidate.value("status") == "ready") { next = candidate; break; }
    }
    if (next.isEmpty()) { connected = false; invalidatePreview(); updateActions(); return; }
    const auto parameters = next.value("acceptedState").toObject().value("parameters").toObject();
    const auto identity = next.value("identity").toObject();
    if (!attempt.isEmpty() && identity != attempt.value("identity").toObject()) {
        submitted = false; invalidatePreview(); draftLoaded = false;
    }
    const QString newBinding = fingerprint(QJsonObject{{"identity", identity}, {"parameters", parameters}});
    if (binding != newBinding) { if (!submitted) invalidatePreview(); binding = newBinding; }
    attempt = next; connected = result.value("connected").toBool();
    if (result.value("jobState").isString() && jobs->currentIndex() >= 0)
        jobs->setItemText(jobs->currentIndex(), tr("Native job %1 (%2)").arg(jobId(), result.value("jobState").toString()));
    {
        const QSignalBlocker blocker(savedProposals);
        const QString selected = proposal.value("proposalId").toString();
        savedProposals->clear(); savedProposals->addItem(tr("Select a saved parameter update"), QString());
        const auto records = next.value("proposals").toObject();
        for (auto it = records.begin(); it != records.end(); ++it) {
            const auto record = it.value().toObject();
            if (record.value("identity") != identity) continue;
            savedProposals->addItem(tr("%1 — gain %2 N*s/m (%3)")
                .arg(record.value("createdAt").toString(),
                     value(record.value("parameters").toObject(), "velocityGainNsm"),
                     record.value("status").toString()), it.key());
        }
        const int savedIndex = savedProposals->findData(selected);
        if (savedIndex >= 0) savedProposals->setCurrentIndex(savedIndex);
    }
    metadata->setText(tr("Source SHA256: %1\nRun: %2\nBase controller: %3\nCurrent parameter revision: %4\nParameter SHA256: %5\nRuntime controller: %6")
        .arg(identity.value("modelSha256").toString(), identity.value("runId").toString(), identity.value("baseControllerRevision").toString(),
             value(parameters, "revision"), parameters.value("parameterSha256").toString(), parameters.value("runtimeControllerRevision").toString()));
    const auto limits = parameters.value("actuatorLimits").toArray();
    if (!draftLoaded && limits.size() == 1) {
        const QSignalBlocker a(gain), b(minimum), c(maximum), d(rate);
        gain->setValue(parameters.value("velocityGainNsm").toDouble());
        const auto force = limits.first().toObject(); minimum->setValue(force.value("minN").toDouble());
        maximum->setValue(force.value("maxN").toDouble()); rate->setValue(force.value("rateNPerS").toDouble()); draftLoaded = true;
    }
    QStringList accepted;
    for (const auto &entry : next.value("acceptedState").toObject().value("history").toArray()) {
        const auto change = entry.toObject(); accepted << tr("Revision %1: accepted sequence %2, time %3 s; SHA256 %4")
            .arg(value(change, "revision"), value(change, "appliedSeq"), value(change, "acceptedTimeS"), change.value("parameterSha256").toString());
    }
    history->setPlainText(accepted.join('\n'));
    const auto native = result.value("execution").toObject();
    if (native.value("availability") == "available")
        execution->setText(tr("Last native boundary: %1; accepted sequence %2, time %3 s")
            .arg(native.value("state").toString(), value(native, "acceptedSeq"), value(native, "timeS")));
    else execution->setText(tr("Native execution state unavailable"));
    if (!connected) invalidatePreview();
    updateActions();
}
void RuntimeControlWidget::showProposal(const QJsonObject &record)
{
    if (record.value("proposalId").toString().isEmpty()) return;
    proposal = record;
    const auto before = record.value("beforeParameters").toObject(), after = record.value("parameters").toObject();
    QStringList lines;
    lines << (record.value("status") == "accepted"
        ? tr("Accepted runtime update %1 → %2").arg(value(before, "revision"), value(after, "revision"))
        : tr("Runtime update %1 → %2 (saved preview; explicit approval required)").arg(value(before, "revision"), value(after, "revision")));
    lines << tr("Velocity gain [N*s/m]: %1 → %2").arg(value(before, "velocityGainNsm"), value(after, "velocityGainNsm"));
    const auto oldLimits = before.value("actuatorLimits").toArray(), newLimits = after.value("actuatorLimits").toArray();
    if (oldLimits.size() == 1 && newLimits.size() == 1) {
        const auto oldForce = oldLimits.first().toObject(), newForce = newLimits.first().toObject();
        lines << tr("Actuator: %1").arg(newForce.value("actuatorId").toString());
        for (const auto &key : {QString("minN"), QString("maxN"), QString("rateNPerS")})
            lines << tr("%1 [%2]: %3 → %4").arg(key, key == "rateNPerS" ? "N/s" : "N", value(oldForce, key), value(newForce, key));
    }
    lines << tr("Parameter SHA256: %1 → %2").arg(before.value("parameterSha256").toString(), after.value("parameterSha256").toString());
    preview->setPlainText(lines.join('\n'));
    const auto receipt = record.value("receipt").toObject();
    if (record.value("status") == "accepted" && receipt.value("status") == "accepted"
        && record.value("identity") == attempt.value("identity")
        && receipt.value("proposalId") == record.value("proposalId")
        && receipt.value("parameterSha256") == after.value("parameterSha256")
        && receipt.value("runtimeControllerRevision") == after.value("runtimeControllerRevision")
        && receipt.value("parameterRevision") == after.value("revision")
        && receipt.value("acceptedTimeS").isDouble() && std::isfinite(receipt.value("acceptedTimeS").toDouble())
        && receipt.value("acceptedTimeS").toDouble() >= 0 && receipt.contains("appliedSeq")) {
        status->setText(tr("Accepted at sequence %1, time %2 s; parameter revision %3; SHA256 %4; runtime controller %5")
            .arg(value(receipt, "appliedSeq"), value(receipt, "acceptedTimeS"), value(receipt, "parameterRevision"),
                 receipt.value("parameterSha256").toString(), receipt.value("runtimeControllerRevision").toString()));
        submitted = false;
    } else status->setText(tr("Proposal state: %1. Native acceptance requires an accepted receipt.").arg(record.value("status").toString()));
    if (record.value("status") == "rejected" || record.value("status") == "cancelled") submitted = false;
    updateActions();
}
