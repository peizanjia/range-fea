#include "series_workbench_widget.h"
#include "workflow_process.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
bool closed(const QJsonObject &o, const QStringList &required, const QStringList &optional = {})
{
    for (const auto &k : required) if (!o.contains(k)) return false;
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!required.contains(it.key()) && !optional.contains(it.key())) return false;
    return true;
}
bool textValue(const QJsonValue &v, int limit = 512, bool nonempty = true)
{ return v.isString() && v.toString().size() <= limit && (!nonempty || !v.toString().isEmpty()); }
bool sha(const QJsonValue &v)
{ return v.isString() && QRegularExpression("^[a-f0-9]{64}$").match(v.toString()).hasMatch(); }
bool integer(const QJsonValue &v, double lo, double hi)
{ return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi; }
bool selector(const QJsonObject &s, bool pinned)
{
    const auto kind = s.value("kind").toString();
    QStringList required{"kind", "jobId", "mode", "artifactId"};
    if (kind != "energy") required << "channelId";
    if (kind == "reference") required << "referenceSha256";
    if (!closed(s, required, {"sourceHash"}) || !QStringList{"native", "reference", "energy"}.contains(kind)
        || !QStringList{"active", "passive"}.contains(s.value("mode").toString())) return false;
    for (const auto &k : {"jobId", "artifactId"}) if (!textValue(s.value(k), 256)) return false;
    if (kind != "energy" && !textValue(s.value("channelId"), 256)) return false;
    if (kind == "reference" && (s.value("mode") != "active" || !sha(s.value("referenceSha256")))) return false;
    return (!pinned || s.contains("sourceHash")) && (!s.contains("sourceHash") || sha(s.value("sourceHash")));
}
QJsonObject bare(QJsonObject s) { s.remove("sourceHash"); return s; }
QString identity(const QJsonObject &s)
{ return QString::fromUtf8(QJsonDocument(bare(s)).toJson(QJsonDocument::Compact)); }
bool metadata(const QJsonObject &m)
{
    if (!closed(m, {"selector", "sourceKind", "modelSha256", "manifestSha256", "sourceHash", "rawSampleCount", "channel", "proofScope"},
        {"referenceSha256", "derived", "derivation", "physicalChannelIds", "underlyingSourceKind", "linearSha256", "matrixSha256"})
        || !m.value("selector").isObject() || !selector(m.value("selector").toObject(), true)
        || !sha(m.value("sourceHash")) || !sha(m.value("manifestSha256")) || !sha(m.value("modelSha256"))
        || m.value("sourceHash") != m.value("selector").toObject().value("sourceHash")
        || !integer(m.value("rawSampleCount"), 4, 10000) || !textValue(m.value("proofScope"), 128)) return false;
    const auto s = m.value("selector").toObject(); const auto k = s.value("kind").toString();
    const auto expected = k == "native" ? "native-" + s.value("mode").toString() + "-fe"
        : k == "reference" ? QString("independent-reference") : QString("derived-mechanical-energy");
    if (m.value("sourceKind") != expected || !m.value("channel").isObject()) return false;
    const auto c = m.value("channel").toObject();
    if (!closed(c, {"channelId", "variableId", "quantity", "axis", "unit", "nodeTarget"}, {"nodePhysicalId"})
        || !textValue(c.value("channelId"), 256) || !textValue(c.value("unit"), 64)
        || !textValue(c.value("variableId"), 128) || !textValue(c.value("quantity"), 128)
        || !textValue(c.value("axis"), 128) || !textValue(c.value("nodeTarget"), 128)
        || (c.contains("nodePhysicalId") && !integer(c.value("nodePhysicalId"), 1, 4294967295.0))) return false;
    if (k != "energy" && s.value("channelId") != c.value("channelId")) return false;
    if (k == "reference" && (m.value("referenceSha256") != s.value("referenceSha256") || !sha(m.value("referenceSha256")))) return false;
    const auto proof = m.value("proofScope").toString();
    if ((k == "native" && !QStringList{"solver-and-declared-controller-runtime/v1", "verified-native-manifest/v1", "verified-native-result-manifest/v1"}.contains(proof))
        || (k == "reference" && !QStringList{"independent-matrix-newmark-reference/v1", "independent-matrix-newmark-accepted-schedule/v1"}.contains(proof))
        || (k == "energy" && proof != "derived-physical-dofs-independent-matrices/v1")) return false;
    if (k == "energy" && (m.value("derived") != true || c.value("unit") != "J"
        || !sha(m.value("linearSha256")) || !sha(m.value("matrixSha256")) || !textValue(m.value("derivation")))) return false;
    return true;
}
QString sourceLabel(const QJsonObject &s)
{ return s.value("kind").toString() + " / " + s.value("jobId").toString() + " / " + s.value("mode").toString() + " / " + s.value("channelId").toString("mechanical-energy"); }
}

