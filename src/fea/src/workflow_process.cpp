#include "workflow_process.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

namespace
{
constexpr qsizetype maxResponse = 16 * 1024 * 1024;
QString firstFile(const QStringList &candidates)
{
    for (const auto &path : candidates)
        if (QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath();
    return {};
}
QString firstDirectory(const QStringList &candidates)
{
    for (const auto &path : candidates)
        if (QFileInfo(path).isDir()) return QFileInfo(path).absoluteFilePath();
    return {};
}
}

WorkflowProcess::WorkflowProcess(QObject *parent) : QObject(parent)
{
    clock.start();
    process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process, &QProcess::stateChanged, this, [this](QProcess::ProcessState) { emit processStateChanged(); });
    connect(&process, &QProcess::started, this, [this] {
        handshakeId = request("capabilities.query");
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, &WorkflowProcess::consumeOutput);
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        const auto data = process.readAllStandardError();
        diagnostics.append(data.left(qMax<qsizetype>(0, 65536 - diagnostics.size())));
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!stopping) failTransport("SERVICE_ERROR", tr("Workflow service could not run: %1").arg(process.errorString()));
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        consumeOutput();
        setReady(false);
        if (!stopping)
            failTransport("SERVICE_EXITED", tr("Workflow service exited (%1). Check the selected Python runtime and installed calculation packages.").arg(code));
        deadlineTimer.stop();
    });
    deadlineTimer.setInterval(100);
    connect(&deadlineTimer, &QTimer::timeout, this, [this] {
        for (const auto &item : std::as_const(pending))
            if (clock.elapsed() >= item.deadline)
            {
                failTransport("REQUEST_TIMEOUT", tr("The workflow request timed out. Reopen its workspace to reconcile unfinished work before retrying."));
                return;
            }
    });
}

WorkflowProcess::~WorkflowProcess()
{
    stopping = true;
    process.disconnect(this);
    process.closeWriteChannel();
    if (process.state() != QProcess::NotRunning && !process.waitForFinished(1500))
    {
        process.kill();
        process.waitForFinished(1500);
    }
}

