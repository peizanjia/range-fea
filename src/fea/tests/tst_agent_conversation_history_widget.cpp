#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QRunnable>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include <ragent_conversation_store.h>
#include <ragent_evaluation_capture_store.h>
#include <rml_model.h>
#include <rml_time_solver.h>
#include <rbl_job_manager.h>

#include "agent_widget.h"
#include "application.h"
#include "application_settings.h"
#include "model_command_host.h"
#include "model_io.h"
#include "session.h"
#include "solver_manager.h"
#include "workflow_process.h"

namespace
{
class AgentSettingsSnapshot
{
public:
    explicit AgentSettingsSnapshot(QSettings *settings, QStringList additionalKeys = {})
        : settings(settings), additionalKeys(std::move(additionalKeys))
    {
        if (!settings) return;
        for (const QString &key : settings->allKeys())
        {
            if (!key.startsWith(QStringLiteral("aiAgent/")) && !this->additionalKeys.contains(key)) continue;
            present.insert(key, true);
            values.insert(key, settings->value(key));
        }
    }

    ~AgentSettingsSnapshot()
    {
        if (!settings) return;
        for (const QString &key : settings->allKeys())
            if (key.startsWith(QStringLiteral("aiAgent/")) || additionalKeys.contains(key))
                settings->remove(key);
        for (auto it = present.cbegin(); it != present.cend(); ++it)
            if (it.value()) settings->setValue(it.key(), values.value(it.key()));
    }

private:
    QSettings *settings = nullptr;
    QStringList additionalKeys;
    QHash<QString, bool> present;
    QHash<QString, QVariant> values;
};

Model modalConversationFixture()
{
    Model model;
    model.setName(QStringLiteral("History widget modal fixture"));
    model.setNNodes(2);
    model.setNode(0, RNode(0.0, 0.0, 0.0));
    model.setNode(1, RNode(1.0, 0.0, 0.0));
    model.setNElements(1);
    RElement element(R_ELEMENT_TRUSS1);
    element.setNodeId(0, 0);
    element.setNodeId(1, 1);
    model.setElement(0, element);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    auto &modal = model.getProblemSetup().getModalSetup();
    modal.setMethod(R_MODAL_MULTIPLE_MODES);
    modal.setNIterations(12);
    modal.setNModesToExtract(2);
    return model;
}

QJsonObject trustedRodProblemSpec()
{
    const auto quantity = [](double value, const char *unit) {
        return QJsonObject{{QStringLiteral("value"), value},
                           {QStringLiteral("unit"), QString::fromLatin1(unit)}};
    };
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("modelType"), QStringLiteral("axialRod")},
            {QStringLiteral("name"), QStringLiteral("Agent UI review rod")},
            {QStringLiteral("geometry"), QJsonObject{
                 {QStringLiteral("length"), quantity(1.0, "m")},
                 {QStringLiteral("segments"), 2}}},
            {QStringLiteral("material"), QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("Steel")},
                 {QStringLiteral("source"), QStringLiteral("test fixture")},
                 {QStringLiteral("youngModulus"), quantity(210e9, "Pa")},
                 {QStringLiteral("poissonRatio"), 0.3},
                 {QStringLiteral("density"), quantity(7850.0, "kg/m3")}}},
            {QStringLiteral("section"), QJsonObject{
                 {QStringLiteral("area"), quantity(1e-4, "m2")}}},
            {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}}},
            {QStringLiteral("supports"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}},
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}}}},
            {QStringLiteral("loads"), QJsonArray{
                 QJsonObject{{QStringLiteral("kind"), QStringLiteral("pointForce")},
                             {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("components"), QJsonArray{quantity(1000.0, "N"),
                                 quantity(0.0, "N"), quantity(0.0, "N")}}}}},
            {QStringLiteral("initialConditions"), QJsonArray{}},
            {QStringLiteral("observables"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("quantity"), QStringLiteral("displacement")},
                             {QStringLiteral("component"), QStringLiteral("ux")}}}},
            {QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}}}};
}

QJsonObject trustedCantileverBeamProblemSpec()
{
    const auto quantity = [](double value, const char *unit) {
        return QJsonObject{{QStringLiteral("value"), value},
                           {QStringLiteral("unit"), QString::fromLatin1(unit)}};
    };
    QJsonObject spec = trustedRodProblemSpec();
    spec.insert(QStringLiteral("modelType"), QStringLiteral("cantileverBeam"));
    spec.insert(QStringLiteral("name"), QStringLiteral("Agent UI review cantilever"));
    spec.insert(QStringLiteral("geometry"), QJsonObject{
        {QStringLiteral("length"), quantity(1.0, "m")}, {QStringLiteral("segments"), 1}});
    spec.insert(QStringLiteral("section"), QJsonObject{
        {QStringLiteral("area"), quantity(1.0e-4, "m2")},
        {QStringLiteral("iy"), quantity(8.333333333333333e-10, "m4")},
        {QStringLiteral("iz"), quantity(8.333333333333333e-10, "m4")},
        {QStringLiteral("torsionalConstant"), quantity(1.666666666666667e-9, "m4")},
        {QStringLiteral("wy"), quantity(1.666666666666667e-7, "m3")},
        {QStringLiteral("wz"), quantity(1.666666666666667e-7, "m3")},
        {QStringLiteral("wt"), quantity(1.666666666666667e-7, "m3")},
        {QStringLiteral("localY"), QJsonArray{0.0, 1.0, 0.0}}});
    spec.insert(QStringLiteral("supports"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
        {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                                             QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")}}}});
    spec.insert(QStringLiteral("loads"), QJsonArray{QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("pointForce")},
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("components"), QJsonArray{quantity(0.0, "N"), quantity(0.0, "N"), quantity(-1.0, "N")}}}});
    spec.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("quantity"), QStringLiteral("displacement")},
        {QStringLiteral("component"), QStringLiteral("uz")}}});
    return spec;
}

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), operation}, {QStringLiteral("parameters"), parameters}};
}

QJsonObject projectIdentity(ModelCommandHost *host)
{
    if (!host) return {};
    const QJsonObject response = host->handle(hostRequest(QStringLiteral("project.query")));
    if (response.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok"))) return {};
    return response.value(QStringLiteral("result")).toObject();
}

bool openProjectWithModal(Session *session, const QString &workspace, uint *modelId,
                          QString *error = nullptr)
{
    if (!session) return false;
    session->clear();
    if (!session->openProject(workspace, error)) return false;
    session->addModel(modalConversationFixture());
    const uint id = session->getNModels() - 1;
    session->setModelSelected(id, true);
    if (modelId) *modelId = id;
    return true;
}

RAgentSettings testProviderSettings(const QString &apiKey)
{
    RAgentSettings settings(RAgentSettings::Type::DeepSeek);
    settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
    settings.setModel(QStringLiteral("history-widget-fixture"));
    settings.setApiKey(apiKey);
    return settings;
}

struct NetworkScript
{
    QByteArray body;
    int status = 200;
    int delayMs = 1;
    bool deliverAfterAbort = false;
};

class HistoryFixtureReply final : public QNetworkReply
{
public:
    HistoryFixtureReply(const QNetworkRequest &request, QNetworkAccessManager *manager,
                        NetworkScript script)
        : QNetworkReply(manager), script(std::move(script))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::PostOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, this->script.status);
        setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void begin()
    {
        QTimer::singleShot(script.delayMs, this, [this] {
            if (isFinished()) return;
            bytes = script.body;
            emit readyRead();
            setFinished(true);
            emit finished();
        });
    }

    void abort() override
    {
        if (isFinished() || script.deliverAfterAbort) return;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("fixture request cancelled"));
        emit errorOccurred(QNetworkReply::OperationCanceledError);
        setFinished(true);
        emit finished();
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return bytes.size() - readPosition + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        if (readPosition >= bytes.size()) return isFinished() ? -1 : 0;
        const qint64 count = qMin(maximum, qint64(bytes.size() - readPosition));
        std::memcpy(destination, bytes.constData() + readPosition, size_t(count));
        readPosition += qsizetype(count);
        return count;
    }

private:
    NetworkScript script;
    QByteArray bytes;
    qsizetype readPosition = 0;
};

class HistoryFixtureNetwork final : public QNetworkAccessManager
{
public:
    void enqueue(NetworkScript script) { scripts.append(std::move(script)); }

    QList<QNetworkRequest> requests;
    QList<QByteArray> requestBodies;
    int finishedReplies = 0;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        Q_UNUSED(operation);
        requests.append(request);
        requestBodies.append(outgoingData ? outgoingData->readAll() : QByteArray());
        NetworkScript script;
        if (scripts.isEmpty())
        {
            script.status = 500;
            script.body = QByteArray("no provider script was enqueued");
        }
        else script = scripts.takeFirst();
        auto *reply = new HistoryFixtureReply(request, this, std::move(script));
        connect(reply, &QNetworkReply::finished, this, [this] { ++finishedReplies; });
        QTimer::singleShot(0, reply, &HistoryFixtureReply::begin);
        return reply;
    }

private:
    QList<NetworkScript> scripts;
};

NetworkScript completionScript(const QString &text, int delayMs = 1,
                               bool deliverAfterAbort = false,
                               const QString &privateReasoning = {},
                               const QString &providerModel = {})
{
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                        {QStringLiteral("content"), text}};
    if (!privateReasoning.isEmpty())
        message.insert(QStringLiteral("reasoning_content"), privateReasoning);
    QJsonObject response{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("finish_reason"), QStringLiteral("stop")},
        {QStringLiteral("message"), message}}}},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 987654321},
            {QStringLiteral("completion_tokens"), 123456789},
            {QStringLiteral("total_tokens"), 1111111110}}}};
    if (!providerModel.isEmpty()) response.insert(QStringLiteral("model"), providerModel);
    return {QJsonDocument(response).toJson(QJsonDocument::Compact), 200, delayMs, deliverAfterAbort};
}

NetworkScript toolCallScript(const QString &callId, const QString &name,
                             const QJsonObject &arguments, const QString &text,
                             const QString &privateReasoning = {},
                             const QString &providerModel = {})
{
    QString effectiveName = name;
    QJsonObject effectiveArguments = arguments;
    if (name == QStringLiteral("document_propose")
        && arguments.value(QStringLiteral("command")) == QJsonValue(QStringLiteral("experiment.run")))
    {
        effectiveName = QStringLiteral("experiment_run");
        effectiveArguments = {{QStringLiteral("documentId"), arguments.value(QStringLiteral("documentId"))},
            {QStringLiteral("documentRevision"), arguments.value(QStringLiteral("documentRevision"))}};
        const QJsonObject commandArguments = arguments.value(QStringLiteral("arguments")).toObject();
        for (auto it = commandArguments.constBegin(); it != commandArguments.constEnd(); ++it)
            effectiveArguments.insert(it.key(), it.value());
    }
    const QJsonObject providerCall{{QStringLiteral("id"), callId},
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), effectiveName},
            {QStringLiteral("arguments"), QString::fromUtf8(
                 QJsonDocument(effectiveArguments).toJson(QJsonDocument::Compact))}}}};
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), text}, {QStringLiteral("tool_calls"), QJsonArray{providerCall}}};
    if (!privateReasoning.isEmpty())
        message.insert(QStringLiteral("reasoning_content"), privateReasoning);
    QJsonObject response{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("finish_reason"), QStringLiteral("tool_calls")},
        {QStringLiteral("message"), message}}}},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 987654321},
            {QStringLiteral("completion_tokens"), 123456789},
            {QStringLiteral("total_tokens"), 1111111110}}}};
    if (!providerModel.isEmpty()) response.insert(QStringLiteral("model"), providerModel);
    return {QJsonDocument(response).toJson(QJsonDocument::Compact), 200, 1, false};
}