bool SeriesWorkbenchWidget::parsePlotDto(const QJsonObject &dto, SeriesPlotWidget::Dataset &out, QString *error)
{
    const auto fail = [error] { if (error) *error = QStringLiteral("Invalid bounded series plot response; displayed data preserved."); return false; };
    if (QJsonDocument(dto).toJson(QJsonDocument::Compact).size() > 2*1024*1024
        || !closed(dto, {"schemaId", "methodVersion", "view", "title", "axes", "traces", "unavailable", "export"})
        || dto.value("schemaId") != "range.series-plot/v1" || dto.value("methodVersion") != "range.series-analysis/v1"
        || !textValue(dto.value("title")) || !dto.value("traces").isArray() || !dto.value("unavailable").isArray()) return fail();
    const auto view = dto.value("view").toString();
    if (!QStringList{"time", "error", "phase", "amplitude", "psd", "energy", "work"}.contains(view)) return fail();
    const auto axes = dto.value("axes").toObject();
    if (!closed(axes, {"x", "y"})) return fail();
    for (const auto &a : {"x", "y"}) {
        const auto axis = axes.value(a).toObject();
        if (!closed(axis, {"label", "unit"}) || !textValue(axis.value("label")) || !textValue(axis.value("unit"), 64)) return fail();
    }
    SeriesPlotWidget::Dataset candidate;
    candidate.title = dto.value("title").toString(); candidate.xLabel = axes.value("x").toObject().value("label").toString();
    candidate.xUnit = axes.value("x").toObject().value("unit").toString(); candidate.yLabel = axes.value("y").toObject().value("label").toString();
    candidate.yUnit = axes.value("y").toObject().value("unit").toString();
    candidate.viewKind = view == "phase" ? SeriesPlotWidget::ViewKind::Phase
        : (view == "amplitude" || view == "psd") ? SeriesPlotWidget::ViewKind::Spectrum : SeriesPlotWidget::ViewKind::Time;
    if ((candidate.viewKind == SeriesPlotWidget::ViewKind::Time && candidate.xUnit != "s")
        || (candidate.viewKind == SeriesPlotWidget::ViewKind::Spectrum && candidate.xUnit != "Hz")) return fail();
    const auto traces = dto.value("traces").toArray(); if (traces.size() > 4) return fail();
    QSet<QString> ids, authenticated;
    for (const auto &v : traces) {
        if (!v.isObject()) return fail(); const auto t = v.toObject();
        if (!closed(t, {"id", "label", "unit", "sources", "points", "rawSampleCount", "displaySampleCount", "displayOnly"},
            {"spectralBinCount", "window", "removeMean"}) || !textValue(t.value("id")) || !textValue(t.value("label"))
            || t.value("unit") != candidate.yUnit || t.value("displayOnly") != true
            || !integer(t.value("rawSampleCount"), 4, 10000) || !t.value("points").isArray() || !t.value("sources").isArray()) return fail();
        if (ids.contains(t.value("id").toString())) return fail(); ids.insert(t.value("id").toString());
        const auto points = t.value("points").toArray(); const auto sources = t.value("sources").toArray();
        if (points.size() > 1000 || !integer(t.value("displaySampleCount"), 0, 1000)
            || t.value("displaySampleCount").toInt() != points.size() || sources.isEmpty() || sources.size() > 2) return fail();
        if ((view == "phase" || view == "error") && (sources.size() != 2 || traces.size() != 1)) return fail();
        if (view != "phase" && view != "error" && sources.size() != 1) return fail();
        QStringList sourceIds;
        for (const auto &s : sources) {
            if (!s.isObject() || !metadata(s.toObject())) return fail();
            const auto m = s.toObject(); const auto unit = m.value("channel").toObject().value("unit").toString();
            const auto expected = view == "psd" ? "(" + unit + ")^2/Hz" : unit;
            if (view != "phase" && expected != candidate.yUnit) return fail();
            authenticated.insert(QString::fromUtf8(QJsonDocument(m.value("selector").toObject()).toJson(QJsonDocument::Compact)));
            sourceIds << sourceLabel(m.value("selector").toObject());
        }
        if (view == "phase" && (sources[0].toObject().value("channel").toObject().value("unit") != candidate.xUnit
            || sources[1].toObject().value("channel").toObject().value("unit") != candidate.yUnit)) return fail();
        int ordinalBound = t.value("rawSampleCount").toInt();
        if (candidate.viewKind == SeriesPlotWidget::ViewKind::Spectrum) {
            if (!integer(t.value("spectralBinCount"), 1, 10000) || !QStringList{"hann", "rectangular"}.contains(t.value("window").toString())
                || !t.value("removeMean").isBool()) return fail(); ordinalBound = t.value("spectralBinCount").toInt();
        } else if (t.contains("spectralBinCount") || t.contains("window") || t.contains("removeMean")) return fail();
        SeriesPlotWidget::Trace trace; trace.id = t.value("id").toString(); trace.label = t.value("label").toString();
        trace.unit = candidate.yUnit; trace.sourceId = sourceIds.join(" + ");
        trace.artifactSha256 = sources[0].toObject().value("sourceHash").toString();
        int previousOrdinal = -1; double previousX = -INFINITY;
        for (const auto &p : points) {
            if (!p.isObject()) return fail(); const auto point = p.toObject();
            if (!closed(point, {"x", "y", "ordinal"}) || !point.value("x").isDouble() || !point.value("y").isDouble()
                || !std::isfinite(point.value("x").toDouble()) || !std::isfinite(point.value("y").toDouble())
                || !integer(point.value("ordinal"), previousOrdinal + 1, ordinalBound-1)) return fail();
            const double x = point.value("x").toDouble();
            if (view != "phase" && x <= previousX) return fail();
            previousX = x; previousOrdinal = point.value("ordinal").toInt();
            trace.points.append(QPointF(x, point.value("y").toDouble())); trace.originalOrdinals.append(quint64(previousOrdinal));
        }
        candidate.traces.append(trace);
    }
    const auto unavailable = dto.value("unavailable").toArray(); if (unavailable.size() > 4) return fail();
    for (const auto &u : unavailable) {
        const auto o = u.toObject();
        if (!u.isObject() || !closed(o, {"source", "code", "message"}) || !selector(o.value("source").toObject(), false)
            || !textValue(o.value("code"), 128) || !textValue(o.value("message"), 1024)) return fail();
    }
    const auto e = dto.value("export").toObject();
    if (!closed(e, {"available", "scope", "sources"}) || !e.value("available").isBool()
        || e.value("scope") != "complete-raw" || !e.value("sources").isArray()) return fail();
    const auto pins = e.value("sources").toArray(); if (pins.size() > 4 || e.value("available").toBool() != !pins.isEmpty()) return fail();
    for (const auto &p : pins) if (!p.isObject() || !selector(p.toObject(), true)
        || !authenticated.contains(QString::fromUtf8(QJsonDocument(p.toObject()).toJson(QJsonDocument::Compact)))) return fail();
    out = std::move(candidate); if (error) error->clear(); return true;
}

