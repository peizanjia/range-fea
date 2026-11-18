#include "mesh_sensitivity_dialog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>

#include <rbl_error.h>
#include <rml_file_utils.h>
#include <rmesh_sensitivity.h>
#include <rmesh_adaptivity.h>
#include <rmodel_command_service.h>

namespace
{
constexpr int solverTimeoutMs = 120000;
constexpr int adaptiveTotalMs = 180000;
constexpr double adaptiveMarkingFraction = 0.5;

QString digestOpenedFile(QFile &file)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.seek(0) || !hash.addData(&file) || !file.seek(0))
        throw std::runtime_error("Could not verify the study artifact.");
    return QString::fromLatin1(hash.result().toHex());
}

QString fileDigest(const QString &path)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not open the study artifact.");
    return digestOpenedFile(file);
}

RModel readDirect(const QString &path, const QString &expectedHash = {})
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not open the expected native result.");
    const QString before = digestOpenedFile(file);
    if (!expectedHash.isEmpty() && before != expectedHash)
        throw std::runtime_error("The native model artifact changed.");
    RModel model;
    model.readFromVerifiedFile(path, file);
    if (digestOpenedFile(file) != before)
        throw std::runtime_error("The native model artifact changed during read.");
    return model;
}

QString analysisIdentity(RModel model)
{
    model.removeAllVariables();
    model.clearEntityVariableData();
    model.getMonitoringPointManager().clear();
    return RModelCommandService::stateFingerprint(model);
}

QString timestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

double component(const QJsonObject &value, const char *key)
{
    return value.value(QLatin1String(key)).toDouble();
}

double delta(const QJsonObject &a, const QJsonObject &b)
{
    return std::max(std::abs(component(a, "uxMm") - component(b, "uxMm")),
                    std::abs(component(a, "uyMm") - component(b, "uyMm")));
}

QString errorMessage(const std::exception &error)
{
    return QString::fromLocal8Bit(error.what());
}

// The stress solver nondimensionalizes and restores coordinates, dimensions,
// and loads; that round-trip can move finite doubles by a few ULPs.
bool sameNumber(double a, double b)
{
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a-b) <= 128 * std::numeric_limits<double>::epsilon() * std::max(std::abs(a), std::abs(b));
}

bool sameMesh(const RModel &input, const RModel &result)
{
    if (input.getNNodes() != result.getNNodes() || input.getNElements() != result.getNElements())
        return false;
    for (uint i = 0; i < input.getNNodes(); ++i)
    {
        const auto &a = input.getNode(i);
        const auto &b = result.getNode(i);
        if (!sameNumber(a.getX(), b.getX()) || !sameNumber(a.getY(), b.getY()) || !sameNumber(a.getZ(), b.getZ()))
            return false;
    }
    for (uint i = 0; i < input.getNElements(); ++i)
    {
        const auto &a = input.getElement(i);
        const auto &b = result.getElement(i);
        if (a.getType() != b.getType() || a.size() != b.size())
            return false;
        for (uint j = 0; j < a.size(); ++j)
            if (a.getNodeId(j) != b.getNodeId(j))
                return false;
    }
    return true;
}

bool sameTable(const RValueTable &a, const RValueTable &b)
{
    if (a.size() != b.size()) return false;
    for (uint i = 0; i < a.size(); ++i)
        if (!sameNumber(a.getKey(i), b.getKey(i)) || !sameNumber(a.getValue(i), b.getValue(i)))
            return false;
    return true;
}

bool sameComponents(const RCondition &a, const RCondition &b)
{
    if (a.size() != b.size()) return false;
    for (uint i = 0; i < a.size(); ++i)
    {
        const auto &x = a.getComponent(i);
        const auto &y = b.getComponent(i);
        if (x.getType() != y.getType() || x.getKeyType() != y.getKeyType() ||
            x.getEnabled() != y.getEnabled() || !sameTable(x, y)) return false;
    }
    return true;
}

bool sameStaticPhysics(const RModel &input, const RModel &result)
{
    if (!sameMesh(input, result) || !RMeshSensitivity::eligibility(result).isEmpty() ||
        input.getNElementGroups() != result.getNElementGroups() ||
        input.getNSurfaces() != result.getNSurfaces()) return false;
    for (uint i = 0; i < input.getNElementGroups(); ++i)
    {
        if (input.getEntityGroupType(i) != result.getEntityGroupType(i)) return false;
        const auto &a = *input.getElementGroupPtr(i);
        const auto &b = *result.getElementGroupPtr(i);
        if (a.size() != b.size() || a.getNBoundaryConditions() != b.getNBoundaryConditions() ||
            a.getNEnvironmentConditions() != b.getNEnvironmentConditions()) return false;
        for (uint j = 0; j < a.size(); ++j)
            if (a.get(j) != b.get(j)) return false;
        for (uint j = 0; j < a.getNBoundaryConditions(); ++j)
        {
            const auto &x = a.getBoundaryCondition(j);
            const auto &y = b.getBoundaryCondition(j);
            if (x.getType() != y.getType() || x.getExplicit() != y.getExplicit() || !sameComponents(x, y))
                return false;
        }
        for (uint j = 0; j < a.getNEnvironmentConditions(); ++j)
        {
            const auto &x = a.getEnvironmentCondition(j);
            const auto &y = b.getEnvironmentCondition(j);
            if (x.getType() != y.getType() || !sameComponents(x, y)) return false;
        }
        const auto &x = a.getMaterial();
        const auto &y = b.getMaterial();
        if (x.getState() != y.getState() || x.size() != y.size()) return false;
        for (uint j = 0; j < x.size(); ++j)
            if (x.get(j).getType() != y.get(j).getType() || !sameTable(x.get(j), y.get(j)))
                return false;
    }
    for (uint i = 0; i < input.getNSurfaces(); ++i)
    {
        const auto &a = input.getSurface(i);
        const auto &b = result.getSurface(i);
        if (a.getStructuralFormulation() != b.getStructuralFormulation() || !sameNumber(a.getThickness(), b.getThickness()))
            return false;
    }
    return true;
}
}