QJsonObject modalChangeArguments(ModelCommandHost *host, Session *session, uint modelId,
                                 uint iterations = 24)
{
    const QPair<QString, quint64> stamp = session->modelCommandStamp(modelId);
    const QJsonObject response = host->handle(hostRequest(QStringLiteral("context.query")));
    for (const QJsonValue &value : response.value(QStringLiteral("result")).toObject()
             .value(QStringLiteral("documents")).toArray())
    {
        const QJsonObject document = value.toObject();
        if (document.value(QStringLiteral("documentId")).toString() != stamp.first) continue;
        return {{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), document.value(QStringLiteral("documentRevision"))},
            {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("method"), QStringLiteral("multiple")},
                {QStringLiteral("iterations"), int(iterations)},
                {QStringLiteral("modes"), 2}, {QStringLiteral("tolerance"), 1e-6},
                {QStringLiteral("unit"), QStringLiteral("1")}}}};
    }
    return {};
}

bool configureConversationStore(RAgentConversationStore *store, ModelCommandHost *host,
                                QString *projectId = nullptr, QString *error = nullptr)
{
    if (!store || !host) return false;
    const QJsonObject identity = projectIdentity(host);
    const QString id = identity.value(QStringLiteral("projectId")).toString();
    const QString workspace = host->projectWorkspace();
    if (id.isEmpty() || workspace.isEmpty()) return false;
    if (projectId) *projectId = id;
    return store->configure(workspace, id, error);
}

bool hasVisibleText(const QJsonObject &snapshot, const QString &needle)
{
    for (const QJsonValue &value : snapshot.value(QStringLiteral("messages")).toArray())
        if (value.toObject().value(QStringLiteral("text")).toString().contains(needle)) return true;
    return false;
}

QString toolResultForCall(const QByteArray &requestBody, const QString &callId)
{
    const QJsonObject request = QJsonDocument::fromJson(requestBody).object();
    for (const QJsonValue &value : request.value(QStringLiteral("messages")).toArray())
    {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")) == QJsonValue(QStringLiteral("tool"))
            && message.value(QStringLiteral("tool_call_id")) == QJsonValue(callId))
            return message.value(QStringLiteral("content")).toString();
    }
    return {};
}

QString sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

QString sha256Directory(const QString &path)
{
    QDir root(path);
    QStringList files;
    QDirIterator iterator(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) files.append(root.relativeFilePath(iterator.next()));
    std::sort(files.begin(), files.end());
    if (files.isEmpty()) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &relative : files)
    {
        hash.addData(relative.toUtf8());
        hash.addData("\0", 1);
        QFile file(root.filePath(relative));
        if (!file.open(QIODevice::ReadOnly)) return {};
        while (!file.atEnd())
        {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return {};
            hash.addData(chunk);
        }
        hash.addData("\0", 1);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject readBoundedJsonObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > 64 * 1024) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

bool saveEvidence(QWidget *widget, const QString &name)
{
    const QString directory = qEnvironmentVariable("RANGE_AGENT_HISTORY_EVIDENCE_DIRECTORY");
    if (directory.isEmpty()) return true;
    if (!widget || !QDir().mkpath(directory)) return false;
    return widget->grab().save(QDir(directory).filePath(name));
}

bool exportThroughVisibleMenuAction(QAction *action, const QString &target)
{
    if (!action) return false;
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs = qScopeGuard([nativeDialogsDisabled] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    });
    bool selected = false;
    QTimer finder;
    finder.setInterval(10);
    QObject::connect(&finder, &QTimer::timeout, qApp, [&] {
        for (QWidget *topLevel : QApplication::topLevelWidgets())
        {
            auto *dialog = qobject_cast<QFileDialog *>(topLevel);
            if (!dialog) continue;
            dialog->selectFile(target);
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            auto *save = buttons ? buttons->button(QDialogButtonBox::Save) : nullptr;
            if (!save || !save->isEnabled()) continue;
            QTest::mouseClick(save, Qt::LeftButton);
            selected = true;
            finder.stop();
            return;
        }
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, qApp, [] {
        if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    finder.start();
    timeout.start(5000);
    action->trigger();
    timeout.stop();
    finder.stop();
    return selected && QFileInfo(target).isFile();
}

#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_AGENT_HISTORY_SOURCE_ROOT_CONFIGURED 0
#define RANGE_WORKFLOW_SOURCE_ROOT ""
#else
#define RANGE_AGENT_HISTORY_SOURCE_ROOT_CONFIGURED 1
#endif

bool loadNativeTrussFixture(const QString &directory, const QString &stem,
                            Model *model, QString *sourcePath, QString *error)
{
    if (!model) return false;
    const QString fixture = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT))
        .filePath(QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram"));
    const QString target = QDir(directory).filePath(stem + QStringLiteral(".ram"));
    if (fixture.isEmpty() || !QFileInfo(fixture).isFile() || !QFile::copy(fixture, target))
    {
        if (error) *error = QStringLiteral("The stage9_truss4.ram native fixture is unavailable or could not be copied.");
        return false;
    }
    try { ModelIO::open(*model, target); }
    catch (const RError &exception)
    {
        if (error) *error = exception.getMessage();
        return false;
    }
    // Keep the structural source while removing the stage-nine controller binding,
    // which belongs to its separate reference-workflow CLI scenario.
    model->getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    model->getProblemSetup().getStructuralDynamicsSetup().setEnabled(false);
    model->getProblemSetup().setRestart(false);
    model->getTimeSolver().setEnabled(false);
    if (model->getNLines() == 0)
    {
        if (error) *error = QStringLiteral("The native fixture has no material-bearing line.");
        return false;
    }
    for (uint index = 0; index < model->getNPoints(); ++index)
        model->getPoint(index).setMaterial(model->getLine(0).getMaterial());
    constexpr uint steps = 20;
    constexpr uint outputFrequency = 1;
    constexpr double timeStep = 0.001;
    RStructuralDynamicsSetup &dynamics = model->getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
    dynamics.setTimeStep(timeStep);
    dynamics.setNSteps(steps);
    dynamics.setOutputFrequency(outputFrequency);
    RTimeSolver &time = model->getTimeSolver();
    time.setEnabled(true);
    time.setInputStartTime(0.0);
    time.setInputNTimeSteps(steps);
    time.setInputTimeStepSize(timeStep);
    time.setOutputFrequency(outputFrequency);
    time.setComputedTime(0.0);
    time.setTimes(RTimeSolver::findTimesVector(steps, 0.0, timeStep));
    time.setCurrentTimeStep(0);
    try { ModelIO::save(*model, target, false); }
    catch (const RError &exception)
    {
        if (error) *error = exception.getMessage();
        return false;
    }
    if (sourcePath) *sourcePath = target;
    return model->getNNodes() > 0 && model->getNElements() > 0;
}

class HistoryNativeThreadPoolGateRunnable final : public QRunnable
{
public:
    HistoryNativeThreadPoolGateRunnable(const std::shared_ptr<QSemaphore> &started,
                                        const std::shared_ptr<QSemaphore> &release)
        : started(started), release(release) { setAutoDelete(true); }

    void run() override
    {
        started->release();
        release->tryAcquire(1, 120000);
    }

private:
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
};

class HistoryNativeThreadPoolGate
{
public:
    HistoryNativeThreadPoolGate()
        : pool(QThreadPool::globalInstance()), previousMaximum(pool->maxThreadCount()),
          started(std::make_shared<QSemaphore>()), release(std::make_shared<QSemaphore>()) {}

    ~HistoryNativeThreadPoolGate()
    {
        release->release();
        pool->waitForDone(120000);
        if (maximumChanged) pool->setMaxThreadCount(previousMaximum);
    }

    bool start()
    {
        if (!pool->waitForDone(15000)) return false;
        pool->setMaxThreadCount(1);
        maximumChanged = true;
        pool->start(new HistoryNativeThreadPoolGateRunnable(started, release));
        return started->tryAcquire(1, 10000);
    }

    void releaseNow() { release->release(); }

private:
    QThreadPool *pool;
    int previousMaximum;
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
    bool maximumChanged = false;
};

class HistoryNativeJobReaper
{
public:
    explicit HistoryNativeJobReaper(SolverManager *manager) : manager(manager) {}
    void setJobId(const QString &id) { jobId = id; }
    void disarm() { jobId.clear(); }

    ~HistoryNativeJobReaper()
    {
        if (!manager || jobId.isEmpty()) return;
        const auto active = [this] {
            const QString state = manager->jobQuery(jobId).value(QStringLiteral("state")).toString();
            return state == QStringLiteral("queued") || state == QStringLiteral("running")
                || state == QStringLiteral("cancelling");
        };
        if (active())
        {
            QString ignored;
            manager->cancelJob(jobId, &ignored);
        }
        QElapsedTimer timer;
        timer.start();
        while (active() && timer.elapsed() < 120000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
    }

private:
    SolverManager *manager = nullptr;
    QString jobId;
};

bool waitForAgentNativeManagerIdle(SolverManager &manager)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000)
    {
        if (manager.getNRunning() == 0 && manager.getNWaiting() == 0
            && RJobManager::getInstance().getNRunning() == 0
            && RJobManager::getInstance().getNWaiting() == 0) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(10);
    }
    return false;
}
}