SeriesWorkbenchWidget::SeriesWorkbenchWidget(WorkflowProcess *process, QWidget *parent) : QWidget(parent), transport(process)
{
    setObjectName("seriesWorkbenchWidget"); setAccessibleName(tr("Authenticated time series workbench"));
    auto *outer = new QVBoxLayout(this); splitter = new QSplitter(Qt::Vertical); splitter->setObjectName("seriesWorkbenchSplitter"); outer->addWidget(splitter);
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); splitter->addWidget(scroll);
    auto *body = new QWidget; scroll->setWidget(body); auto *layout = new QVBoxLayout(body);
    auto *form = new QFormLayout; form->setRowWrapPolicy(QFormLayout::WrapLongRows); layout->addLayout(form);
    auto combo = [form](const QString &label, const char *name) {
        auto *c = new QComboBox; c->setObjectName(name); c->setAccessibleName(label);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); c->setMinimumContentsLength(8); form->addRow(label,c); return c;
    };
    jobs = combo(tr("Successful native job"), "seriesJobChoice"); modes = combo(tr("Native mode"), "seriesModeChoice");
    modes->addItem(tr("Active"), "active"); modes->addItem(tr("Passive"), "passive");
    channels = combo(tr("Registered channel"), "seriesChannelChoice"); views = combo(tr("View"), "seriesViewChoice");
    for (const auto &v : {"time", "error", "phase", "amplitude", "psd", "energy", "work"}) views->addItem(QString::fromLatin1(v), QString::fromLatin1(v));
    window = combo(tr("Spectrum window"), "seriesWindowChoice"); window->addItem("Hann", "hann"); window->addItem("Rectangular", "rectangular");
    removeMean = new QCheckBox(tr("Remove mean before spectrum")); removeMean->setObjectName("seriesRemoveMean"); removeMean->setChecked(true); layout->addWidget(removeMean);
    selected = new QListWidget; selected->setObjectName("seriesSourceSelection"); selected->setAccessibleName(tr("Selected sources, maximum four")); selected->setMaximumHeight(110); layout->addWidget(selected);
    auto *actions = new QGridLayout; layout->addLayout(actions);
    auto button = [actions](const QString &text, const char *name, int r, int c) { auto *b = new QPushButton(text); b->setObjectName(name); actions->addWidget(b,r,c); return b; };
    connect(button(tr("Add native / derived"), "seriesAddNative",0,0), &QPushButton::clicked,this,[this]{addSource(false);});
    connect(button(tr("Add reference"), "seriesAddReference",0,1), &QPushButton::clicked,this,[this]{addSource(true);});
    connect(button(tr("Remove selected"), "seriesRemoveSource",1,0), &QPushButton::clicked,this,[this]{invalidate(); delete selected->takeItem(selected->currentRow());});
    connect(button(tr("Query plot"), "seriesQuery",1,1), &QPushButton::clicked,this,&SeriesWorkbenchWidget::queryPlot);
    connect(button(tr("Refresh sources"), "seriesRefresh",2,0), &QPushButton::clicked,this,&SeriesWorkbenchWidget::refresh);
    connect(button(tr("Save layout"), "seriesSaveLayout",3,0), &QPushButton::clicked,this,&SeriesWorkbenchWidget::saveLayout);
    connect(button(tr("Restore layout"), "seriesRestoreLayout",3,1), &QPushButton::clicked,this,&SeriesWorkbenchWidget::restoreLayout);
    status = new QLabel(tr("Select registered sources and query a view.")); status->setObjectName("seriesStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    caption = new QLabel; caption->setObjectName("seriesDisplayedSources"); caption->setTextFormat(Qt::PlainText); caption->setWordWrap(true); layout->addWidget(caption);
    auto *ordinalHelp = new QLabel(tr("Hover ordinal: zero-based raw sample index; spectra use frequency-bin index. It is independent of native recordNo. Error/phase require two sources on an identical verified time grid; no interpolation."));
    ordinalHelp->setWordWrap(true); ordinalHelp->setTextFormat(Qt::PlainText); layout->addWidget(ordinalHelp);
    plot = new SeriesPlotWidget; splitter->addWidget(plot); splitter->setStretchFactor(1,1);
    connect(jobs,&QComboBox::currentIndexChanged,this,&SeriesWorkbenchWidget::selectCatalog);
    connect(modes,&QComboBox::currentIndexChanged,this,&SeriesWorkbenchWidget::selectCatalog);
    connect(views,&QComboBox::currentIndexChanged,this,[this]{invalidate();});
    connect(window,&QComboBox::currentIndexChanged,this,[this]{invalidate();});
    connect(removeMean,&QCheckBox::toggled,this,[this]{invalidate();});
    connect(plot, &SeriesPlotWidget::displayHoldChanged,this,[this](bool held){
        if (!held && pendingDisplay) {
            displayed = *pendingDisplay; displayedOptions = pendingOptions.value_or(QJsonObject{});
            pendingDisplay.reset(); pendingOptions.reset(); updateDisplayMetadata();
        }
    });
    connect(plot,&SeriesPlotWidget::exportFullSeriesRequested,this,[this]{
        if (!transport->isReady() || exportFile) return;
        const auto destination = QFileDialog::getSaveFileName(this,tr("Export complete raw series"),{},tr("CSV files (*.csv)"));
        if (!destination.isEmpty()) exportToFile(destination);
    });
    connect(transport,&WorkflowProcess::readyChanged,this,[this](bool ready){
        const auto now = QDir(transport->configuration().workspace).absolutePath();
        const bool changed = now != workspace; workspace = now; invalidate(changed);
        reconnectValidation = ready && !changed && !displayed.isEmpty();
        if (changed) { projectId.clear(); layoutKey.clear(); catalog = {}; const QSignalBlocker b(jobs); jobs->clear(); channels->clear(); }
        plot->setFullExportAvailable(ready && !displayed.isEmpty() && displayed.value("export").toObject().value("available").toBool());
        if (ready) refresh();
    });
    connect(transport,&WorkflowProcess::replyReceived,this,[this](const QString &id,const QString &,const QJsonObject &result){
        if (!requests.contains(id)) return; const auto route = requests.take(id);
        if (!transport->isReady() || route.generation != generation || route.workspace != transport->configuration().workspace) return;
        if (route.purpose == "catalog" && (route.job != jobId() || route.mode != mode())) return;
        receive(route,result);
    });
    connect(transport,&WorkflowProcess::requestFailed,this,[this](const QString &id,const QString &,const QString &code,const QString &message){
        if (!requests.contains(id)) return; const auto route = requests.take(id);
        if (route.generation != generation || route.workspace != transport->configuration().workspace) return;
        if (route.purpose.startsWith("export")) abortExport(code + ": " + message);
        else status->setText(code + ": " + message);
    });
    if (transport->isReady()) { workspace = QDir(transport->configuration().workspace).absolutePath(); refresh(); }
}
SeriesWorkbenchWidget::~SeriesWorkbenchWidget() = default;
QString SeriesWorkbenchWidget::jobId() const { return jobs->currentData().toString(); }
QString SeriesWorkbenchWidget::mode() const { return modes->currentData().toString(); }
QJsonArray SeriesWorkbenchWidget::sourceRefs() const
{ QJsonArray a; for (int i=0;i<selected->count();++i) a.append(selected->item(i)->data(Qt::UserRole).toJsonObject()); return a; }
void SeriesWorkbenchWidget::send(const QString &op,const QJsonObject &p,const QString &purpose)
{
    if (!transport->isReady()) return;
    const auto id = transport->request(op,p);
    requests.insert(id,{transport->configuration().workspace,purpose,jobId(),mode(),generation,p.value("sources").toArray(),p.value("view").toString(),p.value("options").toObject()});
}
void SeriesWorkbenchWidget::invalidate(bool workspaceChanged)
{
    ++generation; requests.clear(); abortExport({});
    restoreRange.reset(); restoreCursor.reset(); restoreHold = false;
    if (workspaceChanged) {
        pendingDisplay.reset(); pendingOptions.reset(); displayed = {}; displayedOptions = {}; restorePins = {};
        plot->setDisplayHeld(false); SeriesPlotWidget::Dataset empty; empty.title = tr("Workspace changed"); plot->setDataset(empty);
        selected->clear(); updateDisplayMetadata();
    }
}
void SeriesWorkbenchWidget::refresh()
{
    if (!transport->isReady()) return;
    send("project.query",{},"project"); send("jobs.query",{},"jobs");
}
void SeriesWorkbenchWidget::selectCatalog()
{
    invalidate(); catalog = {}; const auto previous = channels->currentData().toJsonObject();
    channels->clear(); channels->setProperty("previousSelector",previous);
    if (!jobId().isEmpty()) send("result.plot.sources.query",{{"jobId",jobId()},{"mode",mode()}},"catalog");
}
void SeriesWorkbenchWidget::applyCatalog(const QJsonObject &result)
{
    if (!closed(result,{"schemaId","jobId","mode","artifactId","channels","derived","unavailable"})
        || result.value("schemaId") != "range.series-source-catalog/v1" || result.value("jobId") != jobId()
            || result.value("mode") != mode() || !result.value("unavailable").isArray() || !result.value("channels").isArray() || !result.value("derived").isArray()
        || result.value("channels").toArray().size() > 2048 || result.value("derived").toArray().size() > 4) {
        status->setText(tr("Invalid source catalog; selection preserved.")); return;
    }
    const auto previous = channels->currentData().toJsonObject().isEmpty() ? channels->property("previousSelector").toJsonObject() : channels->currentData().toJsonObject();
    QJsonArray all = result.value("channels").toArray(); for (const auto &d : result.value("derived").toArray()) all.append(d);
    for (const auto &v : all) {
        const auto row = v.toObject(); const auto s = row.value("selector").toObject();
        if (!v.isObject() || !closed(row,{"selector","label","unit"},{"quantity","reference"}) || !selector(s,false)
            || s.value("jobId") != jobId() || s.value("mode") != mode() || s.value("artifactId") != result.value("artifactId")
            || !textValue(row.value("label")) || !textValue(row.value("unit"),64)
            || (row.contains("reference") && !selector(row.value("reference").toObject(),false))) {
            status->setText(tr("Invalid channel/reference selector; selection preserved.")); return;
        }
    }
    catalog = result; const QSignalBlocker b(channels); channels->clear(); int restore = -1;
    for (const auto &v : all) {
        const auto row = v.toObject(); const auto s = row.value("selector").toObject();
        channels->addItem(row.value("label").toString() + " [" + row.value("unit").toString() + "]",s);
        channels->setItemData(channels->count()-1,row,Qt::UserRole+1);
        if (identity(s) == identity(previous)) restore = channels->count()-1;
    }
    if (!previous.isEmpty()) channels->setCurrentIndex(restore); // Never swap an unavailable reference by list position.
    status->setText(tr("Catalog refreshed; edited source selection preserved. Query validates saved identities."));
    for (const auto &u : result.value("unavailable").toArray()) status->setText(status->text()+"\n"+u.toObject().value("message").toString());
}
void SeriesWorkbenchWidget::addSource(bool reference)
{
    if (selected->count() >= 4) { status->setText(tr("At most four authenticated sources may be selected.")); return; }
    const auto row = channels->currentData(Qt::UserRole+1).toJsonObject();
    const auto s = bare(row.value(reference ? "reference" : "selector").toObject());
    if (!selector(s,false)) { status->setText(tr("This channel has no registered source of the requested kind.")); return; }
    for (const auto &v : sourceRefs()) if (identity(v.toObject()) == identity(s)) return;
    invalidate(); auto *item = new QListWidgetItem(sourceLabel(s),selected); item->setData(Qt::UserRole,s);
}
void SeriesWorkbenchWidget::queryPlot()
{
    const auto sources = sourceRefs(); const auto view = views->currentData().toString();
    if (sources.isEmpty() || sources.size() > 4 || ((view == "error" || view == "phase") && sources.size() != 2)) {
        status->setText(tr("Select one to four sources; error and phase require exactly two.")); return;
    }
    invalidate(); send("result.plot.query",{{"view",view},{"sources",sources},
        {"options",QJsonObject{{"limit",1000},{"window",window->currentData().toString()},{"removeMean",removeMean->isChecked()}}}},"plot");
}
void SeriesWorkbenchWidget::adopt(const QJsonObject &dto, const QJsonObject &options, bool restoring)
{
    SeriesPlotWidget::Dataset dataset; QString error;
    if (!parsePlotDto(dto,dataset,&error)) { status->setText(error); return; }
    if (restoring) {
        pendingDisplay.reset(); pendingOptions.reset();
        plot->setDisplayHeld(false);
    }
    if (!plot->setDataset(dataset,&error)) { status->setText(error); return; }
    if (plot->isDisplayHeld()) {
        pendingDisplay = dto; pendingOptions = options;
        status->setText(tr("Latest verified result pending; displayed sources and export remain held."));
    } else {
        displayed = dto; displayedOptions = options; pendingDisplay.reset(); pendingOptions.reset(); updateDisplayMetadata();
    }
    if (restoring) {
        if (restoreRange) { plot->setVisibleRange(*restoreRange); restoreRange.reset(); }
        if (restoreCursor) { plot->setCursor(*restoreCursor); restoreCursor.reset(); }
        plot->setDisplayHeld(restoreHold); restoreHold = false;
    }
}
void SeriesWorkbenchWidget::updateDisplayMetadata()
{
    QStringList lines;
    for (const auto &v : displayed.value("traces").toArray()) for (const auto &s : v.toObject().value("sources").toArray()) {
        const auto m = s.toObject(); lines << sourceLabel(m.value("selector").toObject()) + " / " + m.value("sourceKind").toString()
            + " / " + m.value("channel").toObject().value("unit").toString() + " / SHA-256 " + m.value("sourceHash").toString()
            + " / proof " + m.value("proofScope").toString();
    }
    caption->setText(lines.join('\n'));
    plot->setFullExportAvailable(transport->isReady() && displayed.value("export").toObject().value("available").toBool());
    QStringList diagnostic;
    for (const auto &u : displayed.value("unavailable").toArray()) diagnostic << u.toObject().value("code").toString()+": "+u.toObject().value("message").toString();
    status->setText(diagnostic.isEmpty() ? tr("Displayed verified bounded samples; export includes complete raw histories.") : diagnostic.join('\n'));
}
void SeriesWorkbenchWidget::receive(const Pending &route,const QJsonObject &result)
{
    if (route.purpose == "project") {
        const auto id = result.value("projectId").toString(); if (id.isEmpty()) return;
        if (!projectId.isEmpty() && projectId != id) { invalidate(true); }
        projectId = id; const auto canonical = QFileInfo(workspace).canonicalFilePath();
        if (canonical.isEmpty()) { layoutKey.clear(); return; }
        layoutKey = "seriesWorkbench/v1/" + QString::fromLatin1(QCryptographicHash::hash((canonical+"\n"+projectId).toUtf8(),QCryptographicHash::Sha256).toHex());
    } else if (route.purpose == "jobs") {
        const auto previous = jobId(); const QSignalBlocker b(jobs); jobs->clear();
        for (const auto &v : result.value("jobs").toArray()) {
            const auto j = v.toObject(); const auto command = j.value("command").toString();
            if (j.value("state") != "succeeded" || !QStringList{"workflow.run","workflow.resume","workflow.recover"}.contains(command) || !textValue(j.value("jobId"),256)) continue;
            jobs->addItem(j.value("jobId").toString(),j.value("jobId").toString()); if (jobs->count() >= 128) break;
        }
        if (!previous.isEmpty()) jobs->setCurrentIndex(jobs->findData(previous));
        if (jobId() != previous) selectCatalog();
        else if (!jobId().isEmpty()) send("result.plot.sources.query",{{"jobId",jobId()},{"mode",mode()}},"catalog");
    } else if (route.purpose == "catalog") {
        applyCatalog(result);
        if (reconnectValidation) {
            reconnectValidation = false;
            const auto pins = displayed.value("export").toObject().value("sources").toArray();
            if (!pins.isEmpty()) send("result.plot.query",{{"sources",pins},{"view",displayed.value("view")},
                {"options",displayedOptions}},"plotRevalidate");
        }
    }
    else if (route.purpose == "plot" || route.purpose == "plotRestore" || route.purpose == "plotRevalidate") {
        if (result.value("view") != route.view) { status->setText(tr("Plot view identity mismatch.")); return; }
        // Every returned source must match a requested identity (energy view may derive its fixed selector).
        for (const auto &t : result.value("traces").toArray()) for (const auto &m : t.toObject().value("sources").toArray()) {
            const auto returned = m.toObject().value("selector").toObject(); const auto actual = bare(returned); bool found = false;
            for (const auto &s : route.sources) {
                auto expected = bare(s.toObject());
                if (route.view == "energy" && expected.value("kind") == "native") { expected.remove("channelId"); expected.insert("kind","energy"); }
                const bool workFromNodal = route.view == "work" && expected.value("kind") == "native"
                    && expected.value("mode") == "active" && expected.value("channelId") != "control:work";
                if (workFromNodal) expected.insert("channelId", "control:work");
                const bool derivedFromNative = (route.view == "energy" && s.toObject().value("kind") == "native") || workFromNodal;
                const bool exactPin = !s.toObject().contains("sourceHash") || derivedFromNative
                    || returned.value("sourceHash") == s.toObject().value("sourceHash");
                found |= expected == actual && exactPin;
            }
            if (!found) { status->setText(tr("Plot source identity mismatch; display preserved.")); return; }
        }
        if (route.view == "amplitude" || route.view == "psd") {
            for (const auto &t : result.value("traces").toArray()) {
                const auto trace = t.toObject();
                if (trace.value("window") != route.options.value("window") || trace.value("removeMean") != route.options.value("removeMean")) {
                    status->setText(tr("Plot analysis options mismatch; display preserved.")); return;
                }
            }
        }
        adopt(result,route.options,route.purpose == "plotRestore");
    } else if (route.purpose == "export") {
        if (!exportFile || exportGeneration != generation || !closed(result,{"schemaId","methodVersion","exportId","scope","sha256","bytes","count","sources"})
            || result.value("schemaId") != "range.series-export/v1" || result.value("scope") != "complete-raw" || !sha(result.value("sha256"))
            || !textValue(result.value("exportId"),128) || !integer(result.value("bytes"),1,32*1024*1024)
            || !integer(result.value("count"),4,40000) || !result.value("sources").isArray()) { abortExport(tr("Invalid raw export manifest.")); return; }
        const auto metas = result.value("sources").toArray();
        if (metas.size() != exportSources.size()) { abortExport(tr("Export sources differ from displayed sources.")); return; }
        qint64 count = 0;
        for (int i=0;i<metas.size();++i) {
            if (!metadata(metas[i].toObject()) || metas[i].toObject().value("selector") != exportSources[i]) { abortExport(tr("Export identity mismatch.")); return; }
            count += metas[i].toObject().value("rawSampleCount").toInt();
        }
        if (count != result.value("count").toInt()) { abortExport(tr("Export raw count mismatch.")); return; }
        exportManifest = result; nextChunk();
    } else if (route.purpose == "exportChunk") {
        if (!exportFile || exportGeneration != generation) return;
        const qint64 remaining = exportManifest.value("bytes").toInt()-exportOffset; const qint64 expectedLength = std::min<qint64>(65536,remaining);
        if (!closed(result,{"schemaId","exportId","sha256","bytes","offset","length","encoding","data","complete"})
            || result.value("schemaId") != "range.series-export-chunk/v1" || result.value("exportId") != exportManifest.value("exportId")
            || result.value("sha256") != exportManifest.value("sha256") || result.value("bytes") != exportManifest.value("bytes")
            || !integer(result.value("offset"),exportOffset,exportOffset) || !integer(result.value("length"),expectedLength,expectedLength)
            || result.value("encoding") != "base64" || !textValue(result.value("data"),87384)
            || !result.value("complete").isBool() || result.value("complete").toBool() != (remaining == expectedLength)) { abortExport(tr("Invalid bounded export chunk.")); return; }
        const auto encoded = result.value("data").toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() != expectedLength || decoded.decoded.toBase64() != encoded
            || exportFile->write(decoded.decoded) != expectedLength) { abortExport(tr("Export decoding or file write failed; destination preserved.")); return; }
        exportHash->addData(decoded.decoded); exportOffset += expectedLength;
        if (exportOffset == exportManifest.value("bytes").toInt()) {
            if (QString::fromLatin1(exportHash->result().toHex()) != exportManifest.value("sha256").toString()) { abortExport(tr("Complete CSV SHA-256 mismatch; destination preserved.")); return; }
            if (!exportFile->commit()) { abortExport(tr("Atomic CSV commit failed; destination preserved.")); return; }
            exportFile.reset(); exportHash.reset(); status->setText(tr("Exported %1 complete raw samples; SHA-256 verified.").arg(exportManifest.value("count").toInt()));
        } else nextChunk();
    }
}
void SeriesWorkbenchWidget::exportToFile(const QString &destination)
{
    if (!transport->isReady() || displayed.isEmpty() || exportFile || destination.isEmpty()
        || !displayed.value("export").toObject().value("available").toBool()) return;
    exportSources = displayed.value("export").toObject().value("sources").toArray();
    exportFile = std::make_unique<QSaveFile>(destination); exportFile->setDirectWriteFallback(false);
    if (!exportFile->open(QIODevice::WriteOnly)) { abortExport(tr("Cannot open atomic CSV destination; existing file preserved.")); return; }
    exportHash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256); exportOffset = 0; exportGeneration = generation; exportManifest = {};
    send("result.raw.export",{{"sources",exportSources},{"range","complete"}},"export");
}
void SeriesWorkbenchWidget::abortExport(const QString &message)
{
    if (exportFile) exportFile->cancelWriting(); exportFile.reset(); exportHash.reset(); exportManifest = {}; exportOffset = 0;
    if (!message.isEmpty()) status->setText(message);
}
void SeriesWorkbenchWidget::nextChunk()
{ send("result.raw.export.chunk",{{"exportId",exportManifest.value("exportId")},{"offset",double(exportOffset)},
    {"length",double(std::min<qint64>(65536,exportManifest.value("bytes").toInt()-exportOffset))}},"exportChunk"); }