MeshSensitivityDialog::MeshSensitivityDialog(const RModel &source, QString solver, QString artifactRoot,
                                             QString documentId, quint64 revision,
                                             std::function<bool()> sourceCurrent, bool agentAvailable,
                                             QWidget *parent, bool baselineVerified)
    : QDialog(parent)
    , current(source)
    , solverPath(std::move(solver))
    , rootPath(std::move(artifactRoot))
    , documentId(std::move(documentId))
    , revision(revision)
    , sourceCurrent(std::move(sourceCurrent))
    , agentAvailable(agentAvailable)
    , baselineVerified(baselineVerified)
    , baselineProof(baselineVerified ? QStringLiteral("native_snapshot") : QStringLiteral("unverified"))
{
    setObjectName(QStringLiteral("meshSensitivityDialog"));
    setWindowTitle(tr("Mesh sensitivity · %1").arg(source.getName()));
    resize(620, 330);
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(tr("Compare displacement at fixed points. Opened results are reused; refinement runs only when requested."), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    table = new QTableWidget(0, 4, this);
    table->setObjectName(QStringLiteral("meshSensitivityPoints"));
    table->setHorizontalHeaderLabels({tr("Point (m)"), tr("Previous Ux / Uy (mm)"),
                                      tr("Current Ux / Uy (mm)"), tr("Max change (mm)")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table, 1);

    manualControls = new QWidget(this);
    auto *manualLayout = new QVBoxLayout(manualControls);
    manualLayout->setContentsMargins(0,0,0,0);
    layout->addWidget(manualControls);
    auto *toleranceRow = new QHBoxLayout;
    auto *toleranceLabel = new QLabel(tr("Displacement tolerance"), this);
    tolerance = new QDoubleSpinBox(this);
    tolerance->setObjectName(QStringLiteral("meshSensitivityTolerance"));
    tolerance->setDecimals(6);
    tolerance->setRange(0.000001, 1000.0);
    tolerance->setValue(0.01);
    tolerance->setSingleStep(0.001);
    tolerance->setSuffix(tr(" mm"));
    toleranceLabel->setBuddy(tolerance);
    toleranceRow->addWidget(toleranceLabel);
    toleranceRow->addWidget(tolerance);
    toleranceRow->addStretch();
    manualLayout->addLayout(toleranceRow);

    auto *optionsButton = new QToolButton(this);
    optionsButton->setText(tr("Refinement options"));
    optionsButton->setCheckable(true);
    optionsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    optionsButton->setArrowType(Qt::RightArrow);
    toleranceRow->addWidget(optionsButton);
    auto *options = new QWidget(manualControls);
    auto *optionsLayout = new QHBoxLayout(options);
    optionsLayout->setContentsMargins(0,0,0,0);
    auto *scopeLabel = new QLabel(tr("Refinement area"), options);
    refinementScope = new QComboBox(options);
    refinementScope->setObjectName(QStringLiteral("meshSensitivityScope"));
    if (RMeshSensitivity::supportsLocal(source))
        refinementScope->addItem(tr("Around selected point"), QStringLiteral("local"));
    refinementScope->addItem(tr("Whole model"), QStringLiteral("global"));
    optionsButton->setVisible(refinementScope->count() > 1);
    scopeLabel->setBuddy(refinementScope);
    optionsLayout->addWidget(scopeLabel);
    optionsLayout->addWidget(refinementScope);
    radiusLabel = new QLabel(tr("Radius"), options);
    radius = new QDoubleSpinBox(options);
    radius->setObjectName(QStringLiteral("meshSensitivityRadius"));
    radius->setDecimals(8);
    radius->setRange(0.00000001, 1000000000.0);
    radius->setSuffix(tr(" m"));
    double span = 1.0;
    if (source.getNNodes())
    {
        const auto &a = source.getNode(0);
        span = 0.0;
        for (const auto &node : source.getNodes())
            span = std::max(span, std::hypot(node.getX()-a.getX(), node.getY()-a.getY()));
    }
    radius->setValue(std::isfinite(span) ? std::max(span*0.05, 0.00000001) : 1.0);
    radiusLabel->setBuddy(radius);
    optionsLayout->addWidget(radiusLabel);
    optionsLayout->addWidget(radius);
    optionsLayout->addStretch();
    manualLayout->addWidget(options);
    options->hide();
    connect(optionsButton, &QToolButton::toggled, options, &QWidget::setVisible);
    connect(optionsButton, &QToolButton::toggled, this, [optionsButton](bool shown) {
        optionsButton->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(refinementScope, &QComboBox::currentIndexChanged, this, &MeshSensitivityDialog::refresh);

    status = new QLabel(this);
    status->setObjectName(QStringLiteral("meshSensitivityStatus"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    adaptiveSummary = new QLabel(this);
    adaptiveSummary->setObjectName(QStringLiteral("meshSensitivityAdaptiveSummary"));
    adaptiveSummary->setWordWrap(true);
    adaptiveSummary->hide();
    layout->addWidget(adaptiveSummary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    refineButton = buttons->addButton(tr("Refine once"), QDialogButtonBox::ActionRole);
    refineButton->setObjectName(QStringLiteral("meshSensitivityRefine"));
    refineButton->setToolTip(tr("Halve the element size across the mesh and solve one detached candidate."));
    reviewButton = buttons->addButton(tr("Ask Assistant"), QDialogButtonBox::ActionRole);
    reviewButton->setObjectName(QStringLiteral("meshSensitivityReview"));
    acceptButton = buttons->addButton(tr("Accept current"), QDialogButtonBox::ActionRole);
    acceptButton->setObjectName(QStringLiteral("meshSensitivityAccept"));
    openButton = buttons->addButton(tr("Open accepted model"), QDialogButtonBox::ActionRole);
    openButton->setObjectName(QStringLiteral("meshSensitivityOpen"));
    cancelButton = buttons->addButton(tr("Cancel solve"), QDialogButtonBox::ActionRole);
    cancelButton->setObjectName(QStringLiteral("meshSensitivityCancel"));
    layout->addWidget(buttons);
    for (auto *button : buttons->buttons())
        if (auto *push = qobject_cast<QPushButton *>(button))
            push->setAutoDefault(false);

    connect(refineButton, &QPushButton::clicked, this, &MeshSensitivityDialog::refineOnce);
    connect(acceptButton, &QPushButton::clicked, this, &MeshSensitivityDialog::recordAcceptance);
    connect(reviewButton, &QPushButton::clicked, this, &MeshSensitivityDialog::requestReview);
    connect(cancelButton, &QPushButton::clicked, this, [this] { stopRefinement(QStringLiteral("cancelled")); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(openButton, &QPushButton::clicked, this, [this] {
        const auto [relative, hash] = openableArtifact();
        if (relative.isEmpty() || hash.isEmpty()) return;
        const QString path = QDir(studyDirectory).filePath(relative);
        try
        {
            if (fileDigest(path) != hash)
                throw std::runtime_error("The recorded result artifact changed.");
            emit openResultRequested(path);
        }
        catch (const std::exception &error) { lastMessage = errorMessage(error); refresh(); }
    });
    connect(tolerance, &QDoubleSpinBox::valueChanged, this, &MeshSensitivityDialog::refresh);
    connect(&process, &QProcess::finished, this, &MeshSensitivityDialog::finishRefinement);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finishRefinement(-1, QProcess::CrashExit);
    });
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] { stopRefinement(QStringLiteral("time_limit")); });

    try
    {
        invalidReason = RMeshSensitivity::eligibility(source);
        if (invalidReason.isEmpty() && source.findVariable(R_VARIABLE_DISPLACEMENT) != RConstants::eod)
        {
            positions = RMeshSensitivity::probePositions(source);
            appendLevel(RMeshSensitivity::measurements(source, positions), {});
        }
        else if (invalidReason.isEmpty())
        {
            this->baselineVerified = false;
            baselineProof = QStringLiteral("unverified");
        }
    }
    catch (const RError &error) { invalidReason = error.getMessage(); }
    catch (const std::exception &error) { invalidReason = errorMessage(error); }
    refresh();
}

MeshSensitivityDialog::~MeshSensitivityDialog()
{
    if (!pending) return;
    if (adaptiveActive) adaptiveStatus = QStringLiteral("interrupted");
    disconnect(&process, nullptr, this, nullptr);
    process.kill();
    process.waitForFinished(1000);
    auto attempt = attempts.last().toObject();
    attempt.insert(QStringLiteral("status"), QStringLiteral("interrupted"));
    attempt.insert(QStringLiteral("finishedUtc"), timestamp());
    attempts.replace(attempts.size()-1, attempt);
    try { saveStudy(QStringLiteral("interrupted")); }
    catch (...) {} // A prior atomic receipt remains available.
}

bool MeshSensitivityDialog::hasNativeCompletion(const QString &path, const QString &hash)
{
    if (path.isEmpty() || hash.size() != 64)
        return false;
    const QFileInfo result(path);
    const QString base = QFileInfo(RFileUtils::getFileNameWithOutTimeStep(path)).fileName();
    const auto files = result.dir().entryInfoList({base + QStringLiteral(".*.events.jsonl")},
                                                  QDir::Files | QDir::NoSymLinks, QDir::Time);
    for (qsizetype i = 0; i < std::min<qsizetype>(files.size(), 32); ++i)
    {
        const auto &info = files.at(i);
        if (info.size() > 1024 * 1024) continue;
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        QByteArray last;
        while (!file.atEnd())
        {
            const auto line = file.readLine().trimmed();
            if (!line.isEmpty()) last = line;
        }
        const auto event = QJsonDocument::fromJson(last).object();
        const auto payload = event.value(QStringLiteral("payload")).toObject();
        const QFileInfo target(payload.value(QStringLiteral("resultModelTargetPath")).toString());
        if (event.value(QStringLiteral("type")) == QStringLiteral("RunFinished") &&
            event.value(QStringLiteral("status")) == QStringLiteral("finished") &&
            payload.value(QStringLiteral("resultModelTargetSha256")) == hash &&
            target.absoluteFilePath() == result.absoluteFilePath())
            return true;
    }
    return false;
}

bool MeshSensitivityDialog::matchesSolvedSnapshot(const RModel &source, const QString &path,
                                                   const QString &hash)
{
    if (path.isEmpty() || hash.isEmpty() || source.findVariable(R_VARIABLE_DISPLACEMENT) == RConstants::eod)
        return false;
    try
    {
        const RModel solved = readDirect(path, hash);
        const uint a = source.findVariable(R_VARIABLE_DISPLACEMENT);
        const uint b = solved.findVariable(R_VARIABLE_DISPLACEMENT);
        if (b == RConstants::eod || !sameMesh(source, solved))
            return false;
        const auto &u = source.getVariable(a);
        const auto &v = solved.getVariable(b);
        if (u.getApplyType() != v.getApplyType() || u.getNVectors() != v.getNVectors() ||
            u.getNValues() != v.getNValues())
            return false;
        for (uint i = 0; i < u.getNVectors(); ++i)
        {
            if (u[i].size() != v[i].size()) return false;
            for (uint j = 0; j < u[i].size(); ++j)
                if (u.getValue(i, j) != v.getValue(i, j)) return false;
        }
        return analysisIdentity(source) == analysisIdentity(solved);
    }
    catch (...) { return false; }
}

bool MeshSensitivityDialog::belongsTo(const QPair<QString,quint64> &stamp) const
{
    return stamp.first == documentId && stamp.second == revision;
}

double MeshSensitivityDialog::maximumChange() const
{
    double change = 0.0;
    if (levels.size() < 2)
        return change;
    const auto currentValues = levels.last().toObject().value(QStringLiteral("points")).toArray();
    const auto previous = levels.at(levels.size()-2).toObject().value(QStringLiteral("points")).toArray();
    for (qsizetype i = 0; i < currentValues.size(); ++i)
        change = std::max(change, delta(currentValues.at(i).toObject(), previous.at(i).toObject()));
    return change;
}

QPair<QString,QString> MeshSensitivityDialog::openableArtifact() const
{
    if (!decision.isEmpty())
        return {decision.value(QStringLiteral("result")).toString(), decision.value(QStringLiteral("resultSha256")).toString()};
    if (!adaptiveActive && !adaptiveStatus.isEmpty() && baselineVerified && !levels.isEmpty())
    {
        const auto latest = levels.last().toObject();
        return {latest.value(QStringLiteral("result")).toString(), latest.value(QStringLiteral("sha256")).toString()};
    }
    return {};
}

QString MeshSensitivityDialog::statusCode() const
{
    if (!invalidReason.isEmpty()) return QStringLiteral("unavailable");
    if (!sourceCurrent()) return QStringLiteral("stale_source");
    if (pending) return QStringLiteral("running");
    if (!decision.isEmpty()) return decision.value(QStringLiteral("status")).toString();
    if (!adaptiveStatus.isEmpty()) return QStringLiteral("adaptive_") + adaptiveStatus;
    if (!baselineVerified) return QStringLiteral("unverified_baseline");
    if (levels.size() < 2) return QStringLiteral("no_comparison");
    return maximumChange() <= tolerance->value() ? QStringLiteral("within_tolerance")
                                                  : QStringLiteral("above_tolerance");
}

void MeshSensitivityDialog::refresh()
{
    const QString state = statusCode();
    QString text;
    if (state == QStringLiteral("unavailable")) text = invalidReason;
    else if (state == QStringLiteral("stale_source")) text = tr("The source model is busy or has changed. Reopen the study for the current result.");
    else if (state == QStringLiteral("running")) text = baselineVerified ? tr("Solving one refined mesh…") : tr("Solving the baseline mesh…");
    else if (state == QStringLiteral("unverified_baseline"))
        text = tr("The current settings could not be matched to a solved snapshot. Solve the baseline to continue.");
    else if (state == QStringLiteral("accepted_by_review")) text = tr("Accepted by review without a mesh comparison.");
    else if (state == QStringLiteral("accepted_with_adaptive_estimate")) text = tr("Accepted. The adaptive energy error estimate met the target.");
    else if (state == QStringLiteral("accepted_with_comparison")) text = tr("Accepted. The last adjacent mesh comparison met the displacement tolerance.");
    else if (state == QStringLiteral("within_tolerance")) text = tr("The last adjacent mesh comparison is within tolerance.");
    else if (state == QStringLiteral("above_tolerance")) text = tr("The last adjacent mesh comparison exceeds tolerance.");
    else if (state.startsWith(QStringLiteral("adaptive_"))) text = tr("The table compares displacement at fixed points.");
    else text = tr("No mesh comparison yet. You can accept by review or refine once.");

    const bool local = refinementScope->currentData().toString() == QStringLiteral("local");
    const bool budget = attempts.size() < maximumAttempts &&
        (!baselineVerified ? current.getNElements() <= maximumElements :
         local ? current.getNElements() < maximumElements : current.getNElements() <= maximumElements / 4);
    if (!budget && !pending && decision.isEmpty() && invalidReason.isEmpty())
        text += tr(" The refinement budget has been reached.");
    if (!lastMessage.isEmpty())
        text += QStringLiteral("\n") + lastMessage;
    status->setText(text);
    const bool editable = !pending && !adaptiveActive && invalidReason.isEmpty() && sourceCurrent() && decision.isEmpty();
    const bool automaticStudy = !adaptiveStatus.isEmpty();
    if (automaticStudy)
    {
        const auto latest = adaptiveEstimates.isEmpty() ? QJsonObject() : adaptiveEstimates.last().toObject();
        QString summary;
        if (adaptiveActive) summary = tr("Adaptive review in progress.");
        else if (adaptiveStatus == QStringLiteral("converged")) summary = tr("Adaptive error estimate met the target.");
        else if (adaptiveStatus == QStringLiteral("budget")) summary = tr("Adaptive review stopped at a resource limit.");
        else if (adaptiveStatus == QStringLiteral("stagnated")) summary = tr("Adaptive review stopped because two passes gave too little improvement.");
        else if (adaptiveStatus == QStringLiteral("cancelled")) summary = tr("Adaptive review cancelled.");
        else summary = tr("Adaptive review stopped before reaching the target.");
        if (!latest.isEmpty())
        {
            summary += tr(" Estimated energy error: %1% · Target: %2%")
                .arg(100.0 * latest.value(QStringLiteral("relativeError")).toDouble(), 0, 'g', 4)
                .arg(adaptiveTolerancePercent, 0, 'g', 4);
            if (adaptiveActive && latest.value(QStringLiteral("relativeError")).toDouble() > adaptiveTolerancePercent / 100.0)
                summary += tr(" · Marked triangles: %1").arg(latest.value(QStringLiteral("markedCount")).toInt());
            if (pending && !attempts.isEmpty())
                summary += tr(" · Candidate elements: %1").arg(
                    attempts.last().toObject().value(QStringLiteral("candidateElements")).toInt());
            const auto leading = latest.value(QStringLiteral("leadingCells")).toArray();
            QStringList locations;
            for (const auto &cell : leading)
            {
                const auto xy = cell.toObject().value(QStringLiteral("centerM")).toArray();
                locations.append(tr("(%1, %2)").arg(xy.at(0).toDouble(), 0, 'g', 4).arg(xy.at(1).toDouble(), 0, 'g', 4));
            }
            adaptiveSummary->setToolTip(locations.isEmpty() ? QString() :
                tr("Leading marked triangle centers (m): %1").arg(locations.join(QStringLiteral(", "))));
        }
        adaptiveSummary->setText(summary);
    }
    adaptiveSummary->setVisible(automaticStudy);
    manualControls->setVisible(!automaticStudy);
    refineButton->setVisible(!automaticStudy);
    refineButton->setEnabled(editable && !automaticStudy && budget && QFileInfo(solverPath).isExecutable());
    refineButton->setToolTip(!baselineVerified ? tr("Solve the current mesh once to verify its baseline.") :
                             local ? tr("Refine around the selected point and solve one detached candidate.")
                                   : tr("Refine the whole mesh and solve one detached candidate."));
    refineButton->setText(!baselineVerified ? tr("Solve baseline") :
                          local ? tr("Refine locally") : tr("Refine whole mesh"));
    refinementScope->setEnabled(editable && baselineVerified && !automaticStudy);
    radius->setEnabled(editable && baselineVerified && !automaticStudy);
    radiusLabel->setVisible(local);
    radius->setVisible(local);
    reviewButton->setVisible(agentAvailable);
    reviewButton->setEnabled(agentAvailable && !pending && !adaptiveActive && invalidReason.isEmpty());
    acceptButton->setEnabled(editable && baselineVerified && !levels.isEmpty() &&
                             (automaticStudy ? adaptiveStatus == QStringLiteral("converged") :
                              (levels.size() < 2 || maximumChange() <= tolerance->value())));
    tolerance->setEnabled(editable && !automaticStudy);
    openButton->setText(decision.isEmpty() ? tr("Open latest result") : tr("Open accepted model"));
    openButton->setVisible(!openableArtifact().first.isEmpty());
    cancelButton->setVisible(pending || adaptiveActive);

    if (levels.isEmpty()) return;
    const auto values = levels.last().toObject().value(QStringLiteral("points")).toArray();
    const auto previous = levels.size() > 1
        ? levels.at(levels.size()-2).toObject().value(QStringLiteral("points")).toArray() : QJsonArray();
    table->setRowCount(int(values.size()));
    if (table->currentRow() < 0 && !values.isEmpty()) table->selectRow(0);
    const auto pairText = [this](const QJsonObject &v) {
        return tr("%1 / %2").arg(component(v, "uxMm"), 0, 'g', 7).arg(component(v, "uyMm"), 0, 'g', 7);
    };
    for (qsizetype i = 0; i < values.size(); ++i)
    {
        const auto v = values.at(i).toObject();
        const auto p = previous.isEmpty() ? QJsonObject() : previous.at(i).toObject();
        const double change = p.isEmpty() ? 0.0 : delta(v, p);
        const QString changeText = (change > 0.0 && change < 1e-6)
            ? tr("< 0.000001") : QString::number(change, 'g', 6);
        const QStringList cells{
            tr("%1, %2").arg(component(v, "xM"), 0, 'g', 7).arg(component(v, "yM"), 0, 'g', 7),
            p.isEmpty() ? QStringLiteral("—") : pairText(p), pairText(v),
            p.isEmpty() ? QStringLiteral("—") : changeText};
        for (int j = 0; j < cells.size(); ++j)
            table->setItem(int(i), j, new QTableWidgetItem(cells.at(j)));
    }
}

QJsonObject MeshSensitivityDialog::evidence(const QString &state) const
{
    QJsonObject data{{QStringLiteral("schema"), QStringLiteral("range.mesh-sensitivity/v1")},
                     {QStringLiteral("documentId"), documentId},
                     {QStringLiteral("sourceRevision"), QString::number(revision)},
                     {QStringLiteral("status"), state.isEmpty() ? statusCode() : state},
                     {QStringLiteral("toleranceMm"), tolerance->value()},
                     {QStringLiteral("method"), QStringLiteral("nested h-refinement, fixed XY displacement probes")},
                     {QStringLiteral("levels"), levels},
                     {QStringLiteral("baselineProof"), baselineProof},
                     {QStringLiteral("attempts"), attempts},
                     {QStringLiteral("maximumAttempts"), maximumAttempts},
                     {QStringLiteral("maximumElements"), int(maximumElements)},
                     {QStringLiteral("maximumSecondsPerSolve"), solverTimeoutMs / 1000},
                     {QStringLiteral("decision"), decision}};
    if (levels.size() > 1)
        data.insert(QStringLiteral("maximumAdjacentChangeMm"), maximumChange());
    QJsonObject next{{QStringLiteral("scope"), baselineVerified ? refinementScope->currentData().toString()
                                                                 : QStringLiteral("baseline")}};
    if (next.value(QStringLiteral("scope")) == QStringLiteral("local") && !positions.empty())
    {
        const auto &point = positions.at(size_t(std::max(0, table->currentRow())));
        next.insert(QStringLiteral("pointM"), QJsonArray{point[0], point[1], point[2]});
        next.insert(QStringLiteral("radiusM"), radius->value());
    }
    if (adaptiveStatus.isEmpty())
        data.insert(QStringLiteral("proposedRefinement"), next);
    else
        data.insert(QStringLiteral("adaptive"), QJsonObject{
            {QStringLiteral("status"), adaptiveStatus},
            {QStringLiteral("tolerancePercent"), adaptiveTolerancePercent},
            {QStringLiteral("markingFraction"), adaptiveMarkingFraction},
            {QStringLiteral("minimumEdgeM"), adaptiveMinimumEdgeM},
            {QStringLiteral("maximumSeconds"), adaptiveTotalMs / 1000},
            {QStringLiteral("estimates"), adaptiveEstimates},
            {QStringLiteral("previousReviews"), adaptiveHistory}});
    return data;
}

void MeshSensitivityDialog::appendLevel(const QJsonArray &values, const QString &fileName)
{
    if (values.isEmpty() || (!levels.isEmpty() &&
        levels.first().toObject().value(QStringLiteral("points")).toArray().size() != values.size()))
        throw std::runtime_error("The fixed displacement probes could not all be read.");
    QJsonObject level{{QStringLiteral("nodes"), int(current.getNNodes())},
                      {QStringLiteral("elements"), int(current.getNElements())},
                      {QStringLiteral("points"), values}};
    if (!fileName.isEmpty())
    {
        const QFileInfo file(fileName);
        level.insert(QStringLiteral("result"), QDir(studyDirectory).relativeFilePath(fileName));
        level.insert(QStringLiteral("sha256"), fileDigest(fileName));
    }
    levels.append(level);
}

void MeshSensitivityDialog::ensureStudyDirectory()
{
    if (!studyDirectory.isEmpty()) return;
    if (rootPath.isEmpty() || !QDir().mkpath(rootPath))
        throw std::runtime_error("Could not create the mesh study artifact directory.");
    QTemporaryDir directory(QDir(rootPath).filePath(QStringLiteral("mesh-study-XXXXXX")));
    if (!directory.isValid())
        throw std::runtime_error("Could not allocate a mesh study directory.");
    const QString baseline = QDir(directory.path()).filePath(QStringLiteral("baseline.rbm"));
    current.write(baseline, false);
    if (!levels.isEmpty())
    {
        auto level = levels.first().toObject();
        level.insert(QStringLiteral("sha256"), fileDigest(baseline));
        level.insert(QStringLiteral("result"), QStringLiteral("baseline.rbm"));
        levels.replace(0, level);
    }
    studyDirectory = directory.path();
    directory.setAutoRemove(false);
}

void MeshSensitivityDialog::saveStudy(const QString &state)
{
    QSaveFile file(QDir(studyDirectory).filePath(QStringLiteral("study.json")));
    const QByteArray json = QJsonDocument(evidence(state)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        throw std::runtime_error("Could not save the study evidence.");
}

bool MeshSensitivityDialog::startAdaptive(double tolerancePercent)
{
    if (isRunning() || !decision.isEmpty() ||
        !sourceCurrent() || !std::isfinite(tolerancePercent) || tolerancePercent <= 0 || tolerancePercent > 100)
        return false;
    const QString problem = invalidReason.isEmpty() ? RMeshAdaptivity::eligibility(current) : invalidReason;
    if (!problem.isEmpty())
    {
        lastMessage = problem;
        refresh();
        return false;
    }
    if (!adaptiveStatus.isEmpty())
    {
        auto previous = evidence().value(QStringLiteral("adaptive")).toObject();
        previous.remove(QStringLiteral("previousReviews"));
        if (adaptiveHistory.size() >= 3) adaptiveHistory.removeFirst();
        adaptiveHistory.append(previous);
        adaptiveEstimates = {};
        adaptiveMarks.clear();
    }
    lastMessage.clear();
    adaptiveTolerancePercent = tolerancePercent;
    // An automatic pass never deliberately inserts edges below one millionth
    // of the model span. The current minimum-size limit is recorded per run.
    const auto &origin = current.getNode(0);
    double span = 0.0;
    for (const auto &node : current.getNodes())
        span = std::max(span, std::hypot(node.getX()-origin.getX(), node.getY()-origin.getY()));
    adaptiveMinimumEdgeM = span * 1e-6;
    adaptiveActive = true;
    adaptiveStatus = QStringLiteral("running");
    adaptiveClock.start();
    refresh();
    QTimer::singleShot(0, this, &MeshSensitivityDialog::advanceAdaptive);
    return true;
}

void MeshSensitivityDialog::advanceAdaptive()
{
    if (!adaptiveActive || pending) return;
    if (!sourceCurrent())
    {
        finishAdaptive(QStringLiteral("stale_source"));
        return;
    }
    if (adaptiveClock.elapsed() >= adaptiveTotalMs)
    {
        finishAdaptive(QStringLiteral("budget"), tr("The adaptive time budget has been reached."));
        return;
    }
    try
    {
        if (baselineVerified)
        {
            const auto estimate = RMeshAdaptivity::estimate(current, adaptiveMarkingFraction);
            QJsonArray leadingCells;
            for (size_t i = 0; i < std::min<size_t>(estimate.markedElements.size(), 6); ++i)
            {
                const uint id = estimate.markedElements[i];
                const auto &cell = current.getElement(id);
                double x = 0.0, y = 0.0;
                for (uint j = 0; j < 3; ++j)
                {
                    const auto &node = current.getNode(cell.getNodeId(j));
                    x += node.getX() / 3.0;
                    y += node.getY() / 3.0;
                }
                leadingCells.append(QJsonObject{{QStringLiteral("elementPhysicalId"), double(current.getElementPhysicalId(id))},
                                               {QStringLiteral("centerM"), QJsonArray{x,y}}});
            }
            adaptiveEstimates.append(QJsonObject{
                {QStringLiteral("level"), int(levels.size()-1)},
                {QStringLiteral("leadingCells"), leadingCells},
                {QStringLiteral("relativeError"), estimate.relativeError},
                {QStringLiteral("indicatorEnergy"), estimate.indicatorEnergy},
                {QStringLiteral("strainEnergy"), estimate.strainEnergy},
                {QStringLiteral("markedCount"), int(estimate.markedElements.size())},
                {QStringLiteral("elements"), int(current.getNElements())}});
            if (estimate.relativeError <= adaptiveTolerancePercent / 100.0)
            {
                finishAdaptive(QStringLiteral("converged"));
                return;
            }
            if (adaptiveEstimates.size() >= 3)
            {
                const qsizetype n = adaptiveEstimates.size();
                const auto relativeAt = [this](qsizetype i) {
                    return adaptiveEstimates.at(i).toObject().value(QStringLiteral("relativeError")).toDouble();
                };
                if (relativeAt(n-1) >= 0.98 * relativeAt(n-2) && relativeAt(n-2) >= 0.98 * relativeAt(n-3))
                {
                    finishAdaptive(QStringLiteral("stagnated"));
                    return;
                }
            }
            adaptiveMarks = estimate.markedElements;
        }
        if (attempts.size() >= maximumAttempts || current.getNElements() > maximumElements)
        {
            finishAdaptive(QStringLiteral("budget"));
            return;
        }
        if (!QFileInfo(solverPath).isExecutable())
        {
            finishAdaptive(QStringLiteral("failed"), tr("The solver executable is unavailable."));
            return;
        }
        refineOnce();
    }
    catch (const RError &error) { finishAdaptive(QStringLiteral("failed"), error.getMessage()); }
    catch (const std::exception &error) { finishAdaptive(QStringLiteral("failed"), errorMessage(error)); }
}

void MeshSensitivityDialog::finishAdaptive(const QString &outcome, const QString &message)
{
    adaptiveActive = false;
    adaptiveStatus = outcome;
    if (!message.isEmpty()) lastMessage = message;
    try
    {
        ensureStudyDirectory();
        saveStudy();
    }
    catch (const RError &error) { lastMessage = error.getMessage(); }
    catch (const std::exception &error) { lastMessage = errorMessage(error); }
    refresh();
}

void MeshSensitivityDialog::refineOnce()
{
    if ((!adaptiveActive && !refineButton->isEnabled()) || pending) return;
    if (!sourceCurrent())
    {
        if (adaptiveActive) finishAdaptive(QStringLiteral("stale_source"));
        return;
    }
    lastMessage.clear();
    try
    {
        const QString scope = !baselineVerified ? QStringLiteral("baseline") :
            adaptiveActive ? QStringLiteral("adaptive") : refinementScope->currentData().toString();
        const RR3Vector point = scope == QStringLiteral("local")
            ? positions.at(size_t(std::max(0, table->currentRow()))) : RR3Vector();
        RModel candidate;
        if (scope == QStringLiteral("baseline")) candidate = current;
        else if (scope == QStringLiteral("adaptive"))
            candidate = RMeshSensitivity::refineMarked(current, adaptiveMarks, maximumElements, adaptiveMinimumEdgeM);
        else if (scope == QStringLiteral("local"))
            candidate = RMeshSensitivity::refineLocal(current, point, radius->value(), maximumElements);
        else candidate = RMeshSensitivity::refine(current, maximumElements);
        candidate.removeAllVariables();
        const QString problem = RMeshSensitivity::eligibility(candidate);
        if (!problem.isEmpty()) throw std::runtime_error(problem.toStdString());
        ensureStudyDirectory();
        QTemporaryDir attemptDirectory(QDir(studyDirectory).filePath(QStringLiteral("attempt-XXXXXX")));
        if (!attemptDirectory.isValid())
            throw std::runtime_error("Could not create the refinement directory.");
        levelDirectory = attemptDirectory.path();
        const QString levelName = QFileInfo(levelDirectory).fileName();
        const QDir dir(levelDirectory);
        const QString input = dir.filePath(QStringLiteral("candidate.rbm"));
        const QString run = dir.filePath(QStringLiteral("run.rbm"));
        candidate.write(input, false);
        if (!QFile::copy(input, run))
            throw std::runtime_error("Could not prepare the isolated solver input.");
        QJsonObject attempt{{QStringLiteral("directory"), levelName},
                            {QStringLiteral("inputSha256"), fileDigest(input)},
                            {QStringLiteral("scope"), scope},
                            {QStringLiteral("candidateElements"), int(candidate.getNElements())},
                            {QStringLiteral("candidateNodes"), int(candidate.getNNodes())},
                            {QStringLiteral("startedUtc"), timestamp()},
                            {QStringLiteral("status"), QStringLiteral("running")}};
        if (scope == QStringLiteral("local"))
        {
            attempt.insert(QStringLiteral("pointM"), QJsonArray{point[0], point[1], point[2]});
            attempt.insert(QStringLiteral("radiusM"), radius->value());
        }
        if (adaptiveActive && adaptiveClock.elapsed() >= adaptiveTotalMs)
        {
            finishAdaptive(QStringLiteral("budget"), tr("The adaptive time budget has been reached."));
            return;
        }
        attempts.append(attempt);
        pending = true;
        stopReason.clear();
        saveStudy();
        attemptDirectory.setAutoRemove(false);
        process.setWorkingDirectory(levelDirectory);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setStandardOutputFile(QDir(levelDirectory).filePath(QStringLiteral("solver.log")));
        const int remainingMs = adaptiveActive ? int(adaptiveTotalMs - adaptiveClock.elapsed()) : solverTimeoutMs;
        deadline.start(std::clamp(remainingMs, 1, solverTimeoutMs));
        process.start(solverPath, {QStringLiteral("--file=") + run,
                                   QStringLiteral("--monitoring-file=") + dir.filePath(QStringLiteral("monitoring.txt")),
                                   QStringLiteral("--nthreads=2")});
    }
    catch (const RMeshSensitivity::RefinementLimit &error)
    {
        if (adaptiveActive)
        {
            finishAdaptive(QStringLiteral("budget"), errorMessage(error));
            return;
        }
        lastMessage = errorMessage(error);
    }
    catch (const RError &error) { lastMessage = error.getMessage(); }
    catch (const std::exception &error) { lastMessage = errorMessage(error); }
    if (!lastMessage.isEmpty() && pending && process.state() == QProcess::NotRunning)
    {
        stopReason = QStringLiteral("preparation_failed");
        finishRefinement(-1, QProcess::CrashExit);
    }
    else if (adaptiveActive && !pending && !lastMessage.isEmpty())
    {
        finishAdaptive(QStringLiteral("failed"));
        return;
    }
    refresh();
}

void MeshSensitivityDialog::stopRefinement(const QString &reason)
{
    if (!pending)
    {
        if (adaptiveActive) finishAdaptive(reason);
        return;
    }
    stopReason = reason;
    process.kill();
}

void MeshSensitivityDialog::finishRefinement(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!pending) return;
    deadline.stop();
    pending = false;
    auto attempt = attempts.last().toObject();
    QString outcome = stopReason;
    try
    {
        if (outcome.isEmpty())
        {
            if (exitCode != 0 || exitStatus != QProcess::NormalExit)
                throw std::runtime_error("The refinement solver failed. See the solver log in the study directory.");
            const QDir dir(levelDirectory);
            const QString candidatePath = dir.filePath(QStringLiteral("candidate.rbm"));
            const QString resultPath = dir.filePath(QStringLiteral("run-00000.rbm"));
            auto input = readDirect(candidatePath, attempt.value(QStringLiteral("inputSha256")).toString());
            const auto result = readDirect(resultPath);
            if (!sameStaticPhysics(input, result))
                throw std::runtime_error("The solver result does not match the candidate mesh and analysis.");
            // Keep the exact input geometry and conditions for future
            // refinement; only import the solver's nodal/element results.
            static_cast<RResults &>(input) = static_cast<const RResults &>(result);
            const bool rebuiltBaseline = attempt.value(QStringLiteral("scope")).toString() == QStringLiteral("baseline");
            const auto newPositions = rebuiltBaseline ? RMeshSensitivity::probePositions(input) : positions;
            const auto values = RMeshSensitivity::measurements(input, newPositions);
            const RModel previous = current;
            const auto previousLevels = levels;
            if (rebuiltBaseline) levels = {};
            current = std::move(input);
            try { appendLevel(values, resultPath); }
            catch (...) { current = previous; levels = previousLevels; throw; }
            if (rebuiltBaseline)
            {
                baselineVerified = true;
                baselineProof = QStringLiteral("recomputed");
                positions = newPositions;
            }
            outcome = QStringLiteral("complete");
        }
    }
    catch (const RError &error) { lastMessage = error.getMessage(); }
    catch (const std::exception &error) { lastMessage = errorMessage(error); }
    if (outcome.isEmpty()) outcome = QStringLiteral("failed");
    if (outcome == QStringLiteral("time_limit")) lastMessage = tr("Refinement stopped at the run time limit.");
    else if (outcome == QStringLiteral("cancelled")) lastMessage = tr("Refinement cancelled.");
    attempt.insert(QStringLiteral("status"), outcome);
    attempt.insert(QStringLiteral("exitCode"), exitCode);
    attempt.insert(QStringLiteral("finishedUtc"), timestamp());
    if (!lastMessage.isEmpty()) attempt.insert(QStringLiteral("detail"), lastMessage);
    attempts.replace(attempts.size()-1, attempt);
    if (adaptiveActive)
    {
        if (outcome == QStringLiteral("complete"))
        {
            try { saveStudy(); }
            catch (const std::exception &error) { finishAdaptive(QStringLiteral("failed"), errorMessage(error)); return; }
            QTimer::singleShot(0, this, &MeshSensitivityDialog::advanceAdaptive);
            refresh();
        }
        else finishAdaptive(outcome == QStringLiteral("time_limit") ? QStringLiteral("budget") :
                            outcome == QStringLiteral("cancelled") ? outcome : QStringLiteral("failed"));
        return;
    }
    try { saveStudy(); }
    catch (const std::exception &error) { lastMessage = errorMessage(error); }
    refresh();
}

void MeshSensitivityDialog::recordAcceptance()
{
    if (!acceptButton->isEnabled() || pending || !sourceCurrent()) return;
    try
    {
        ensureStudyDirectory();
        const auto currentLevel = levels.last().toObject();
        const QString resultPath = currentLevel.value(QStringLiteral("result")).toString();
        const QString hash = currentLevel.value(QStringLiteral("sha256")).toString();
        if (fileDigest(QDir(studyDirectory).filePath(resultPath)) != hash)
            throw std::runtime_error("The displayed result artifact changed.");
        const QString accepted = adaptiveStatus == QStringLiteral("converged") ? QStringLiteral("accepted_with_adaptive_estimate") :
            levels.size() > 1 ? QStringLiteral("accepted_with_comparison") : QStringLiteral("accepted_by_review");
        decision = {{QStringLiteral("status"), accepted},
                    {QStringLiteral("acceptedUtc"), timestamp()},
                    {QStringLiteral("result"), resultPath},
                    {QStringLiteral("resultSha256"), hash}};
        saveStudy();
        lastMessage = tr("Evidence saved in %1").arg(studyDirectory);
    }
    catch (const RError &error) { decision = {}; lastMessage = error.getMessage(); }
    catch (const std::exception &error) { decision = {}; lastMessage = errorMessage(error); }
    refresh();
}

void MeshSensitivityDialog::requestReview()
{
    if (!reviewButton->isEnabled()) return;
    auto analysis = evidence();
    QJsonArray publicAttempts;
    for (const auto &entry : attempts)
    {
        auto attempt = entry.toObject();
        attempt.remove(QStringLiteral("directory"));
        attempt.remove(QStringLiteral("detail"));
        publicAttempts.append(attempt);
    }
    analysis.insert(QStringLiteral("attempts"), publicAttempts);
    emit reviewRequested(tr("Review this mesh study evidence. Explain whether the displayed result is "
                            "supported for the measured displacements, or whether one more bounded refinement "
                            "or a model correction is warranted. Use the selected scope and remaining budget. "
                            "For an adaptive study, explain the residual indicator, leading marked regions and "
                            "termination reason. The estimated error is an indicator. Review the displacements and "
                            "suggest the next step for the user.\n%1")
                         .arg(QString::fromUtf8(QJsonDocument(analysis).toJson(QJsonDocument::Compact))));
}