class TestAgentConversationHistoryWidget : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        session->clear();
    }

    void cleanup()
    {
        Application *application = Application::instance();
        if (application && application->getSession()) application->getSession()->clear();
    }

    void trustedExperimentPreviewShowsPhysicsAndRequiresApprovalInAutoMode()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), true);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const auto stamp = session->modelCommandStamp(modelId);
        const QJsonObject arguments{{QStringLiteral("problemSpec"), trustedRodProblemSpec()},
            {QStringLiteral("allowedNumericalRanges"), QJsonArray{QJsonObject{
                {QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                {QStringLiteral("minimum"), 2}, {QStringLiteral("maximum"), 4}}}},
            {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 2},
                {QStringLiteral("repairs"), 1}, {QStringLiteral("modelTurns"), 3}}}};
        const QJsonObject proposal{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), arguments}};

        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("trusted-experiment-review"),
            QStringLiteral("document_propose"), proposal,
            QStringLiteral("I prepared a trusted rod experiment.")));
        network.enqueue(completionScript(QStringLiteral("I stopped after your review decision.")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
                           AgentTaskRunner::AccessPolicy::ReadWrite, false);
        widget.setProviderSettings(testProviderSettings(QStringLiteral("agent-ui-review-test-key")));
        widget.resize(900, 980);
        widget.show();

        auto *approval = widget.findChild<QFrame *>(QStringLiteral("agentApprovalCard"));
        auto *preview = widget.findChild<QTextBrowser *>(QStringLiteral("agentPreview"));
        auto *approve = widget.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"));
        auto *reject = widget.findChild<QPushButton *>(QStringLiteral("agentRejectProposal"));
        auto *newChat = widget.findChild<QToolButton *>(QStringLiteral("agentNewChat"));
        auto *automatic = widget.findChild<QCheckBox *>(QStringLiteral("agentAutomaticChanges"));
        QVERIFY(approval);
        QVERIFY(approval->parentWidget() == &widget);
        QVERIFY(preview);
        QVERIFY(approve);
        QVERIFY(reject);
        QVERIFY(newChat);
        QVERIFY(automatic);
        QVERIFY(automatic->isChecked());

        QSignalSpy proposals(widget.taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        widget.startTask(QStringLiteral("Review the trusted rod experiment."), true);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(approval->isVisible(), 3000);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
        QVERIFY(widget.taskRunner()->isActive());
        QVERIFY(approve->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(widget.childAt(approve->mapTo(&widget, approve->rect().center())),
                                  static_cast<QWidget *>(approve), 2000);
        const QString review = preview->toPlainText();
        const QString agentTaskId = widget.taskRunner()->taskId();
        QVERIFY(!agentTaskId.isEmpty());
        QVERIFY(review.contains(QStringLiteral("Agent task ID: %1").arg(agentTaskId)));
        QVERIFY(review.contains(QStringLiteral("Trusted mechanics experiment")));
        QVERIFY(review.contains(QStringLiteral("normalized SI")));
        QVERIFY(review.contains(QStringLiteral("Agent UI review rod")));
        QVERIFY(review.contains(QStringLiteral("youngModulus")));
        QVERIFY(review.contains(QStringLiteral("210000000000")));
        QVERIFY(review.contains(QStringLiteral("Authorized numerical ranges")));
        QVERIFY(review.contains(QStringLiteral("/geometry/segments")));
        QVERIFY(review.contains(QStringLiteral("Solve / repair / model-turn budgets")));
        QVERIFY(review.contains(QStringLiteral("source remains unchanged")));

        const QJsonObject pendingProposal = widget.taskRunner()->pendingProposal();
        const QString previewId = pendingProposal.value(QStringLiteral("previewId")).toString();
        const QString experimentId = pendingProposal.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!previewId.isEmpty());
        QVERIFY(!experimentId.isEmpty());
        QVERIFY(review.contains(QStringLiteral("Preview ID: %1").arg(previewId)));
        QVERIFY(review.contains(QStringLiteral("proposed experiment ID: %1").arg(experimentId)));
        const QString authorizationDigest = pendingProposal.value(QStringLiteral("authorizationDigest")).toString();
        QVERIFY(!authorizationDigest.isEmpty());
        QVERIFY(review.contains(QStringLiteral("Authorization digest: %1").arg(authorizationDigest)));
        const QJsonObject sourceBinding = pendingProposal.value(QStringLiteral("sourceBinding")).toObject();
        const QString snapshotSha256 = sourceBinding.value(QStringLiteral("snapshotSha256")).toString();
        QCOMPARE(snapshotSha256.size(), 64);
        QVERIFY(review.contains(QStringLiteral("Source snapshot SHA-256: %1").arg(snapshotSha256)));
        const QString sourceSessionId = sourceBinding.value(QStringLiteral("sessionId")).toString();
        const QString sourceSessionEpoch = sourceBinding.value(QStringLiteral("sessionEpoch")).toString();
        QVERIFY(!sourceSessionId.isEmpty());
        QVERIFY(!sourceSessionEpoch.isEmpty());
        QVERIFY(sourceSessionId != sourceSessionEpoch);
        QVERIFY(review.contains(QStringLiteral("Source session: %1 · epoch %2")
            .arg(sourceSessionId, sourceSessionEpoch)));

        QJsonObject malformedPreview = pendingProposal;
        QJsonObject malformedBinding = sourceBinding;
        malformedBinding.insert(QStringLiteral("snapshotSha256"), QString(64, QLatin1Char('z')));
        malformedPreview.insert(QStringLiteral("sourceBinding"), malformedBinding);
        const QString proposalId = proposals.first().at(1).toString();
        QVERIFY(!proposalId.isEmpty());
        Q_EMIT widget.taskRunner()->proposalReady(QStringLiteral("experiment"), proposalId, malformedPreview);
        QTRY_VERIFY_WITH_TIMEOUT(!approve->isEnabled(), 2000);
        QVERIFY(preview->toPlainText().contains(QStringLiteral("Host approval details are unavailable")));
        Q_EMIT widget.taskRunner()->proposalReady(QStringLiteral("experiment"), proposalId, pendingProposal);
        QTRY_VERIFY_WITH_TIMEOUT(approve->isEnabled(), 2000);

        reject->click();
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(newChat->isEnabled(), 2000);
        const QString finishedConversationId = widget.conversationId();
        newChat->click();
        QTRY_VERIFY_WITH_TIMEOUT(!widget.conversationId().isEmpty(), 2000);
        QVERIFY(widget.conversationId() != finishedConversationId);
        auto *conversationState = widget.findChild<QLabel *>(QStringLiteral("agentConversationStatus"));
        auto *historyStatus = widget.findChild<QLabel *>(QStringLiteral("agentHistoryStatus"));
        QVERIFY(conversationState);
        QVERIFY(historyStatus);
        QCOMPARE(conversationState->text(), QStringLiteral("Ready"));
        QVERIFY(historyStatus->text().contains(QStringLiteral("0 messages")));
        QCOMPARE(session->modelCommandStamp(modelId), stamp);
        const QJsonObject rejectedExperiment = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}}));
        QCOMPARE(rejectedExperiment.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(rejectedExperiment.value(QStringLiteral("code")).toString(), QStringLiteral("EXPERIMENT_NOT_FOUND"));
    }

    void trustedRodExperimentRunsThroughApprovalWaitAndHostEvidence()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), true);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        RAgentConversationStore conversationStoreProbe;
        QVERIFY2(configureConversationStore(&conversationStoreProbe, host, nullptr, &error), qPrintable(error));
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QJsonObject sourceBefore = host->query(modelId);
        QVERIFY(!sourceStamp.first.isEmpty());

        const QJsonObject proposalArguments{{QStringLiteral("documentId"), sourceStamp.first},
            {QStringLiteral("documentRevision"), double(sourceStamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
                {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                    {QStringLiteral("repairs"), 0}, {QStringLiteral("modelTurns"), 3}}}}}};

        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("trusted-rod-submit"),
            QStringLiteral("document_propose"), proposalArguments,
            QStringLiteral("I prepared the rod analysis for approval."), {},
            QStringLiteral("history-widget-fixture")));
        QTemporaryDir captureDirectory;
        QVERIFY(captureDirectory.isValid());
        RAgentEvaluationCaptureStore captureStore;
        QString captureError;
        QVERIFY2(captureStore.configure(captureDirectory.filePath(QStringLiteral("agent-host-capture.jsonl")),
                                        &captureError), qPrintable(captureError));
        QList<QJsonObject> captureEvents;
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
                           AgentTaskRunner::AccessPolicy::ReadWrite, false);
        widget.setProviderSettings(testProviderSettings(QStringLiteral("agent-rod-e2e-test-key")));
        QVERIFY(widget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            captureEvents.append(event);
            return captureStore.append(event, &captureError);
        }));
        widget.resize(900, 980);
        widget.show();

        auto *approval = widget.findChild<QFrame *>(QStringLiteral("agentApprovalCard"));
        auto *approve = widget.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"));
        QVERIFY(approval);
        QVERIFY(approve);
        QSignalSpy proposals(widget.taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy observed(widget.taskRunner(), &AgentTaskRunner::experimentObserved);
        QSignalSpy accepted(widget.taskRunner(), &AgentTaskRunner::executionObserved);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);

        widget.startTask(QStringLiteral("Run and verify this trusted rod problem."), true);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(approval->isVisible(), 3000);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
        const QString experimentId = widget.taskRunner()->pendingProposal()
            .value(QStringLiteral("experimentId")).toString();
        QVERIFY(!experimentId.isEmpty());

        network.enqueue(toolCallScript(QStringLiteral("trusted-rod-wait"),
            QStringLiteral("experiment_wait"),
            QJsonObject{{QStringLiteral("experimentId"), experimentId}},
            QStringLiteral("I will wait for the Host result."), {},
            QStringLiteral("history-widget-fixture")));
        network.enqueue(completionScript(QStringLiteral("The Host reports the rod candidate passed verification."),
            1, false, {}, QStringLiteral("history-widget-fixture")));
        approve->click();

        QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 10000);
        QCOMPARE(accepted.first().at(1).toString(), QStringLiteral("experiment"));
        QCOMPARE(accepted.first().at(2).toJsonObject().value(QStringLiteral("experimentId")).toString(), experimentId);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(observed.size(), 1, 90000);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));

        const QJsonObject hostSummary = observed.first().at(2).toJsonObject();
        QCOMPARE(observed.first().at(1).toString(), experimentId);
        QCOMPARE(hostSummary.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
        QVERIFY(hostSummary.value(QStringLiteral("verificationPassed")).toBool());
        QVERIFY(hostSummary.value(QStringLiteral("verification")).toObject()
                    .value(QStringLiteral("metrics")).isObject());
        QVERIFY(!QJsonDocument(hostSummary).toJson(QJsonDocument::Compact).contains(workspace.path().toUtf8()));
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(host->query(modelId), sourceBefore);

        const QString waitFacts = toolResultForCall(network.requestBodies.at(2), QStringLiteral("trusted-rod-wait"));
        QVERIFY2(waitFacts.contains(QStringLiteral("candidate_ready")), qPrintable(waitFacts));
        QVERIFY(waitFacts.contains(QStringLiteral("verificationPassed")));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(transcript);
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Trusted Host evidence")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("passed=true")));

        QList<QJsonObject> hostObservations;
        QList<QJsonObject> hostEvidence;
        QList<QJsonObject> providerResponses;
        QList<QJsonObject> dispatchIntents;
        for (const QJsonObject &event : captureEvents)
        {
            if (event.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("provider_response")))
                providerResponses.append(event);
            if (event.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("dispatch_intent")))
                dispatchIntents.append(event);
            if (event.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("host_observation")))
                hostObservations.append(event);
            if (event.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("host_evidence")))
                hostEvidence.append(event);
        }
        QCOMPARE(providerResponses.size(), 3);
        QCOMPARE(dispatchIntents.size(), 3);
        const QString configuredModelSha256 = QString::fromLatin1(QCryptographicHash::hash(
            QByteArrayLiteral("history-widget-fixture"), QCryptographicHash::Sha256).toHex());
        for (const QJsonObject &response : providerResponses)
        {
            bool matchedDispatch = false;
            for (const QJsonObject &dispatch : dispatchIntents)
                if (dispatch.value(QStringLiteral("requestId")) == response.value(QStringLiteral("requestId")))
                {
                    QCOMPARE(dispatch.value(QStringLiteral("provider")).toString(), QStringLiteral("DeepSeek"));
                    QCOMPARE(dispatch.value(QStringLiteral("settings")).toObject()
                                 .value(QStringLiteral("model")).toString(),
                             QStringLiteral("history-widget-fixture"));
                    matchedDispatch = true;
                    break;
                }
            QVERIFY(matchedDispatch);
            QCOMPARE(response.value(QStringLiteral("returnedModelStatus")).toString(),
                     QStringLiteral("matches_configured"));
            QCOMPARE(response.value(QStringLiteral("returnedModelSha256")).toString(), configuredModelSha256);
        }
        QVERIFY(!hostObservations.isEmpty());
        QCOMPARE(hostEvidence.size(), 1);
        const QJsonObject evidence = hostEvidence.first();
        QVERIFY(!evidence.contains(QStringLiteral("provider")));
        QCOMPARE(evidence.value(QStringLiteral("toolName")).toString(), QStringLiteral("experiment_wait"));
        QCOMPARE(evidence.value(QStringLiteral("experimentId")).toString(), experimentId);
        QCOMPARE(evidence.value(QStringLiteral("sourceBinding")).toObject()
                     .value(QStringLiteral("documentId")).toString(), sourceStamp.first);
        QCOMPARE(evidence.value(QStringLiteral("sourceBinding")).toObject()
                     .value(QStringLiteral("documentRevision")).toString(), QString::number(sourceStamp.second));
        QVERIFY(evidence.value(QStringLiteral("authorizationDigest")).toString().size() == 64);
        QCOMPARE(evidence.value(QStringLiteral("childJob")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
        QCOMPARE(evidence.value(QStringLiteral("verificationReport")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("pass"));
        QVERIFY(evidence.value(QStringLiteral("verificationReport")).toObject()
                    .value(QStringLiteral("passed")).toBool());
        QCOMPARE(evidence.value(QStringLiteral("evidenceSchema")).toString(),
                 QStringLiteral("range.trusted-agent-stage7-physical-evidence/v1"));
        const QJsonObject capturedVerification = evidence.value(QStringLiteral("verificationReport")).toObject();
        QCOMPARE(capturedVerification.value(QStringLiteral("metricsCaptureStatus")).toString(),
                 QStringLiteral("complete"));
        const QByteArray capturedMetricsJson = capturedVerification.value(QStringLiteral("metricsJson")).toString().toUtf8();
        QVERIFY(!capturedMetricsJson.isEmpty());
        const QJsonObject capturedMetrics = QJsonDocument::fromJson(capturedMetricsJson).object();
        QVERIFY(!capturedMetrics.isEmpty());
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(capturedMetricsJson,
                     QCryptographicHash::Sha256).toHex()),
                 capturedVerification.value(QStringLiteral("metricsSha256")).toString());
        QVERIFY(capturedMetrics.contains(QStringLiteral("identity")));
        QCOMPARE(evidence.value(QStringLiteral("solveIdentity")).toObject()
                     .value(QStringLiteral("jobId")),
                 evidence.value(QStringLiteral("childJob")).toObject().value(QStringLiteral("jobId")));
        QCOMPARE(evidence.value(QStringLiteral("verificationReport")).toObject()
                     .value(QStringLiteral("identity")).toObject().value(QStringLiteral("manifestSha256")),
                 evidence.value(QStringLiteral("childJob")).toObject().value(QStringLiteral("resultManifestSha256")));

        QFile persistedCapture(captureDirectory.filePath(QStringLiteral("agent-host-capture.jsonl")));
        QVERIFY(persistedCapture.open(QIODevice::ReadOnly));
        const QByteArray captureBytes = persistedCapture.readAll();
        QVERIFY(!captureBytes.isEmpty());
        QVERIFY(!captureBytes.contains(workspace.path().toUtf8()));
        const QString captureExportPath = qEnvironmentVariable("RANGE_TEST_CAPTURE_EXPORT");
        if (!captureExportPath.isEmpty())
        {
            QFile exportedCapture(captureExportPath);
            QVERIFY2(exportedCapture.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                     qPrintable(exportedCapture.errorString()));
            QCOMPARE(exportedCapture.write(captureBytes), qint64(captureBytes.size()));
            QVERIFY(exportedCapture.flush());
        }
    }

    void trustedBeamConvergenceRunsThroughAgentApprovalWaitAndEvidence()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), true);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QJsonObject sourceBefore = host->query(modelId);
        const QJsonObject problem = trustedCantileverBeamProblemSpec();
        const QJsonObject proposalArguments{{QStringLiteral("documentId"), sourceStamp.first},
            {QStringLiteral("documentRevision"), double(sourceStamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("problemSpec"), problem},
                {QStringLiteral("allowedNumericalRanges"), QJsonArray{QJsonObject{
                    {QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                    {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 4}}}},
                {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 3},
                    {QStringLiteral("repairs"), 2}, {QStringLiteral("modelTurns"), 4}}},
                {QStringLiteral("convergenceStudy"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("mesh")},
                    {QStringLiteral("maximumLevels"), 3}}}}}};

        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("trusted-beam-submit"),
            QStringLiteral("document_propose"), proposalArguments,
            QStringLiteral("I prepared the cantilever beam mesh study for review.")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
                           AgentTaskRunner::AccessPolicy::ReadWrite, false);
        widget.setProviderSettings(testProviderSettings(QStringLiteral("agent-beam-e2e-test-key")));
        widget.resize(900, 980);
        widget.show();

        auto *approval = widget.findChild<QFrame *>(QStringLiteral("agentApprovalCard"));
        auto *preview = widget.findChild<QTextBrowser *>(QStringLiteral("agentPreview"));
        auto *approve = widget.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"));
        QVERIFY(approval && preview && approve);
        QSignalSpy proposals(widget.taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy observed(widget.taskRunner(), &AgentTaskRunner::experimentObserved);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);

        widget.startTask(QStringLiteral("Run and verify a cantilever beam mesh study."), true);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(approval->isVisible(), 3000);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
        const QString review = preview->toPlainText();
        QVERIFY(review.contains(QStringLiteral("cantileverBeam")));
        QVERIFY(review.contains(QStringLiteral("Authorized numerical ranges")));
        QVERIFY(review.contains(QStringLiteral("maximumLevels")));

        const QString experimentId = widget.taskRunner()->pendingProposal()
            .value(QStringLiteral("experimentId")).toString();
        QVERIFY(!experimentId.isEmpty());
        network.enqueue(toolCallScript(QStringLiteral("trusted-beam-wait"),
            QStringLiteral("experiment_wait"),
            QJsonObject{{QStringLiteral("experimentId"), experimentId}},
            QStringLiteral("I will wait for the Host-verified beam convergence result.")));
        network.enqueue(completionScript(QStringLiteral("The Host reports verified beam convergence.")));
        approve->click();

        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(observed.size(), 1, 120000);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));

        const QJsonObject summary = observed.first().at(2).toJsonObject();
        QCOMPARE(summary.value(QStringLiteral("experimentId")).toString(), experimentId);
        QCOMPARE(summary.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
        QVERIFY(summary.value(QStringLiteral("verificationPassed")).toBool());
        const QJsonObject convergence = summary.value(QStringLiteral("convergence")).toObject();
        QCOMPARE(convergence.value(QStringLiteral("status")).toString(), QStringLiteral("converged"));
        QCOMPARE(convergence.value(QStringLiteral("levelsCompleted")).toInt(), 3);
        QCOMPARE(convergence.value(QStringLiteral("adjacentChanges")).toArray().size(), 2);
        for (const QJsonValue &change : convergence.value(QStringLiteral("adjacentChanges")).toArray())
            QVERIFY(change.toObject().value(QStringLiteral("belowTolerance")).toBool());
        QVERIFY(!QJsonDocument(summary).toJson(QJsonDocument::Compact).contains(workspace.path().toUtf8()));
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(host->query(modelId), sourceBefore);

        const QString waitFacts = toolResultForCall(network.requestBodies.at(2), QStringLiteral("trusted-beam-wait"));
        QVERIFY(waitFacts.contains(QStringLiteral("converged")));
        QVERIFY(waitFacts.contains(QStringLiteral("verificationPassed")));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(transcript);
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Trusted Host evidence")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("converged")));
    }

    void trustedMechanicsClarificationResumesOnlyAfterUserSuppliesMaterial()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), true);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QJsonObject sourceBefore = host->query(modelId);
        QJsonObject incompleteProblem = trustedRodProblemSpec();
        incompleteProblem.remove(QStringLiteral("material"));
        const QJsonObject diagnoseArguments{{QStringLiteral("documentId"), sourceStamp.first},
            {QStringLiteral("documentRevision"), double(sourceStamp.second)},
            {QStringLiteral("scope"), QStringLiteral("candidate")},
            {QStringLiteral("problemSpec"), incompleteProblem}};
        QJsonObject completeProblem = trustedRodProblemSpec();
        QJsonObject completedMaterial = completeProblem.value(QStringLiteral("material")).toObject();
        completedMaterial.insert(QStringLiteral("source"), QStringLiteral("user-provided clarification"));
        completeProblem.insert(QStringLiteral("material"), completedMaterial);
        const QJsonObject proposalArguments{{QStringLiteral("documentId"), sourceStamp.first},
            {QStringLiteral("documentRevision"), double(sourceStamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("problemSpec"), completeProblem},
                {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                    {QStringLiteral("repairs"), 0}, {QStringLiteral("modelTurns"), 5}}}}}};

        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("trusted-missing-diagnosis"),
            QStringLiteral("trusted_problem_diagnose"), diagnoseArguments,
            QStringLiteral("I will inspect the incomplete candidate before deciding whether to proceed.")));
        network.enqueue(toolCallScript(QStringLiteral("trusted-material-question"),
            QStringLiteral("ask_user"), QJsonObject{
                {QStringLiteral("question"), QStringLiteral("Which material properties should I use? The candidate has no material data; I will not infer them.")},
                {QStringLiteral("options"), QJsonArray{QStringLiteral("Steel: E=210 GPa, nu=0.3, density=7850 kg/m3"),
                    QStringLiteral("I will provide different properties")}}},
            QStringLiteral("The deterministic diagnosis reports missing material inputs.")));
        network.enqueue(toolCallScript(QStringLiteral("trusted-material-completed-proposal"),
            QStringLiteral("document_propose"), proposalArguments,
            QStringLiteral("Using the material values you provided, I prepared a candidate.")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
                           AgentTaskRunner::AccessPolicy::ReadWrite, false);
        widget.setProviderSettings(testProviderSettings(QStringLiteral("agent-clarification-e2e-test-key")));
        widget.resize(900, 980);
        widget.show();

        QSignalSpy questions(widget.taskRunner(), &AgentTaskRunner::userInputRequested);
        QSignalSpy proposals(widget.taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy observed(widget.taskRunner(), &AgentTaskRunner::experimentObserved);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        widget.startTask(QStringLiteral("Analyze this rod. Ask before filling missing physical parameters."), true);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
        QCOMPARE(proposals.size(), 0);
        QVERIFY(widget.taskRunner()->isAwaitingInput());
        QVERIFY(widget.taskRunner()->pendingProposal().isEmpty());
        const auto diagnosisToolResult = toolResultForCall(network.requestBodies.at(1),
            QStringLiteral("trusted-missing-diagnosis"));
        QVERIFY2(diagnosisToolResult.contains(QStringLiteral("SPEC_MISSING")), qPrintable(diagnosisToolResult));
        auto *questionCard = widget.findChild<QFrame *>(QStringLiteral("agentQuestionCard"));
        QVERIFY(questionCard);
        QTRY_VERIFY_WITH_TIMEOUT(questionCard->isVisible(), 3000);
        const auto questionText = widget.findChild<QLabel *>(QStringLiteral("agentQuestionText"));
        QVERIFY(questionText);
        QVERIFY(questionText->text().contains(QStringLiteral("no material data")));
        const QList<QPushButton *> answers = questionCard->findChildren<QPushButton *>();
        QCOMPARE(answers.size(), 2);
        const QString suppliedMaterial = QStringLiteral("Steel: E=210 GPa, nu=0.3, density=7850 kg/m3");
        answers.first()->click();

        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 10000);
        QVERIFY(network.requestBodies.at(2).contains(suppliedMaterial.toUtf8()));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 10000);
        const QString experimentId = widget.taskRunner()->pendingProposal()
            .value(QStringLiteral("experimentId")).toString();
        QVERIFY(!experimentId.isEmpty());
        auto *preview = widget.findChild<QTextBrowser *>(QStringLiteral("agentPreview"));
        auto *approve = widget.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"));
        QVERIFY(preview && approve);
        QVERIFY(preview->toPlainText().contains(QStringLiteral("210000000000")));
        network.enqueue(toolCallScript(QStringLiteral("trusted-material-wait"),
            QStringLiteral("experiment_wait"),
            QJsonObject{{QStringLiteral("experimentId"), experimentId}},
            QStringLiteral("I will verify the Host result.")));
        network.enqueue(completionScript(QStringLiteral("The Host reports the completed rod result.")));
        approve->click();

        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 4, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(observed.size(), 1, 90000);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 5, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        const QJsonObject summary = observed.first().at(2).toJsonObject();
        QCOMPARE(summary.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
        QVERIFY(summary.value(QStringLiteral("verificationPassed")).toBool());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(host->query(modelId), sourceBefore);
        const QString waitFacts = toolResultForCall(network.requestBodies.at(4), QStringLiteral("trusted-material-wait"));
        QVERIFY(waitFacts.contains(QStringLiteral("verificationPassed")));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(transcript && transcript->toPlainText().contains(QStringLiteral("Trusted Host evidence")));
    }

    void hostCancellationFactIsShownIndependentlyFromAssistantText()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        Q_UNUSED(modelId);
        WorkflowProcess workflow;
        HistoryFixtureNetwork network;
        AgentWidget widget(session->getModelCommandHost(), &workflow, nullptr, &network,
                           AgentTaskRunner::AccessPolicy::ReadWrite, false);
        widget.resize(820, 900);
        widget.show();
        const QString experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString documentId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QJsonObject sourceBinding{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), session->getModelCommandHost()->sessionEpoch()},
            {QStringLiteral("documentRevision"), 17},
            {QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('a'))}};
        const QJsonObject fact{{QStringLiteral("schema"), QStringLiteral("range.agent-experiment-cancellation/v1")},
            {QStringLiteral("trigger"), QStringLiteral("user_stop")},
            {QStringLiteral("status"), QStringLiteral("cancelled")},
            {QStringLiteral("code"), QStringLiteral("CANCELLED")},
            {QStringLiteral("beforeState"), QStringLiteral("prepared")},
            {QStringLiteral("afterState"), QStringLiteral("cancelled")},
            {QStringLiteral("cancellationPersisted"), true},
            {QStringLiteral("childCancellationRequestFailed"), false},
            {QStringLiteral("childJobsTerminal"), true},
            {QStringLiteral("childJobSetMatches"), true},
            {QStringLiteral("childJobCount"), 0}, {QStringLiteral("terminalChildJobCount"), 0},
            {QStringLiteral("childJobStates"), QStringLiteral("none")}};

        const QString taskId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        widget.taskRunner()->experimentCancellationObserved(taskId, experimentId,
                                                              sourceBinding, fact);
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(transcript);
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Trusted Host cancellation")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Cancellation status: cancelled (CANCELLED)")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("child job set matches=true; child jobs terminal=true (0/0), states=none")));
        QVERIFY(transcript->toPlainText().contains(documentId));

        const QString failedExperimentId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QJsonObject unavailableFact{{QStringLiteral("schema"), QStringLiteral("range.agent-experiment-cancellation/v1")},
            {QStringLiteral("status"), QStringLiteral("unknown")},
            {QStringLiteral("code"), QStringLiteral("HOST_QUERY_FAILED")}};
        widget.taskRunner()->experimentCancellationObserved(taskId, failedExperimentId, {}, unavailableFact);
        QVERIFY(transcript->toPlainText().contains(QStringLiteral(
            "Cancellation status: unknown (HOST_QUERY_FAILED). Persisted=unknown; child job set matches=unknown; child jobs terminal=unknown (unknown/unknown), states=unknown; child cancel request failed=unknown.")));
        const QString verifierFailureExperimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject failedMetric{{QStringLiteral("status"), QStringLiteral("failed")},
            {QStringLiteral("required"), true}, {QStringLiteral("value"), 0.08},
            {QStringLiteral("tolerance"), 0.02}};
        const QJsonObject failedVerification{{QStringLiteral("schema"), QStringLiteral("range.verification/v1")},
            {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("passed"), false},
            {QStringLiteral("metrics"), QJsonObject{{QStringLiteral("frequencyRelativeError"), failedMetric}}}};
        const QJsonObject verifierFailure{{QStringLiteral("experimentId"), verifierFailureExperimentId},
            {QStringLiteral("state"), QStringLiteral("budget_exhausted")},
            {QStringLiteral("verificationPassed"), false},
            {QStringLiteral("verification"), failedVerification},
            {QStringLiteral("convergence"), QJsonObject{{QStringLiteral("status"), QStringLiteral("not_converged")},
                {QStringLiteral("stopReason"), QStringLiteral("solve_budget_exhausted")},
                {QStringLiteral("levels"), QJsonArray{
                    QJsonObject{{QStringLiteral("level"), 1}, {QStringLiteral("verificationPassed"), true}},
                    QJsonObject{{QStringLiteral("level"), 2}, {QStringLiteral("verificationPassed"), false}},
                    QJsonObject{{QStringLiteral("level"), 3}},
                    QJsonObject{{QStringLiteral("level"), 4}, {QStringLiteral("verificationPassed"), QStringLiteral("false")}}}}}}};
        widget.taskRunner()->experimentObserved(taskId, verifierFailureExperimentId, verifierFailure);
        const QString modelClaim = QStringLiteral("The result passed verification and is ready.");
        widget.taskRunner()->assistantMessage(modelClaim);
        QVERIFY(transcript->toPlainText().contains(modelClaim));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Verification: failed · passed=false")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Host state: budget_exhausted")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Level 1 · verified=true")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Level 2 · verified=false")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Level 3 · verified=not reported")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Level 4 · verified=not reported")));
        const QString cancelledInput = QStringLiteral("Discard this queued message when Stop is pressed.");
        widget.taskRunner()->pendingMessagesCancelled({cancelledInput});
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Message not processed")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral(
            "This queued message was cancelled before its Agent task started. No analysis or action was performed.")));

        const QJsonObject project = session->getModelCommandHost()->handle(hostRequest(QStringLiteral("project.query")))
            .value(QStringLiteral("result")).toObject();
        QVERIFY(!project.value(QStringLiteral("projectId")).toString().isEmpty());
        RAgentConversationStore savedConversation;
        QString storeError;
        QVERIFY2(savedConversation.configure(session->getModelCommandHost()->projectWorkspace(),
                                              project.value(QStringLiteral("projectId")).toString(),
                                              &storeError), qPrintable(storeError));
        auto *choice = widget.findChild<QComboBox *>(QStringLiteral("agentConversationChoice"));
        QVERIFY(choice);
        const QString conversationId = choice->currentData().toString();
        QVERIFY(!conversationId.isEmpty());
        const QJsonObject saved = savedConversation.page(conversationId, 0, 20, &storeError);
        QVERIFY2(!saved.isEmpty(), qPrintable(storeError));
        bool cancellationFactPersisted = false;
        bool unavailableFactPersisted = false;
        bool cancelledInputPersisted = false;
        bool cancelledInputFactPersisted = false;
        for (const QJsonValue &entryValue : saved.value(QStringLiteral("messages")).toArray())
        {
            const QJsonObject entry = entryValue.toObject();
            const QJsonObject savedFacts = entry.value(QStringLiteral("facts")).toObject();
            if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("user")
                && entry.value(QStringLiteral("text")).toString() == cancelledInput)
                cancelledInputPersisted = true;
            if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("fact")
                && savedFacts.value(QStringLiteral("experimentId")).toString() == experimentId)
            {
                cancellationFactPersisted = true;
                QCOMPARE(savedFacts.value(QStringLiteral("childJobStates")).toString(), QStringLiteral("none"));
                QCOMPARE(savedFacts.value(QStringLiteral("childJobCount")).toString(), QStringLiteral("0"));
                QCOMPARE(savedFacts.value(QStringLiteral("childJobSetMatches")).toString(), QStringLiteral("true"));
            }
            if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("fact")
                && savedFacts.value(QStringLiteral("experimentId")).toString() == failedExperimentId)
            {
                unavailableFactPersisted = true;
                QVERIFY(!savedFacts.contains(QStringLiteral("cancellationPersisted")));
                QVERIFY(!savedFacts.contains(QStringLiteral("childJobCount")));
                QVERIFY(!savedFacts.contains(QStringLiteral("childJobSetMatches")));
            }
            if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("fact")
                && savedFacts.value(QStringLiteral("code")).toString()
                    == QStringLiteral("INPUT_CANCELLED_BEFORE_TASK_START"))
            {
                cancelledInputFactPersisted = true;
                QCOMPARE(savedFacts.value(QStringLiteral("status")).toString(),
                         QStringLiteral("cancelled_before_start"));
            }
        }
        QVERIFY(cancellationFactPersisted);
        QVERIFY(unavailableFactPersisted);
        QVERIFY(cancelledInputPersisted);
        QVERIFY(cancelledInputFactPersisted);

        AgentWidget reloaded(session->getModelCommandHost(), &workflow, nullptr, &network,
                             AgentTaskRunner::AccessPolicy::ReadWrite, true);
        reloaded.show();
        auto *reloadedTranscript = reloaded.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(reloadedTranscript);
        QTRY_VERIFY_WITH_TIMEOUT(reloadedTranscript->toPlainText().contains(cancelledInput), 3000);
        QVERIFY(reloadedTranscript->toPlainText().contains(QStringLiteral(
            "This queued message was cancelled before its Agent task started. No analysis or action was performed.")));
    }

    void pendingPrimaryWriteSurvivesReadOnlyCancelAndDeniedAutoProposal()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), false);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const QJsonObject documentBefore = host->query(modelId);
        const QJsonObject proposalArguments = modalChangeArguments(host, session, modelId, 25);
        QVERIFY(!proposalArguments.isEmpty());

        const QString apiKey = QStringLiteral("history-widget-key-private-sentinel");
        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("primary-modal-proposal"),
            QStringLiteral("document_propose"), proposalArguments,
            QStringLiteral("I prepared a modal update for review.")));
        network.enqueue(completionScript(QStringLiteral("Late cancelled read-only answer."), 180, true));
        network.enqueue(toolCallScript(QStringLiteral("readonly-document-write"),
            QStringLiteral("document_propose"), proposalArguments,
            QStringLiteral("I will attempt a forbidden write.")));
        network.enqueue(completionScript(QStringLiteral("The read-only task stayed within its access policy.")));

        WorkflowProcess workflow;
        AgentWidget primary(host, &workflow, nullptr, &network,
                            AgentTaskRunner::AccessPolicy::ReadWrite, false);
        primary.setProviderSettings(testProviderSettings(apiKey));
        primary.resize(820, 920);
        primary.show();
        auto *composer = primary.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        auto *approval = primary.findChild<QFrame *>(QStringLiteral("agentApprovalCard"));
        auto *approve = primary.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"));
        QVERIFY(composer);
        QVERIFY(approval);
        QVERIFY(approve);
        QSignalSpy primaryProposals(primary.taskRunner(), &AgentTaskRunner::proposalReady);

        primary.startTask(QStringLiteral("Prepare a modal update and wait for my review."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(primaryProposals.size(), 1, 3000);
        QVERIFY(primary.taskRunner()->isActive());
        QVERIFY(!primary.taskRunner()->pendingProposal().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(approval->isVisible(), 2000);
        const QJsonObject pendingProposal = primary.taskRunner()->pendingProposal();
        composer->setPlainText(QStringLiteral("Primary draft must survive the independent question."));

        primary.startReadOnlyQuestion(QStringLiteral("Ask a separate read-only question."));
        AgentWidget *readOnly = primary.readOnlyWidget();
        QVERIFY(readOnly);
        QCOMPARE(readOnly->objectName(), QStringLiteral("agentReadOnlyTaskWidget"));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 3000);
        const QString readOnlyConversationId = readOnly->conversationId();
        QVERIFY(!readOnlyConversationId.isEmpty());
        readOnly->taskRunner()->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!readOnly->taskRunner()->isActive(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(network.finishedReplies, 2, 3000);
        auto *readOnlyTranscript = readOnly->findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(readOnlyTranscript);
        QVERIFY(!readOnlyTranscript->toPlainText().contains(QStringLiteral("Late cancelled read-only answer.")));

        // Exercise the immutable read-only policy with executeRequestedActions=true,
        // after the actual widget has already captured its fixture transport settings.
        auto *readOnlyComposer = readOnly->findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        auto *automatic = readOnly->findChild<QCheckBox *>(QStringLiteral("agentAutomaticChanges"));
        auto *readOnlyAsk = readOnly->findChild<QPushButton *>(QStringLiteral("agentAskReadOnly"));
        auto *readOnlyApproval = readOnly->findChild<QFrame *>(QStringLiteral("agentApprovalCard"));
        QVERIFY(readOnlyComposer);
        QVERIFY(automatic);
        QVERIFY(readOnlyAsk);
        QVERIFY(automatic->isHidden());
        QVERIFY(readOnlyAsk->isHidden());
        QVERIFY(readOnlyApproval);
        readOnlyComposer->setPlainText(QStringLiteral("Try the write tool even though this task is read-only."));
        QSignalSpy readOnlyProposals(readOnly->taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy readOnlyActions(readOnly->taskRunner(), &AgentTaskRunner::actionCompleted);
        QSignalSpy readOnlyFinished(readOnly->taskRunner(), &AgentTaskRunner::finished);
        readOnly->taskRunner()->startTask(QStringLiteral("Try the write tool even though this task is read-only."), true);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(readOnlyFinished.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 4, 3000);
        QCOMPARE(readOnlyFinished.first().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(readOnlyProposals.size(), 0);
        QCOMPARE(readOnlyActions.size(), 0);
        QVERIFY(readOnly->taskRunner()->pendingProposal().isEmpty());
        QVERIFY(!readOnlyApproval->isVisible());
        const QString denied = toolResultForCall(network.requestBodies.at(3),
                                                 QStringLiteral("readonly-document-write"));
        QVERIFY2(denied.contains(QStringLiteral("READ_ONLY_TOOL_DENIED")), qPrintable(denied));
        QCOMPARE(host->query(modelId), documentBefore);
        QVERIFY(readOnlyTranscript->toPlainText().contains(
            QStringLiteral("The read-only task stayed within its access policy.")));

        QCOMPARE(primary.taskRunner()->pendingProposal(), pendingProposal);
        QVERIFY(primary.taskRunner()->isActive());
        QCOMPARE(composer->toPlainText(), QStringLiteral("Primary draft must survive the independent question."));
        QVERIFY(!readOnlyConversationId.isEmpty());
        QVERIFY(readOnlyConversationId != primary.conversationId());
        QVERIFY(saveEvidence(&primary, QStringLiteral("primary-pending-write.png")));
        QVERIFY(saveEvidence(readOnly, QStringLiteral("readonly-independent-answer.png")));
    }

    void changedOrClosedSourceAndProjectSwitchRejectLateReadOnlyAnswer()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        for (const bool closeSource : {false, true})
        {
            QTemporaryDir workspace;
            QVERIFY(workspace.isValid());
            Session *session = application->getSession();
            uint modelId = 0;
            QString error;
            QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
            ModelCommandHost *host = session->getModelCommandHost();
            QVERIFY(host);
            HistoryFixtureNetwork network;
            network.enqueue(completionScript(QStringLiteral("Stale source answer must not persist."), 100));
            WorkflowProcess workflow;
            AgentWidget primary(host, &workflow, nullptr, &network,
                                AgentTaskRunner::AccessPolicy::ReadWrite, false);
            primary.setProviderSettings(testProviderSettings(QStringLiteral("stale-source-key")));
            primary.startReadOnlyQuestion(closeSource
                ? QStringLiteral("Question while the selected source is about to close.")
                : QStringLiteral("Question while the selected source is about to change."));
            AgentWidget *readOnly = primary.readOnlyWidget();
            QVERIFY(readOnly);
            QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 3000);
            const QString conversationId = readOnly->conversationId();
            QVERIFY(!conversationId.isEmpty());
            RAgentConversationStore store;
            QVERIFY2(configureConversationStore(&store, host, nullptr, &error), qPrintable(error));

            if (closeSource) session->removeModel(modelId);
            else
            {
                session->getModel(modelId).setDescription(QStringLiteral("Source revision changed while the provider was thinking."));
                session->setModelChanged(modelId);
            }
            QSignalSpy finished(readOnly->taskRunner(), &AgentTaskRunner::finished);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
            QTRY_COMPARE_WITH_TIMEOUT(network.finishedReplies, 1, 3000);
            const QJsonObject saved = store.exportConversation(conversationId, &error);
            QVERIFY2(!saved.isEmpty(), qPrintable(error));
            QVERIFY(!hasVisibleText(saved, QStringLiteral("Stale source answer must not persist.")));
            QCOMPARE(saved.value(QStringLiteral("messages")).toArray().first().toObject()
                         .value(QStringLiteral("role")).toString(), QStringLiteral("user"));
        }

        QTemporaryDir workspaceA;
        QTemporaryDir workspaceB;
        QVERIFY(workspaceA.isValid() && workspaceB.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspaceA.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        HistoryFixtureNetwork network;
        network.enqueue(completionScript(QStringLiteral("Project A answer must not enter project B."), 120));
        WorkflowProcess workflow;
        AgentWidget primary(host, &workflow, nullptr, &network,
                            AgentTaskRunner::AccessPolicy::ReadWrite, false);
        primary.setProviderSettings(testProviderSettings(QStringLiteral("project-switch-key")));
        primary.startReadOnlyQuestion(QStringLiteral("Question pinned to project A."));
        AgentWidget *readOnly = primary.readOnlyWidget();
        QVERIFY(readOnly);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 3000);
        const QString conversationA = readOnly->conversationId();
        QVERIFY(!conversationA.isEmpty());
        RAgentConversationStore storeA;
        QVERIFY2(configureConversationStore(&storeA, host, nullptr, &error), qPrintable(error));

        session->removeModel(modelId);
        QVERIFY2(session->openProject(workspaceB.path(), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!readOnly->taskRunner()->isActive(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(network.finishedReplies, 1, 3000);
        const QJsonObject savedA = storeA.exportConversation(conversationA, &error);
        QVERIFY2(!savedA.isEmpty(), qPrintable(error));
        QVERIFY(!hasVisibleText(savedA, QStringLiteral("Project A answer must not enter project B.")));
        QVERIFY(readOnly->conversationId().isEmpty());

        RAgentConversationStore storeB;
        QString projectBId;
        QVERIFY2(configureConversationStore(&storeB, host, &projectBId, &error), qPrintable(error));
        const QJsonObject listingB = storeB.list(0, 32, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(listingB.value(QStringLiteral("projectId")).toString(), projectBId);
        QCOMPARE(listingB.value(QStringLiteral("totalCount")).toInt(), 0);
    }

    void branchReloadExportAndResendPreserveOnlyVisibleHistoryAndAcceptedFacts()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), false);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QTemporaryDir exportedFiles;
        QVERIFY(workspace.isValid() && exportedFiles.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const QJsonObject firstProposal = modalChangeArguments(host, session, modelId, 26);
        QVERIFY(!firstProposal.isEmpty());

        const QString apiKey = QStringLiteral("history-export-key-private-sentinel");
        const QString privateReasoning = QStringLiteral("private-reasoning-sentinel-should-never-persist");
        const QString pendingCallId = QStringLiteral("unapproved-private-proposal-call");
        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("accepted-modal-proposal"),
            QStringLiteral("document_propose"), firstProposal,
            QStringLiteral("I prepared the accepted modal change."), privateReasoning));
        network.enqueue(completionScript(QStringLiteral("The modal settings were updated."), 1, false, privateReasoning));

        QString originalConversationId;
        QJsonObject acceptedState;
        QPair<QString, quint64> acceptedStamp;
        QJsonObject originalExport;
        QString pendingProposalId;
        {
            WorkflowProcess workflow;
            AgentWidget widget(host, &workflow, nullptr, &network,
                               AgentTaskRunner::AccessPolicy::ReadWrite, false);
            widget.setProviderSettings(testProviderSettings(apiKey));
            widget.resize(820, 920);
            widget.show();
            QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
            QSignalSpy actions(widget.taskRunner(), &AgentTaskRunner::actionCompleted);
            QSignalSpy proposals(widget.taskRunner(), &AgentTaskRunner::proposalReady);
            const QString firstPrompt = QStringLiteral("The user accidentally included credential %1 in this message.").arg(apiKey);
            widget.startTask(firstPrompt);
            QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 3000);
            QTRY_VERIFY_WITH_TIMEOUT(widget.findChild<QFrame *>(QStringLiteral("agentApprovalCard"))->isVisible(), 3000);
            QCOMPARE(widget.taskRunner()->pendingProposal().value(QStringLiteral("command")).toString(),
                     QStringLiteral("analysis.modal.configure"));
            widget.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"))->click();
            QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
            QCOMPARE(actions.size(), 1);
            acceptedState = host->query(modelId);
            acceptedStamp = session->modelCommandStamp(modelId);

            const QJsonObject unapprovedProposal = modalChangeArguments(host, session, modelId, 28);
            QVERIFY(!unapprovedProposal.isEmpty());
            network.enqueue(toolCallScript(pendingCallId, QStringLiteral("document_propose"),
                unapprovedProposal, QStringLiteral("A second proposal is waiting for review.")));
            widget.startTask(QStringLiteral("Leave another proposal pending so reload cannot restore approval."));
            QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 3000);
            QTRY_VERIFY_WITH_TIMEOUT(widget.findChild<QFrame *>(QStringLiteral("agentApprovalCard"))->isVisible(), 3000);
            QCOMPARE(proposals.size(), 2);
            pendingProposalId = proposals.last().at(1).toString();
            originalConversationId = widget.conversationId();
            QVERIFY(!originalConversationId.isEmpty());
            widget.taskRunner()->cancel();
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 3000);
            QVERIFY(widget.taskRunner()->pendingProposal().isEmpty());

            RAgentConversationStore store;
            QVERIFY2(configureConversationStore(&store, host, nullptr, &error), qPrintable(error));
            originalExport = store.exportConversation(originalConversationId, &error);
            QVERIFY2(!originalExport.isEmpty(), qPrintable(error));
            const QByteArray storedBytes = QJsonDocument(originalExport).toJson(QJsonDocument::Compact);
            const QJsonArray persistedMessages = originalExport.value(QStringLiteral("messages")).toArray();
            QVERIFY(!persistedMessages.isEmpty());
            QCOMPARE(persistedMessages.first().toObject().value(QStringLiteral("role")).toString(),
                     QStringLiteral("user"));
            QVERIFY(persistedMessages.first().toObject().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("[REDACTED]")));
            QVERIFY(!storedBytes.contains(apiKey.toUtf8()));
            QVERIFY(!storedBytes.contains(privateReasoning.toUtf8()));
            QVERIFY(!storedBytes.contains(QByteArray("987654321")));
            QVERIFY(!storedBytes.contains(pendingCallId.toUtf8()));
            QVERIFY(!storedBytes.contains(pendingProposalId.toUtf8()));
            QVERIFY(!storedBytes.contains(QByteArray("tool_calls")));
            QVERIFY(!storedBytes.contains(QByteArray("reasoning_content")));
            bool foundAcceptedFact = false;
            for (const QJsonValue &value : originalExport.value(QStringLiteral("messages")).toArray())
            {
                const QJsonObject entry = value.toObject();
                const QJsonObject facts = entry.value(QStringLiteral("facts")).toObject();
                if (entry.value(QStringLiteral("role")) == QJsonValue(QStringLiteral("fact"))
                    && facts.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("accepted"))
                    && facts.value(QStringLiteral("command")) == QJsonValue(QStringLiteral("analysis.modal.configure")))
                    foundAcceptedFact = true;
            }
            QVERIFY(foundAcceptedFact);
        }

        {
            WorkflowProcess workflow;
            AgentWidget reloaded(host, &workflow, nullptr, &network,
                                AgentTaskRunner::AccessPolicy::ReadWrite, false);
            reloaded.resize(820, 920);
            reloaded.show();
            QVERIFY2(reloaded.loadConversation(originalConversationId, &error), qPrintable(error));
            QVERIFY(reloaded.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"))->toPlainText()
                .contains(QStringLiteral("Historical host fact")));
            QVERIFY(reloaded.taskRunner()->pendingProposal().isEmpty());
            QVERIFY(!reloaded.findChild<QFrame *>(QStringLiteral("agentApprovalCard"))->isVisible());
            QVERIFY(!reloaded.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"))->isEnabled());
            QVERIFY(saveEvidence(&reloaded, QStringLiteral("reloaded-accepted-history.png")));
            reloaded.setProviderSettings(testProviderSettings(apiKey));

            RAgentConversationStore store;
            QVERIFY2(configureConversationStore(&store, host, nullptr, &error), qPrintable(error));
            const QJsonArray originalMessages = originalExport.value(QStringLiteral("messages")).toArray();
            int acceptedFactIndex = -1;
            QString acceptedFactId;
            for (int index = 0; index < originalMessages.size(); ++index)
            {
                const QJsonObject entry = originalMessages.at(index).toObject();
                const QJsonObject facts = entry.value(QStringLiteral("facts")).toObject();
                if (entry.value(QStringLiteral("role")) == QJsonValue(QStringLiteral("fact"))
                    && facts.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("accepted"))
                    && facts.value(QStringLiteral("command")) == QJsonValue(QStringLiteral("analysis.modal.configure")))
                {
                    acceptedFactIndex = index;
                    acceptedFactId = entry.value(QStringLiteral("messageId")).toString();
                    break;
                }
            }
            QVERIFY(acceptedFactIndex >= 0);
            auto *branchChoice = reloaded.findChild<QComboBox *>(QStringLiteral("agentBranchMessage"));
            auto *branchButton = reloaded.findChild<QToolButton *>(QStringLiteral("agentBranchConversation"));
            QVERIFY(branchChoice);
            QVERIFY(branchButton);
            const int acceptedFactChoice = branchChoice->findData(acceptedFactId);
            QVERIFY(acceptedFactChoice >= 0);
            branchChoice->setCurrentIndex(acceptedFactChoice);
            const QString branchSourceId = reloaded.conversationId();
            branchButton->click();
            QVERIFY(!reloaded.conversationId().isEmpty());
            QVERIFY(reloaded.conversationId() != branchSourceId);
            const QString branchId = reloaded.conversationId();
            network.enqueue(completionScript(QStringLiteral("The branch follow-up did not run an action.")));
            QVERIFY(reloaded.taskRunner()->pendingProposal().isEmpty());
            QVERIFY(!reloaded.findChild<QFrame *>(QStringLiteral("agentApprovalCard"))->isVisible());
            const QJsonObject branchExport = store.exportConversation(branchId, &error);
            QVERIFY2(!branchExport.isEmpty(), qPrintable(error));
            QCOMPARE(branchExport.value(QStringLiteral("messages")).toArray().size(), acceptedFactIndex + 1);
            QCOMPARE(branchExport.value(QStringLiteral("branchSource")).toObject()
                         .value(QStringLiteral("conversationId")).toString(), originalConversationId);
            const QJsonObject copiedAcceptedFact = branchExport.value(QStringLiteral("messages")).toArray()
                .last().toObject();
            QCOMPARE(copiedAcceptedFact.value(QStringLiteral("role")).toString(), QStringLiteral("fact"));
            QVERIFY(copiedAcceptedFact.value(QStringLiteral("messageId")).toString() != acceptedFactId);
            QCOMPARE(copiedAcceptedFact.value(QStringLiteral("branchOrigin")).toObject()
                         .value(QStringLiteral("messageId")).toString(), acceptedFactId);
            QVERIFY(saveEvidence(&reloaded, QStringLiteral("branched-history.png")));

            auto *composer = reloaded.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
            auto *send = reloaded.findChild<QPushButton *>(QStringLiteral("agentSend"));
            QVERIFY(composer);
            QVERIFY(send);
            composer->setPlainText(QStringLiteral("Please explain the accepted setting without applying it again."));
            QSignalSpy actions(reloaded.taskRunner(), &AgentTaskRunner::actionCompleted);
            QSignalSpy finished(reloaded.taskRunner(), &AgentTaskRunner::finished);
            send->click();
            QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 4, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
            QCOMPARE(actions.size(), 0);
            QCOMPARE(host->query(modelId), acceptedState);
            QCOMPARE(session->modelCommandStamp(modelId), acceptedStamp);
            const QJsonObject resendRequest = QJsonDocument::fromJson(network.requestBodies.last()).object();
            for (const QJsonValue &value : resendRequest.value(QStringLiteral("messages")).toArray())
            {
                const QJsonObject message = value.toObject();
                QVERIFY(message.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("tool")));
                QVERIFY(!message.contains(QStringLiteral("tool_calls")));
            }
            const QJsonObject unchangedOriginal = store.exportConversation(originalConversationId, &error);
            QVERIFY2(!unchangedOriginal.isEmpty(), qPrintable(error));
            QCOMPARE(unchangedOriginal.value(QStringLiteral("revision")), originalExport.value(QStringLiteral("revision")));

            const QByteArray exportedPath = QDir(exportedFiles.path()).filePath(QStringLiteral("verified-history.json")).toUtf8();
            QAction *exportAction = reloaded.findChild<QAction *>(QStringLiteral("agentExportConversation"));
            QVERIFY(exportAction);
            QVERIFY2(exportThroughVisibleMenuAction(exportAction, QString::fromUtf8(exportedPath)),
                     "The actual conversation export menu action did not publish its verified JSON file.");
            QFile exportedFile(QString::fromUtf8(exportedPath));
            QVERIFY(exportedFile.open(QIODevice::ReadOnly));
            const QByteArray exportedBytes = exportedFile.readAll();
            const QJsonObject published = QJsonDocument::fromJson(exportedBytes).object();
            QCOMPARE(published.value(QStringLiteral("conversationId")).toString(), branchId);
            QVERIFY(!exportedBytes.contains(apiKey.toUtf8()));
            QVERIFY(!exportedBytes.contains(privateReasoning.toUtf8()));
            QVERIFY(!exportedBytes.contains(QByteArray("987654321")));
            QVERIFY(!exportedBytes.contains(pendingCallId.toUtf8()));
            QVERIFY(!exportedBytes.contains(pendingProposalId.toUtf8()));
            QVERIFY(!exportedBytes.contains(QByteArray("tool_calls")));

            QAction *deleteAction = reloaded.findChild<QAction *>(QStringLiteral("agentDeleteConversation"));
            QVERIFY(deleteAction);
            deleteAction->trigger();
            QVERIFY(reloaded.conversationId().isEmpty());
            QVERIFY(store.exportConversation(branchId, &error).isEmpty());
            QVERIFY(store.exportConversation(originalConversationId, &error).size() > 0);
        }
    }

    void boundedHistoryPagesExposeOldFactsToTheNextQuestion()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        Q_UNUSED(modelId);
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        QString projectId;
        RAgentConversationStore store;
        QVERIFY2(configureConversationStore(&store, host, &projectId, &error), qPrintable(error));
        store.setSensitiveValues({QStringLiteral("pagination-private-key")});
        const QJsonObject created = store.create(QStringLiteral("Long bounded transcript"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString conversationId = created.value(QStringLiteral("conversationId")).toString();
        QString revision = created.value(QStringLiteral("revision")).toString();
        QJsonArray entries;
        for (int index = 0; index < 62; ++index)
        {
            const bool fact = index == 0 || index % 10 == 0;
            QJsonObject entry{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("role"), fact ? QStringLiteral("fact")
                    : index % 2 ? QStringLiteral("assistant") : QStringLiteral("user")},
                {QStringLiteral("text"), index == 0
                    ? QStringLiteral("Old accepted result fact: old-result-fact-marker")
                    : QStringLiteral("Visible bounded history message %1").arg(index)}};
            if (fact) entry.insert(QStringLiteral("facts"), QJsonObject{
                {QStringLiteral("status"), QStringLiteral("accepted")},
                {QStringLiteral("code"), QStringLiteral("OK")},
                {QStringLiteral("command"), QStringLiteral("analysis.run")},
                {QStringLiteral("jobId"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
            entries.append(entry);
        }
        for (int offset = 0; offset < entries.size(); offset += RAgentConversationStore::MaxPageSize)
        {
            QJsonArray batch;
            const int count = qMin(RAgentConversationStore::MaxPageSize, entries.size() - offset);
            for (int index = 0; index < count; ++index) batch.append(entries.at(offset + index));
            const QJsonObject appended = store.append(conversationId, revision, batch, &error);
            QVERIFY2(!appended.isEmpty(), qPrintable(error));
            revision = appended.value(QStringLiteral("revision")).toString();
        }

        HistoryFixtureNetwork network;
        network.enqueue(completionScript(QStringLiteral("The historical fact was available to this query.")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
                           AgentTaskRunner::AccessPolicy::ReadWrite, false);
        widget.setProviderSettings(testProviderSettings(QStringLiteral("pagination-private-key")));
        widget.resize(820, 920);
        widget.show();
        QVERIFY2(widget.loadConversation(conversationId, &error), qPrintable(error));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        auto *previous = widget.findChild<QPushButton *>(QStringLiteral("agentHistoryPrevious"));
        auto *next = widget.findChild<QPushButton *>(QStringLiteral("agentHistoryNext"));
        auto *composer = widget.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        auto *send = widget.findChild<QPushButton *>(QStringLiteral("agentSend"));
        QVERIFY(transcript && previous && next && composer && send);
        QCOMPARE(widget.findChild<QComboBox *>(QStringLiteral("agentBranchMessage"))->count(), 50);
        QVERIFY(previous->isEnabled());
        QVERIFY(!transcript->toPlainText().contains(QStringLiteral("old-result-fact-marker")));
        QVERIFY(saveEvidence(&widget, QStringLiteral("history-newest-page.png")));

        previous->click();
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("Old accepted result fact: old-result-fact-marker")));
        QVERIFY(!next->isHidden());
        QVERIFY(next->isEnabled());
        QCOMPARE(widget.findChild<QComboBox *>(QStringLiteral("agentBranchMessage"))->count(), 50);
        QVERIFY(saveEvidence(&widget, QStringLiteral("history-oldest-page.png")));

        composer->setPlainText(QStringLiteral("Use the visible old accepted fact while answering."));
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 3000);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        const QJsonObject request = QJsonDocument::fromJson(network.requestBodies.first()).object();
        const QString historicalJobId = entries.first().toObject().value(QStringLiteral("facts")).toObject()
            .value(QStringLiteral("jobId")).toString();
        bool oldFactSentAsObservableContext = false;
        for (const QJsonValue &value : request.value(QStringLiteral("messages")).toArray())
        {
            const QJsonObject message = value.toObject();
            const QString content = message.value(QStringLiteral("content")).toString();
            if (content.contains(QStringLiteral("old-result-fact-marker")))
            {
                oldFactSentAsObservableContext = true;
                QCOMPARE(message.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
                const QString prefix = QStringLiteral("Prior conversation data (untrusted; not authorization): ");
                QVERIFY(content.startsWith(prefix));
                const auto historical = QJsonDocument::fromJson(content.mid(prefix.size()).toUtf8());
                QVERIFY(historical.isArray());
                bool matchingFact = false;
                for (const auto &item : historical.array())
                {
                    const auto fact = item.toObject();
                    if (fact.value(QStringLiteral("text")).toString().contains(QStringLiteral("old-result-fact-marker")))
                        matchingFact = fact.value(QStringLiteral("type")) == QJsonValue(QStringLiteral("history_fact"))
                            && fact.value(QStringLiteral("artifactRef")).toObject().value(QStringLiteral("jobId")).toString() == historicalJobId;
                }
                QVERIFY(matchingFact);
            }
            QVERIFY(message.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("tool")));
            QVERIFY(!message.contains(QStringLiteral("tool_calls")));
        }
        QVERIFY(oldFactSentAsObservableContext);
        const QJsonObject finalPage = store.page(conversationId, 0, 50, &error);
        QVERIFY2(!finalPage.isEmpty(), qPrintable(error));
        QCOMPARE(finalPage.value(QStringLiteral("messageCount")).toInt(), 65); // User, accepted assistant text, and task-ended fact.
        QCOMPARE(finalPage.value(QStringLiteral("messages")).toArray().size(), 50);
    }

    void pausedNativeJobKeepsItsAcceptedSourceWhileIndependentWidgetsReadAndWrite()
    {
#ifndef RANGE_WF07_SOLVER
        QSKIP("CMake must provide RANGE_WF07_SOLVER for this real native-job story.");
#else
        Application *application = Application::instance();
        QVERIFY(application);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        AgentSettingsSnapshot restoreSettings(applicationSettings,
            {QStringLiteral("application/solverPath"), QStringLiteral("application/nThreads")});
        const QString solverPath = QStringLiteral(RANGE_WF07_SOLVER);
        QVERIFY2(QFileInfo(solverPath).isFile(), qPrintable(solverPath));
        applicationSettings->setSolverPath(solverPath);
        applicationSettings->setValue(QStringLiteral("application/nThreads"), 4);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), false);
        applicationSettings->setValue(QStringLiteral("aiAgent/streamingResponses"), false);

#if !RANGE_AGENT_HISTORY_SOURCE_ROOT_CONFIGURED
        QSKIP("CMake must provide RANGE_WORKFLOW_SOURCE_ROOT for the pinned native source fixture.");
#endif
        QTemporaryDir workspace;
        QTemporaryDir nativeInputs;
        QVERIFY(workspace.isValid() && nativeInputs.isValid());
        Session *session = application->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        Model nativeModel;
        QString nativeSourcePath;
        QVERIFY2(loadNativeTrussFixture(nativeInputs.path(), QStringLiteral("wf07-paused-source"),
                                        &nativeModel, &nativeSourcePath, &error), qPrintable(error));
        const QString nativeSourceSha = sha256File(nativeSourcePath);
        QVERIFY(!nativeSourceSha.isEmpty());
        session->addModel(nativeModel);
        const uint nativeModelId = session->getNModels() - 1;
        session->setModelSelected(nativeModelId, false);
        session->addModel(modalConversationFixture());
        const uint interactiveModelId = session->getNModels() - 1;
        session->setModelSelected(interactiveModelId, true);
        const QPair<QString, quint64> nativeSourceStamp = session->modelCommandStamp(nativeModelId);
        const QJsonObject nativeAcceptedState = session->getModelCommandHost()->query(nativeModelId);
        const QPair<QString, quint64> interactiveSourceStamp = session->modelCommandStamp(interactiveModelId);
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const QJsonObject proposalArguments = modalChangeArguments(host, session, interactiveModelId, 27);
        QVERIFY(!proposalArguments.isEmpty());

        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForAgentNativeManagerIdle(manager));
        HistoryNativeThreadPoolGate gate;
        QVERIFY2(gate.start(), "The bounded global worker-pool gate could not start.");
        HistoryNativeJobReaper reaper(&manager);
        const QString jobId = manager.submitValidated(nativeModelId, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        reaper.setJobId(jobId);
        const QJsonObject queuedJob = manager.jobQuery(jobId);
        QCOMPARE(queuedJob.value(QStringLiteral("state")).toString(), QStringLiteral("queued"));
        QVERIFY2(manager.requestNativeAcceptedBoundaryPause(jobId, 10, &error), qPrintable(error));
        const QJsonObject pauseRequested = manager.jobQuery(jobId);
        QCOMPARE(pauseRequested.value(QStringLiteral("nativeExecutionPauseRequestedSeq")).toInt(), 10);
        const QString executionStatePath = pauseRequested.value(QStringLiteral("nativeExecutionStateRef")).toString();
        QVERIFY(!executionStatePath.isEmpty());
        const QString inputSnapshotPath = queuedJob.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString inputSnapshotSha = queuedJob.value(QStringLiteral("inputSnapshotSha256")).toString();
        const QString inputPackagePath = queuedJob.value(QStringLiteral("inputPackageRef")).toString();
        const QString inputPackageSha = queuedJob.value(QStringLiteral("inputPackageSha256")).toString();
        QVERIFY(QFileInfo(inputSnapshotPath).isFile());
        QVERIFY(QFileInfo(inputPackagePath).isDir());
        QCOMPARE(sha256File(inputSnapshotPath), inputSnapshotSha);
        QCOMPARE(sha256Directory(inputPackagePath), inputPackageSha);
        gate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(readBoundedJsonObject(executionStatePath)
                                      .value(QStringLiteral("state")).toString(),
                                  QStringLiteral("paused"), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("processStarted")).toBool(), 15000);
        QCOMPARE(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        const QJsonObject pausedState = readBoundedJsonObject(executionStatePath);
        QCOMPARE(pausedState.value(QStringLiteral("acceptedSeq")).toInt(), 10);
        QVERIFY(!manager.jobQuery(jobId).value(QStringLiteral("processExited")).toBool());

        HistoryFixtureNetwork network;
        network.enqueue(toolCallScript(QStringLiteral("interactive-write"),
            QStringLiteral("document_propose"), proposalArguments,
            QStringLiteral("I prepared an independent document update.")));
        network.enqueue(completionScript(QStringLiteral("The independent read-only question completed.")));
        network.enqueue(completionScript(QStringLiteral("The independent write was accepted.")));
        WorkflowProcess workflow;
        AgentWidget primary(host, &workflow, nullptr, &network,
                            AgentTaskRunner::AccessPolicy::ReadWrite, false);
        primary.setProviderSettings(testProviderSettings(QStringLiteral("native-overlap-key")));
        primary.resize(820, 920);
        primary.show();
        QSignalSpy actions(primary.taskRunner(), &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(primary.taskRunner(), &AgentTaskRunner::finished);
        primary.startTask(QStringLiteral("Prepare a write on the other document while the native source runs."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(primary.findChild<QFrame *>(QStringLiteral("agentApprovalCard"))->isVisible(), 5000);
        const QJsonObject pendingWrite = primary.taskRunner()->pendingProposal();
        auto *composer = primary.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        QVERIFY(composer);
        composer->setPlainText(QStringLiteral("Draft remains independent while the native source is paused."));

        primary.startReadOnlyQuestion(QStringLiteral("Read the other document while the native source is paused."));
        AgentWidget *readOnly = primary.readOnlyWidget();
        QVERIFY(readOnly);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 5000);
        QSignalSpy readOnlyFinished(readOnly->taskRunner(), &AgentTaskRunner::finished);
        QTRY_COMPARE_WITH_TIMEOUT(readOnlyFinished.size(), 1, 10000);
        QCOMPARE(readOnlyFinished.first().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(primary.taskRunner()->pendingProposal(), pendingWrite);
        QCOMPARE(composer->toPlainText(), QStringLiteral("Draft remains independent while the native source is paused."));
        QVERIFY(primary.conversationId() != readOnly->conversationId());
        QVERIFY(saveEvidence(&primary, QStringLiteral("paused-native-primary-pending.png")));
        QVERIFY(saveEvidence(readOnly, QStringLiteral("paused-native-readonly.png")));

        primary.findChild<QPushButton *>(QStringLiteral("agentApproveProposal"))->click();
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(actions.size(), 1);
        QVERIFY(session->modelCommandStamp(interactiveModelId).second > interactiveSourceStamp.second);
        const QJsonObject interactiveAcceptedState = host->query(interactiveModelId);
        Q_UNUSED(interactiveAcceptedState);
        QCOMPARE(session->getModel(interactiveModelId).getProblemSetup().getModalSetup().getNIterations(), 27u);
        QCOMPARE(session->modelCommandStamp(nativeModelId), nativeSourceStamp);
        QCOMPARE(host->query(nativeModelId), nativeAcceptedState);
        QCOMPARE(sha256File(nativeSourcePath), nativeSourceSha);
        QCOMPARE(sha256File(inputSnapshotPath), inputSnapshotSha);
        QCOMPARE(sha256Directory(inputPackagePath), inputPackageSha);
        const QJsonObject stillPaused = readBoundedJsonObject(executionStatePath);
        QCOMPARE(stillPaused.value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
        QCOMPARE(stillPaused.value(QStringLiteral("acceptedSeq")).toInt(), 10);
        QCOMPARE(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        QVERIFY(!manager.jobQuery(jobId).value(QStringLiteral("processExited")).toBool());

        QVERIFY2(manager.cancelJob(jobId, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString()
            == QStringLiteral("cancelled"), 30000);
        reaper.disarm();
        QVERIFY(waitForAgentNativeManagerIdle(manager));
#endif
    }
};

int runAgentConversationHistoryWidgetTests(int argc, char **argv)
{
    TestAgentConversationHistoryWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_conversation_history_widget.moc"