WorkflowProcess::Configuration WorkflowProcess::defaultConfiguration(const QString &workspace)
{
    Configuration result;
    const QDir bin(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString suffix = ".exe";
#else
    const QString suffix;
#endif
    QStringList pythonCandidates{bin.filePath("../runtime/python/python" + suffix),
                                 qEnvironmentVariable("RANGE_WORKFLOW_PYTHON")};
#ifdef RANGE_WORKFLOW_DEFAULT_PYTHON
    pythonCandidates.append(QString::fromUtf8(RANGE_WORKFLOW_DEFAULT_PYTHON));
#endif
    pythonCandidates.append(QStandardPaths::findExecutable("python" + suffix));
    result.python = firstFile(pythonCandidates);
    result.solver = firstFile({bin.filePath("fea-solver" + suffix)});
    result.native = firstFile({bin.filePath("range-workflow-native" + suffix)});
    QStringList packages{bin.filePath("../share/range-workflow")};
    QStringList examples{bin.filePath("../examples/control/stage9")};
#ifdef RANGE_WORKFLOW_SOURCE_ROOT
    const QDir source(QString::fromUtf8(RANGE_WORKFLOW_SOURCE_ROOT));
    packages.append(source.filePath("range-workflow/python"));
    examples.append(source.filePath("fea-solver/examples/stage9"));
#endif
    result.packageDirectory = firstDirectory(packages);
    result.examples = firstDirectory(examples);
    result.workspace = QFileInfo(workspace).absoluteFilePath();
    return result;
}

void WorkflowProcess::start(const Configuration &configuration)
{
    if (process.state() != QProcess::NotRunning)
    {
        emit serviceError(tr("Close the current workflow before opening another workspace."));
        return;
    }
    config = configuration;
    if (!QFileInfo(config.python).isFile() || !QFileInfo(config.solver).isFile()
        || !QFileInfo(config.native).isFile() || !QFileInfo(config.examples).isDir()
        || !QFileInfo(config.packageDirectory + "/range_workflow/__main__.py").isFile()
        || !QDir::isAbsolutePath(config.workspace) || config.requestTimeoutMs < 100)
    {
        emit serviceError(tr("The workflow runtime is incomplete. Select an installed Python runtime, solver, and example package."));
        return;
    }
    ++generation;
    stopping = false;
    output.clear(); diagnostics.clear(); pending.clear(); handshakeId.clear();
    setReady(false);
    QProcessEnvironment environment;
    const auto inherited = QProcessEnvironment::systemEnvironment();
    for (const auto &name : {"PATH", "SYSTEMROOT", "WINDIR", "TEMP", "TMP", "COMSPEC", "PATHEXT",
                             "NUMBER_OF_PROCESSORS", "OMP_NUM_THREADS", "QT_PLUGIN_PATH"})
        if (inherited.contains(QString::fromLatin1(name)))
            environment.insert(QString::fromLatin1(name), inherited.value(QString::fromLatin1(name)));
    environment.insert("PYTHONPATH", config.packageDirectory);
    environment.insert("PYTHONNOUSERSITE", "1");
    environment.insert("PYTHONUNBUFFERED", "1");
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(config.packageDirectory);
    process.setProgram(config.python);
    QStringList arguments{"-m", "range_workflow", "--stdio", "--workspace", config.workspace,
                          "--solver", config.solver, "--native", config.native,
                          "--examples", config.examples, "--python", config.python};
    if (config.trustedExtensions) arguments.append("--trusted-extensions");
    process.setArguments(arguments);
    process.start();
    deadlineTimer.start();
}

void WorkflowProcess::setReady(bool value)
{
    if (ready == value) return;
    ready = value;
    emit readyChanged(value);
}

bool WorkflowProcess::isReady() const { return ready; }

QString WorkflowProcess::request(const QString &operation, const QJsonObject &parameters)
{
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto bytes = QJsonDocument(QJsonObject{{"apiVersion", "range.workflow/v1"},
        {"requestId", id}, {"operation", operation}, {"parameters", parameters}}).toJson(QJsonDocument::Compact) + '\n';
    if (stopping || process.state() != QProcess::Running || pending.size() >= 32 || bytes.size() > 256 * 1024)
    {
        QTimer::singleShot(0, this, [this, id, operation] {
            emit requestFailed(id, operation, "SERVICE_UNAVAILABLE", tr("Workflow service is unavailable or its request limit was reached."));
        });
        return id;
    }
    pending.insert(id, {operation, clock.elapsed() + config.requestTimeoutMs});
    const auto requestGeneration = generation;
    if (process.write(bytes) != bytes.size())
        QTimer::singleShot(0, this, [this, id, requestGeneration] {
            if (generation != requestGeneration || !pending.contains(id)) return;
            failTransport("WRITE_FAILED", tr("The workflow request could not be submitted."));
        });
    return id;
}

void WorkflowProcess::consumeOutput()
{
    if (stopping) { process.readAllStandardOutput(); return; }
    output.append(process.readAllStandardOutput());
    if (output.size() > maxResponse)
    {
        failTransport("INVALID_RESPONSE", tr("Workflow output exceeded its size limit."));
        return;
    }
    while (output.contains('\n'))
    {
        const qsizetype end = output.indexOf('\n');
        const QByteArray line = output.left(end);
        output.remove(0, end + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        const auto response = document.object();
        const QString id = response.value("requestId").toString();
        const QString status = response.value("status").toString();
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || response.size() != 5 || response.value("apiVersion") != "range.workflow/v1"
            || !response.value("code").isString() || !response.value("result").isObject()
            || (status != "ok" && status != "error") || !pending.contains(id))
        {
            failTransport("INVALID_RESPONSE", tr("Workflow service returned an invalid or uncorrelated response."));
            return;
        }
        const auto item = pending.take(id);
        const auto result = response.value("result").toObject();
        if (status == "error")
        {
            emit requestFailed(id, item.operation, response.value("code").toString(), result.value("message").toString());
            if (id == handshakeId) failTransport("HANDSHAKE_FAILED", tr("Workflow protocol negotiation failed."));
            continue;
        }
        if (id == handshakeId)
        {
            if (result.value("apiVersion") != "range.workflow/v1")
            {
                failTransport("UNSUPPORTED_VERSION", tr("Workflow service uses an incompatible protocol."));
                return;
            }
            setReady(true);
        }
        emit replyReceived(id, item.operation, result);
    }
}

void WorkflowProcess::failTransport(const QString &code, const QString &message)
{
    const auto failed = pending;
    pending.clear();
    stopping = true;
    setReady(false);
    for (auto it = failed.cbegin(); it != failed.cend(); ++it)
        emit requestFailed(it.key(), it->operation, code, message);
    emit serviceError(message);
    if (process.state() != QProcess::NotRunning) process.kill();
}

void WorkflowProcess::stop()
{
    stopping = true;
    setReady(false);
    const auto failed = pending;
    pending.clear();
    for (auto it = failed.cbegin(); it != failed.cend(); ++it)
        emit requestFailed(it.key(), it->operation, "SERVICE_CLOSED", tr("Workflow connection was closed; check the workspace state before retrying."));
    process.closeWriteChannel();
    const auto closingGeneration = generation;
    QTimer::singleShot(2000, this, [this, closingGeneration] {
        if (generation == closingGeneration && stopping && process.state() != QProcess::NotRunning) process.kill();
    });
}