void SeriesWorkbenchWidget::saveLayout()
{
    if (layoutKey.isEmpty()) { status->setText(tr("A canonical connected project is required to save layout.")); return; }
    const auto range = plot->visibleRange();
    QJsonObject saved{{"version",1},{"sourceRefs",sourceRefs()},{"lastPins",displayed.value("export").toObject().value("sources")},
        {"jobId",jobId()},{"mode",mode()},{"channel",channels->currentData().toJsonObject()}, {"view",views->currentData().toString()},
        {"window",window->currentData().toString()},{"removeMean",removeMean->isChecked()},{"hold",plot->isDisplayHeld()},
        {"displayedView",displayed.value("view")},{"displayedOptions",displayedOptions},
        {"splitter",QString::fromLatin1(splitter->saveState().toBase64())}};
    if (plot->hasDataset()) saved.insert("visibleRange",QJsonArray{range.xMinimum,range.xMaximum,range.yMinimum,range.yMaximum});
    if (const auto cursor = plot->cursor()) saved.insert("cursor", QJsonObject{{"traceId", cursor->traceId}, {"ordinal", QString::number(cursor->ordinal)}});
    QSettings settings; settings.setValue(layoutKey,QJsonDocument(saved).toJson(QJsonDocument::Compact)); settings.sync();
    status->setText(settings.status() == QSettings::NoError ? tr("Layout saved for this workspace and project.") : tr("Layout save failed."));
}
void SeriesWorkbenchWidget::restoreLayout()
{
    if (layoutKey.isEmpty()) return; QSettings settings;
    const auto bytes = settings.value(layoutKey).toByteArray(); if (bytes.size() > 65536) return;
    const auto saved = QJsonDocument::fromJson(bytes).object(); const auto refs = saved.value("sourceRefs").toArray();
    const auto pins = saved.value("lastPins").toArray();
    const auto displayView = saved.value("displayedView").toString();
    const auto displayOptions = saved.value("displayedOptions").toObject();
    if (saved.value("version") != 1 || refs.size() > 4 || !saved.value("sourceRefs").isArray()
        || !QStringList{"time","error","phase","amplitude","psd","energy","work"}.contains(saved.value("view").toString())
        || !QStringList{"hann","rectangular"}.contains(saved.value("window").toString())
        || !saved.value("removeMean").isBool() || !saved.value("hold").isBool()) { status->setText(tr("Invalid saved layout.")); return; }
    for (const auto &s : refs) if (!selector(s.toObject(),false)) { status->setText(tr("Invalid saved source identity.")); return; }
    if (pins.size() > 4 || (!pins.isEmpty() &&
        (!QStringList{"time","error","phase","amplitude","psd","energy","work"}.contains(displayView)
        || !closed(displayOptions,{"limit","window","removeMean"}) || displayOptions.value("limit") != 1000
        || !QStringList{"hann","rectangular"}.contains(displayOptions.value("window").toString())
        || !displayOptions.value("removeMean").isBool()))) {
        status->setText(tr("Saved display has no valid associated view and analysis options; selection preserved.")); return;
    }
    for (const auto &s : pins) if (!selector(s.toObject(),true)) { status->setText(tr("Invalid saved display hash.")); return; }
    invalidate(); selected->clear();
    for (const auto &s : refs) { auto *item = new QListWidgetItem(sourceLabel(s.toObject()),selected); item->setData(Qt::UserRole,bare(s.toObject())); }
    const QSignalBlocker j(jobs),m(modes),v(views),w(window),r(removeMean);
    jobs->setCurrentIndex(jobs->findData(saved.value("jobId").toString())); modes->setCurrentIndex(modes->findData(saved.value("mode").toString()));
    views->setCurrentIndex(views->findData(saved.value("view").toString())); window->setCurrentIndex(window->findData(saved.value("window").toString())); removeMean->setChecked(saved.value("removeMean").toBool());
    channels->setProperty("previousSelector",saved.value("channel").toObject());
    splitter->restoreState(QByteArray::fromBase64(saved.value("splitter").toString().toLatin1()));
    restorePins = pins; restoreHold = saved.value("hold").toBool();
    restoreCursor.reset();
    const auto cursor = saved.value("cursor").toObject();
    bool cursorOk = false; const auto ordinal = cursor.value("ordinal").toString().toULongLong(&cursorOk);
    if (cursorOk && QString::number(ordinal) == cursor.value("ordinal").toString() && cursor.value("traceId").toString().size() <= 512)
        restoreCursor = SeriesPlotWidget::Cursor{cursor.value("traceId").toString(), ordinal};
    const auto range = saved.value("visibleRange").toArray();
    if (range.size()==4 && std::all_of(range.begin(),range.end(),[](const QJsonValue &v){return v.isDouble() && std::isfinite(v.toDouble());}))
        restoreRange = SeriesPlotWidget::VisibleRange{range[0].toDouble(),range[1].toDouble(),range[2].toDouble(),range[3].toDouble()};
    if (!jobId().isEmpty()) send("result.plot.sources.query",{{"jobId",jobId()},{"mode",mode()}},"catalog");
    // Revalidate the last pinned display before any fresh query; stale identities remain selected.
    if (!restorePins.isEmpty()) send("result.plot.query",{{"sources",restorePins},{"view",displayView},
        {"options",displayOptions}},"plotRestore");
    status->setText(tr("Layout restored by stable source identity; validating saved display hashes."));
}
