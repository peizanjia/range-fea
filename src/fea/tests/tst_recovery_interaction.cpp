#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSemaphore>
#include <QStandardPaths>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTreeWidget>
#include <QUuid>
#include <QVector>

#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <rbl_error.h>
#include <rbl_job.h>
#include <rml_time_solver.h>

#include "application.h"
#include "main_window.h"
#include "model_command_host.h"
#include "model_io.h"
#include "problem_tree.h"
#include "session.h"
#include "solver_manager.h"
#include "value_line_edit.h"
#include "workflow_widget.h"

namespace
{
constexpr int producerAbruptExitCode = 86;
constexpr int producerFailureExitCode = 87;
constexpr int solverAbruptExitCode = 88;
constexpr auto producerProofSchema = "range.recovery-interaction-producer/v1";
constexpr auto contenderProofSchema = "range.recovery-interaction-contender/v1";
constexpr auto contenderBarrierSchema = "range.recovery-interaction-barrier/v1";

#ifndef RANGE_RECOVERY_INTERACTION_SOLVER
#define RANGE_RECOVERY_INTERACTION_SOLVER ""
#endif

bool pathIsWithin(const QString &path, const QString &root);
bool writeJsonAtomically(const QString &path, const QJsonObject &value);

int producerFailure(const QString &reason)
{
    qCritical().noquote() << "WF05 producer failed:" << reason;
    return producerFailureExitCode;
}

bool writeProducerFailureFacts(const QString &testRoot, const QString &phase, QJsonObject facts)
{
    facts.insert(QStringLiteral("schema"), QStringLiteral("range.recovery-interaction-failure/v1"));
    facts.insert(QStringLiteral("phase"), phase.left(64));
    const QByteArray compact = QJsonDocument(facts).toJson(QJsonDocument::Compact);
    if (compact.isEmpty() || compact.size() > 8192) return false;
    const QString path = QDir(testRoot).filePath(QStringLiteral("producer-failure.json"));
    return writeJsonAtomically(path, facts) && pathIsWithin(path, testRoot);
}

bool keepRecoveryTestRoot()
{
    return qEnvironmentVariableIntValue("RANGE_RECOVERY_INTERACTION_KEEP_TEST_ROOT") > 0;
}

QString recoveryApplicationName(const QString &testRoot)
{
    return QStringLiteral("RangeFEA-WF05-") + QFileInfo(QDir::cleanPath(testRoot)).fileName();
}

QString normalizedPath(QString path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

bool pathEquals(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QString::compare(normalizedPath(left), normalizedPath(right), Qt::CaseInsensitive) == 0;
#else
    return normalizedPath(left) == normalizedPath(right);
#endif
}

bool pathIsWithin(const QString &path, const QString &root)
{
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalPath.isEmpty() || canonicalRoot.isEmpty()) return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString candidate = QDir::fromNativeSeparators(QDir::cleanPath(canonicalPath));
    QString base = QDir::fromNativeSeparators(QDir::cleanPath(canonicalRoot));
    if (QString::compare(candidate, base, sensitivity) == 0) return true;
    if (!base.endsWith(QLatin1Char('/'))) base.append(QLatin1Char('/'));
    return candidate.startsWith(base, sensitivity);
}

bool containsPathComponent(const QString &path, const QString &component)
{
    const QStringList components = QDir::fromNativeSeparators(path)
        .split(QLatin1Char('/'), Qt::SkipEmptyParts);
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    for (const QString &part : components)
        if (QString::compare(part, component, sensitivity) == 0) return true;
    return false;
}

QString applicationDataRoot(QString *error = nullptr)
{
    if (error) error->clear();
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (path.isEmpty() || !QDir().mkpath(path))
    {
        if (error) *error = QStringLiteral("Qt test-mode application data is unavailable.");
        return {};
    }
    const QString expectedName = recoveryApplicationName(
        qEnvironmentVariable("RANGE_RECOVERY_INTERACTION_ROOT"));
    // The test-mode namespace is the containment boundary on every platform.
    // The explicit application-name leaf keeps this run separate from other Qt tests.
    if (!QStandardPaths::isTestModeEnabled() || !containsPathComponent(path, QStringLiteral("qttest")) ||
        !containsPathComponent(path, expectedName))
    {
        if (error) *error = QStringLiteral("The recovery fixture is outside its unique Qt test-mode application-data namespace.");
        return {};
    }
    return QFileInfo(path).canonicalFilePath();
}

QJsonObject readJsonObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject{};
}

QJsonObject readBoundedJsonObject(const QString &path, qint64 maximumBytes = 16384)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() < 1 || info.size() > maximumBytes) return {};
    return readJsonObject(path);
}

bool waitForFile(const QString &path, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        const QFileInfo info(path);
        if (info.isFile() && !info.isSymLink()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QTest::qWait(5);
    }
    return false;
}

QString sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) return {};
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString sha256Bytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool configuredSolverExecutable(QString *path, QString *sha256, QString *error)
{
    if (path) path->clear();
    if (sha256) sha256->clear();
    if (error) error->clear();
    if (!path || !sha256 || !error) return false;
    const QString buildTarget = QStringLiteral(RANGE_RECOVERY_INTERACTION_SOLVER);
    const QString configured = Application::instance()->getApplicationSettings()->getSolverPath();
    const QString targetCanonical = QFileInfo(buildTarget).canonicalFilePath();
    const QString configuredCanonical = QFileInfo(configured).canonicalFilePath();
    if (buildTarget.isEmpty() || targetCanonical.isEmpty() || configuredCanonical.isEmpty() ||
        !QFileInfo(targetCanonical).isFile() || !pathEquals(targetCanonical, configuredCanonical))
    {
        *error = QStringLiteral("The configured native solver does not match this build's fea-solver target.");
        return false;
    }
    const QString digest = sha256File(targetCanonical);
    if (digest.size() != 64)
    {
        *error = QStringLiteral("The configured fea-solver executable could not be hashed.");
        return false;
    }
    *path = targetCanonical;
    *sha256 = digest;
    return true;
}

bool terminateOwnedSolverChild(qint64 ownerPid, const QString &expectedExecutable,
                               const QString &expectedSha256, quint64 *terminatedPid,
                               QString *error)
{
    if (terminatedPid) *terminatedPid = 0;
    if (error) error->clear();
    if (!terminatedPid || !error || ownerPid <= 0 || expectedExecutable.isEmpty() ||
        expectedSha256.size() != 64)
    {
        if (error) *error = QStringLiteral("The direct-child termination proof is incomplete.");
        return false;
    }
#ifdef Q_OS_WIN
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        *error = QStringLiteral("Windows could not enumerate the producer's direct child processes.");
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD candidatePid = 0;
    uint directChildCount = 0;
    uint matchingSolverCount = 0;
    bool enumerationOk = Process32FirstW(snapshot, &entry) != FALSE;
    while (enumerationOk)
    {
        if (entry.th32ParentProcessID == DWORD(ownerPid))
        {
            if (++directChildCount > 32)
            {
                CloseHandle(snapshot);
                *error = QStringLiteral("The producer has more than 32 direct children; no process was terminated.");
                return false;
            }
            HANDLE child = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!child)
            {
                CloseHandle(snapshot);
                *error = QStringLiteral("A producer direct child could not be opened for image-path verification.");
                return false;
            }
            wchar_t imagePath[32768]{};
            DWORD imagePathLength = DWORD(sizeof(imagePath) / sizeof(imagePath[0]));
            const BOOL imageRead = QueryFullProcessImageNameW(child, 0, imagePath, &imagePathLength);
            CloseHandle(child);
            if (!imageRead)
            {
                CloseHandle(snapshot);
                *error = QStringLiteral("A producer direct child's full image path could not be verified.");
                return false;
            }
            const QString image = QFileInfo(QString::fromWCharArray(imagePath, int(imagePathLength))).canonicalFilePath();
            if (pathEquals(image, expectedExecutable))
            {
                ++matchingSolverCount;
                candidatePid = entry.th32ProcessID;
            }
        }
        enumerationOk = Process32NextW(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    if (matchingSolverCount != 1 || candidatePid == 0)
    {
        *error = QStringLiteral("Exactly one direct child matched the configured fea-solver image.");
        return false;
    }
    HANDLE solver = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                FALSE, candidatePid);
    if (!solver)
    {
        *error = QStringLiteral("The verified fea-solver direct child exited before the bounded termination step.");
        return false;
    }
    wchar_t imagePath[32768]{};
    DWORD imagePathLength = DWORD(sizeof(imagePath) / sizeof(imagePath[0]));
    const BOOL imageRead = QueryFullProcessImageNameW(solver, 0, imagePath, &imagePathLength);
    const QString image = imageRead
        ? QFileInfo(QString::fromWCharArray(imagePath, int(imagePathLength))).canonicalFilePath() : QString();
    if (!imageRead || !pathEquals(image, expectedExecutable) || sha256File(image) != expectedSha256)
    {
        CloseHandle(solver);
        *error = QStringLiteral("The selected direct child no longer matches the configured fea-solver path and SHA-256.");
        return false;
    }
    HANDLE parentSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    bool stillOwned = false;
    if (parentSnapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W parentEntry{};
        parentEntry.dwSize = sizeof(parentEntry);
        bool parentOk = Process32FirstW(parentSnapshot, &parentEntry) != FALSE;
        while (parentOk)
        {
            if (parentEntry.th32ProcessID == candidatePid && parentEntry.th32ParentProcessID == DWORD(ownerPid))
            {
                stillOwned = true;
                break;
            }
            parentOk = Process32NextW(parentSnapshot, &parentEntry) != FALSE;
        }
        CloseHandle(parentSnapshot);
    }
    if (!stillOwned || !TerminateProcess(solver, solverAbruptExitCode) ||
        WaitForSingleObject(solver, 5000) != WAIT_OBJECT_0)
    {
        CloseHandle(solver);
        *error = QStringLiteral("The verified fea-solver direct child did not terminate within five seconds.");
        return false;
    }
    DWORD exitCode = 0;
    const bool fixedExitCode = GetExitCodeProcess(solver, &exitCode) && exitCode == DWORD(solverAbruptExitCode);
    CloseHandle(solver);
    if (!fixedExitCode)
    {
        *error = QStringLiteral("The fea-solver direct child did not report the fixed test termination code.");
        return false;
    }
    *terminatedPid = candidatePid;
    return true;
#else
    Q_UNUSED(ownerPid);
    Q_UNUSED(expectedExecutable);
    Q_UNUSED(expectedSha256);
    *error = QStringLiteral("The deliberate owner-plus-solver crash fixture requires bounded Windows direct-child inspection.");
    return false;
#endif
}

bool writeJsonAtomically(const QString &path, const QJsonObject &value)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

Model modalFixture(const QString &name)
{
    Model model;
    model.setName(name);
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

bool loadRecoveryModel(const QString &testRoot, Model *model, QString *sourcePath, QString *error)
{
#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_WORKFLOW_SOURCE_ROOT "."
#endif
    if (!model || !sourcePath || !error) return false;
    const QString fixture = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT))
        .filePath(QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram"));
    const QString sourceDirectory = QDir(testRoot).filePath(QStringLiteral("producer-source"));
    if (!QDir().mkpath(sourceDirectory) || !pathIsWithin(sourceDirectory, testRoot))
    {
        *error = QStringLiteral("The recovery source directory escaped its temporary root.");
        return false;
    }
    *sourcePath = QDir(sourceDirectory).filePath(QStringLiteral("uncontrolled-newmark.ram"));
    if (!QFile::copy(fixture, *sourcePath))
    {
        *error = QStringLiteral("The bounded native recovery source fixture could not be copied.");
        return false;
    }
    try
    {
        ModelIO::open(*model, *sourcePath);
    }
    catch (const RError &exception)
    {
        *error = exception.getMessage();
        return false;
    }

    model->getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    model->getProblemSetup().getStructuralDynamicsSetup().setEnabled(false);
    model->getProblemSetup().setRestart(false);
    model->getTimeSolver().setEnabled(false);
    if (model->getNLines() == 0)
    {
        *error = QStringLiteral("The copied native recovery fixture contains no structural lines.");
        return false;
    }
    for (uint index = 0; index < model->getNPoints(); ++index)
        model->getPoint(index).setMaterial(model->getLine(0).getMaterial());

    constexpr uint targetSteps = 20;
    constexpr uint outputFrequency = 1;
    constexpr double timeStep = 0.001;
    auto &dynamics = model->getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
    dynamics.setTimeStep(timeStep);
    dynamics.setNSteps(targetSteps);
    dynamics.setOutputFrequency(outputFrequency);
    auto &time = model->getTimeSolver();
    time.setEnabled(true);
    time.setInputStartTime(0.0);
    time.setInputNTimeSteps(targetSteps);
    time.setInputTimeStepSize(timeStep);
    time.setOutputFrequency(outputFrequency);
    time.setComputedTime(0.0);
    time.setTimes(RTimeSolver::findTimesVector(targetSteps, 0.0, timeStep));
    time.setCurrentTimeStep(0);
    ModelIO::save(*model, *sourcePath, false);
    return model->getNNodes() > 0 && model->getNElements() > 0;
}

bool readAcceptedTenCheckpoint(const QJsonObject &job, QJsonObject *proof, QString *error)
{
    if (error) error->clear();
    if (!proof || !error) return false;
    const QString directory = job.value(QStringLiteral("nativeCheckpointDirectory")).toString();
    if (directory.isEmpty() || !QFileInfo(directory).isDir() || QFileInfo(directory).isSymLink())
    {
        *error = QStringLiteral("The native task has not published its managed checkpoint directory.");
        return false;
    }
    const QJsonObject pointer = readJsonObject(QDir(directory).filePath(QStringLiteral("latest.json")));
    const QString envelopeName = pointer.value(QStringLiteral("envelopeFile")).toString();
    if (pointer.value(QStringLiteral("schema")).toString() != QStringLiteral("range.native-checkpoint-pointer/v1") ||
        envelopeName.isEmpty() || QFileInfo(envelopeName).fileName() != envelopeName)
        return false;
    const QString envelopePath = QDir(directory).filePath(envelopeName);
    const QString envelopeSha = pointer.value(QStringLiteral("envelopeSha256")).toString();
    if (sha256File(envelopePath) != envelopeSha) return false;
    const QJsonObject envelope = readJsonObject(envelopePath);
    if (envelope.value(QStringLiteral("schema")).toString() != QStringLiteral("range.native-checkpoint/v1") ||
        envelope.value(QStringLiteral("acceptedSeq")).toInt(-1) != 10 ||
        envelope.value(QStringLiteral("maxAcceptedSteps")).toInt(-1) != 10)
        return false;
    const QString modelName = envelope.value(QStringLiteral("modelFile")).toString();
    if (modelName.isEmpty() || QFileInfo(modelName).fileName() != modelName) return false;
    const QString checkpointModelPath = QDir(directory).filePath(modelName);
    const QString checkpointModelSha = envelope.value(QStringLiteral("checkpointModelSha256")).toString();
    if (sha256File(checkpointModelPath) != checkpointModelSha) return false;

    *proof = {{QStringLiteral("schema"), QString::fromLatin1(producerProofSchema)},
              {QStringLiteral("jobId"), job.value(QStringLiteral("jobId"))},
              {QStringLiteral("acceptedSeq"), 10}, {QStringLiteral("targetSteps"), 20},
              {QStringLiteral("checkpointDirectory"), directory},
              {QStringLiteral("checkpointModelRef"), checkpointModelPath},
              {QStringLiteral("checkpointModelSha256"), checkpointModelSha},
              {QStringLiteral("checkpointTimeS"), envelope.value(QStringLiteral("timeS"))},
              {QStringLiteral("envelopeRef"), envelopePath},
              {QStringLiteral("envelopeSha256"), envelopeSha},
              {QStringLiteral("applicationName"), QCoreApplication::applicationName()},
              {QStringLiteral("applicationDataRoot"), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)}};
    return true;
}

bool verifyTenFrameJournal(const QJsonObject &job, const QString &jobId, const QString &testRoot,
                           QJsonObject *proof, QString *error)
{
    if (error) error->clear();
    if (!proof || !error) return false;
    const QString journalPath = job.value(QStringLiteral("logArtifactRefs")).toObject()
        .value(QStringLiteral("nativeResultJournal")).toString();
    const QString resultPath = job.value(QStringLiteral("resultArtifactRef")).toString();
    const QFileInfo journalInfo(journalPath);
    if (journalPath.isEmpty() || !journalInfo.isAbsolute() || journalInfo.isSymLink() ||
        !journalInfo.isFile() || journalInfo.size() < 1 || journalInfo.size() > 16 * 1024 * 1024 ||
        !pathIsWithin(journalPath, testRoot) || resultPath.isEmpty() ||
        !QFileInfo(resultPath).isAbsolute())
    {
        *error = QStringLiteral("The accepted-step result journal is missing or outside the temporary run root.");
        return false;
    }
    QFile journal(journalPath);
    if (!journal.open(QIODevice::ReadOnly))
    {
        *error = QStringLiteral("The accepted-step result journal could not be opened.");
        return false;
    }
    const QByteArray bytes = journal.readAll();
    if (journal.error() != QFileDevice::NoError || bytes.isEmpty() || !bytes.endsWith('\n'))
    {
        *error = QStringLiteral("The accepted-step result journal is incomplete or unreadable.");
        return false;
    }
    const QString resultDirectory = QFileInfo(resultPath).absolutePath();
    const QList<QByteArray> lines = bytes.split('\n');
    int count = 0;
    for (qsizetype index = 0; index + 1 < lines.size(); ++index)
    {
        const QByteArray &line = lines.at(index);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        const QJsonObject frame = document.object();
        const double sequence = frame.value(QStringLiteral("seq")).toDouble(-1.0);
        const double recordNo = frame.value(QStringLiteral("recordNo")).toDouble(-1.0);
        const QString relativePath = frame.value(QStringLiteral("relativePath")).toString();
        const QString framePath = QDir(resultDirectory).filePath(relativePath);
        const QString frameSha = frame.value(QStringLiteral("sha256")).toString();
        if (line.isEmpty() || line.size() > 16384 || parseError.error != QJsonParseError::NoError ||
            !document.isObject() || frame.value(QStringLiteral("protocolVersion")).toString() !=
                QStringLiteral("range.native-result-frame/v1") ||
            frame.value(QStringLiteral("jobId")).toString() != jobId ||
            sequence != double(count + 1) || recordNo != double(count + 1) ||
            !frame.value(QStringLiteral("timeS")).isDouble() ||
            !std::isfinite(frame.value(QStringLiteral("timeS")).toDouble()) ||
            relativePath.isEmpty() || QFileInfo(relativePath).fileName() != relativePath ||
            QFileInfo(framePath).isSymLink() || frameSha.size() != 64 ||
            !pathIsWithin(framePath, testRoot) || sha256File(framePath) != frameSha)
        {
            *error = QStringLiteral("The accepted-step result journal contains an incomplete or unbound frame.");
            return false;
        }
        ++count;
    }
    if (count != 10)
    {
        *error = QStringLiteral("The accepted-boundary ACK did not follow exactly ten complete output frames.");
        return false;
    }
    *proof = {{QStringLiteral("nativeResultJournalRef"), journalPath},
              {QStringLiteral("nativeResultJournalSha256"), sha256File(journalPath)},
              {QStringLiteral("nativeResultJournalFrameCount"), count},
              {QStringLiteral("nativeResultJournalLastRecordNo"), count}};
    return !proof->value(QStringLiteral("nativeResultJournalSha256")).toString().isEmpty();
}

int boundedJournalLineCount(const QJsonObject &job)
{
    const QString path = job.value(QStringLiteral("logArtifactRefs")).toObject()
        .value(QStringLiteral("nativeResultJournal")).toString();
    const QFileInfo info(path);
    if (path.isEmpty() || info.isSymLink() || !info.isFile() || info.size() < 0 ||
        info.size() > 16 * 1024 * 1024)
        return -1;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return -1;
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || !bytes.endsWith('\n')) return -1;
    return int(bytes.count('\n'));
}

bool hasVerifiedPausedExecutionProof(const QJsonObject &proof, const QString &testRoot)
{
    const QString statePath = proof.value(QStringLiteral("nativeExecutionStateRef")).toString();
    const QJsonObject ack = proof.value(QStringLiteral("nativeExecutionStateAck")).toObject();
    if (statePath.isEmpty() || !pathIsWithin(statePath, testRoot) ||
        sha256File(statePath) != proof.value(QStringLiteral("nativeExecutionStateSha256")).toString() ||
        readBoundedJsonObject(statePath) != ack)
        return false;
    QJsonObject journalProof;
    const QJsonObject job{{QStringLiteral("resultArtifactRef"), proof.value(QStringLiteral("resultArtifactRef"))},
        {QStringLiteral("logArtifactRefs"), QJsonObject{{QStringLiteral("nativeResultJournal"),
            proof.value(QStringLiteral("nativeResultJournalRef"))}}}};
    QString journalError;
    if (!verifyTenFrameJournal(job, proof.value(QStringLiteral("jobId")).toString(), testRoot,
                               &journalProof, &journalError))
        return false;
    return journalProof.value(QStringLiteral("nativeResultJournalSha256")).toString() ==
            proof.value(QStringLiteral("nativeResultJournalSha256")).toString() &&
        proof.value(QStringLiteral("nativeResultJournalFrameCount")).toInt(-1) == 10 &&
        proof.value(QStringLiteral("nativeResultJournalLastRecordNo")).toInt(-1) == 10 &&
        ack.value(QStringLiteral("schema")).toString() == QStringLiteral("range.native-execution-state/v1") &&
        ack.value(QStringLiteral("state")).toString() == QStringLiteral("paused") &&
        ack.value(QStringLiteral("jobId")).toString() == proof.value(QStringLiteral("jobId")).toString() &&
        ack.value(QStringLiteral("taskId")).toString() == proof.value(QStringLiteral("jobId")).toString() &&
        ack.value(QStringLiteral("generation")).toInt(-1) == 1 &&
        ack.value(QStringLiteral("pauseAtAcceptedSeq")).toInt(-1) == 10 &&
        ack.value(QStringLiteral("acceptedSeq")).toInt(-1) == 10 &&
        ack.value(QStringLiteral("timeS")).toDouble(-1.0) == ack.value(QStringLiteral("checkpointTimeS")).toDouble(-2.0) &&
        ack.value(QStringLiteral("checkpointAvailable")).toBool() &&
        ack.value(QStringLiteral("checkpointAcceptedSeq")).toInt(-1) == 10 &&
        ack.value(QStringLiteral("checkpointEnvelopeSha256")).toString() ==
            proof.value(QStringLiteral("envelopeSha256")).toString() &&
        ack.value(QStringLiteral("checkpointModelSha256")).toString() ==
            proof.value(QStringLiteral("checkpointModelSha256")).toString();
}

class NativeJobFailureCleanup
{
public:
    NativeJobFailureCleanup(SolverManager *manager, QString jobId,
                            std::function<void()> beforeCancel = {})
        : manager(manager), jobId(std::move(jobId)), beforeCancel(std::move(beforeCancel)) {}
    ~NativeJobFailureCleanup()
    {
        if (!manager || jobId.isEmpty()) return;
        if (beforeCancel) beforeCancel();
        const auto terminal = [](const QString &state) {
            return state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
                   state == QStringLiteral("cancelled");
        };
        if (terminal(manager->jobQuery(jobId).value(QStringLiteral("state")).toString())) return;
        QString ignoredError;
        manager->cancelJob(jobId, &ignoredError);
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 30000)
        {
            if (terminal(manager->jobQuery(jobId).value(QStringLiteral("state")).toString())) return;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(5);
        }
    }
private:
    SolverManager *manager;
    QString jobId;
    std::function<void()> beforeCancel;
};

class GateJob : public RJob
{
public:
    GateJob(const std::shared_ptr<QSemaphore> &started, const std::shared_ptr<QSemaphore> &release)
        : started(started), release(release) {}
protected:
    int perform() override
    {
        started->release();
        return release->tryAcquire(1, 120000) ? 0 : 1;
    }
private:
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
};

class ThreadPoolGate
{
public:
    ThreadPoolGate() : pool(QThreadPool::globalInstance()), previousMaximum(pool->maxThreadCount()),
        started(std::make_shared<QSemaphore>()), release(std::make_shared<QSemaphore>()) {}
    ~ThreadPoolGate()
    {
        release->release();
        pool->waitForDone(30000);
        delete job;
        pool->setMaxThreadCount(previousMaximum);
    }
    bool start()
    {
        if (!pool->waitForDone(15000)) return false;
        pool->setMaxThreadCount(1);
        job = new GateJob(started, release);
        job->setAutoDelete(false);
        pool->start(job);
        return started->tryAcquire(1, 10000);
    }
    void releaseNow() { release->release(); }
private:
    QThreadPool *pool;
    int previousMaximum;
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
    GateJob *job = nullptr;
};

int runProducer(const QString &testRoot)
{
    if (testRoot.isEmpty() || !QDir(testRoot).exists() ||
        !pathEquals(qEnvironmentVariable("RANGE_RECOVERY_INTERACTION_ROOT"), testRoot))
        return producerFailure(QStringLiteral("the bounded run root is missing or mismatched."));
    const QString expectedApplicationName = recoveryApplicationName(testRoot);
    if (QCoreApplication::applicationName() != expectedApplicationName ||
        !QStandardPaths::isTestModeEnabled())
        return producerFailure(QStringLiteral("the per-run Qt test-mode application identity is not active."));
    QString dataError;
    const QString dataRoot = applicationDataRoot(&dataError);
    if (dataRoot.isEmpty()) return producerFailure(dataError);
    QString solverExecutable;
    QString solverExecutableSha256;
    if (!configuredSolverExecutable(&solverExecutable, &solverExecutableSha256, &dataError))
        return producerFailure(dataError);

    QTemporaryDir sourceDirectory(QDir(testRoot).filePath(QStringLiteral("source-XXXXXX")));
    if (!sourceDirectory.isValid() || !pathIsWithin(sourceDirectory.path(), testRoot))
        return producerFailure(QStringLiteral("the temporary source directory is outside the run root."));
    Model model;
    QString sourcePath;
    QString error;
    if (!loadRecoveryModel(sourceDirectory.path(), &model, &sourcePath, &error))
        return producerFailure(error.isEmpty() ? QStringLiteral("the bounded native model fixture could not be prepared.") : error);
    auto *session = Application::instance()->getSession();
    session->clear();
    session->addModel(model);
    const uint modelId = session->getNModels() - 1;
    SolverManager &manager = SolverManager::getInstance();
    ThreadPoolGate pauseGate;
    if (!pauseGate.start())
        return producerFailure(QStringLiteral("the native worker queue could not be held before submission."));
    const QString jobId = manager.submitValidated(modelId, {}, true, &error);
    if (jobId.isEmpty()) return producerFailure(error.isEmpty() ? QStringLiteral("the public native submit was rejected.") : error);
    // Failure paths cancel and reap only this helper's own test job. The
    // successful crash path calls _Exit and never runs this cleanup.
    NativeJobFailureCleanup failureCleanup(&manager, jobId, [&pauseGate] { pauseGate.releaseNow(); });
    if (!manager.requestNativeAcceptedBoundaryPause(jobId, 10, &error))
        return producerFailure(error.isEmpty()
            ? QStringLiteral("the accepted-step pause request was rejected before solver start.") : error);
    const QJsonObject queuedJob = manager.jobQuery(jobId);
    const QString executionStatePath = queuedJob.value(QStringLiteral("nativeExecutionStateRef")).toString();
    if (queuedJob.value(QStringLiteral("state")).toString() != QStringLiteral("queued") ||
        executionStatePath.isEmpty() || !QFileInfo(executionStatePath).isAbsolute() ||
        !pathIsWithin(QFileInfo(executionStatePath).absolutePath(), testRoot))
        return producerFailure(QStringLiteral("the queued task did not expose a bounded native execution-state path."));
    pauseGate.releaseNow();

    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 120000)
    {
        const QJsonObject job = manager.jobQuery(jobId);
        const QJsonObject executionState = readBoundedJsonObject(executionStatePath);
        if (executionState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"))
        {
            QJsonObject proof;
            if (!readAcceptedTenCheckpoint(job, &proof, &error))
                return producerFailure(QStringLiteral("the native execution-state ACK was not accompanied by a verified accepted-10 checkpoint."));
            QJsonObject journalProof;
            if (!verifyTenFrameJournal(job, jobId, testRoot, &journalProof, &error))
                return producerFailure(error.isEmpty()
                    ? QStringLiteral("the accepted-boundary ACK was not accompanied by ten complete result-journal frames.")
                    : error);
            const QJsonObject checkpointContext = job.value(QStringLiteral("nativeCheckpointContext")).toObject();
            const bool ackMatches = executionState.value(QStringLiteral("schema")).toString() ==
                    QStringLiteral("range.native-execution-state/v1") &&
                executionState.value(QStringLiteral("generation")).toInt(-1) == 1 &&
                executionState.value(QStringLiteral("jobId")).toString() == jobId &&
                executionState.value(QStringLiteral("taskId")).toString() == jobId &&
                executionState.value(QStringLiteral("pauseAtAcceptedSeq")).toInt(-1) == 10 &&
                executionState.value(QStringLiteral("acceptedSeq")).toInt(-1) == 10 &&
                executionState.value(QStringLiteral("timeS")).toDouble(-1.0) ==
                    executionState.value(QStringLiteral("checkpointTimeS")).toDouble(-2.0) &&
                executionState.value(QStringLiteral("checkpointAvailable")).toBool() &&
                executionState.value(QStringLiteral("checkpointAcceptedSeq")).toInt(-1) == 10 &&
                executionState.value(QStringLiteral("checkpointTimeS")).toDouble(-1.0) ==
                    proof.value(QStringLiteral("checkpointTimeS")).toDouble(-2.0) &&
                executionState.value(QStringLiteral("checkpointEnvelopeSha256")).toString() ==
                    proof.value(QStringLiteral("envelopeSha256")).toString() &&
                executionState.value(QStringLiteral("checkpointModelSha256")).toString() ==
                    proof.value(QStringLiteral("checkpointModelSha256")).toString() &&
                executionState.value(QStringLiteral("sourceSha256")).toString() ==
                    checkpointContext.value(QStringLiteral("sourceSha256")).toString() &&
                executionState.value(QStringLiteral("modelSha256")).toString() ==
                    checkpointContext.value(QStringLiteral("modelSha256")).toString() &&
                executionState.value(QStringLiteral("controlSha256")).toString() ==
                    checkpointContext.value(QStringLiteral("controlSha256")).toString() &&
                executionState.value(QStringLiteral("runtimeSha256")).toString() ==
                    checkpointContext.value(QStringLiteral("runtimeSha256")).toString() &&
                executionState.value(QStringLiteral("packageSha256")).toString() ==
                    checkpointContext.value(QStringLiteral("packageSha256")).toString();
            if (!ackMatches)
                return producerFailure(QStringLiteral("the accepted-boundary ACK does not match task, pinned context, and accepted-10 checkpoint."));
            const QString contextSha256 = sha256Bytes(QJsonDocument(checkpointContext).toJson(QJsonDocument::Compact));
            if (executionState.value(QStringLiteral("contextSha256")).toString() != contextSha256)
                return producerFailure(QStringLiteral("the accepted-boundary ACK context SHA-256 does not match the pinned context."));
            const QString acknowledgedStateSha256 = sha256File(executionStatePath);
            QTest::qWait(100); // The gate must remain paused without a RESUME or STOP command.
            const QJsonObject pausedJob = manager.jobQuery(jobId);
            const QJsonObject stateAfterWait = readBoundedJsonObject(executionStatePath);
            const bool stateFileStable = sha256File(executionStatePath) == acknowledgedStateSha256 &&
                stateAfterWait == executionState;
            const QString pausedJobState = pausedJob.value(QStringLiteral("state")).toString();
            const bool processStarted = pausedJob.value(QStringLiteral("processStarted")).toBool();
            const bool processExited = pausedJob.value(QStringLiteral("processExited")).toBool();
            QJsonObject pausedCheckpointProof;
            const bool checkpointValid = readAcceptedTenCheckpoint(pausedJob, &pausedCheckpointProof, &error);
            const bool checkpointUnchanged = checkpointValid &&
                pausedCheckpointProof.value(QStringLiteral("envelopeSha256")) ==
                    proof.value(QStringLiteral("envelopeSha256")) &&
                pausedCheckpointProof.value(QStringLiteral("checkpointModelSha256")) ==
                    proof.value(QStringLiteral("checkpointModelSha256"));
            QJsonObject pausedJournalProof;
            const bool journalValid = verifyTenFrameJournal(pausedJob, jobId, testRoot,
                                                             &pausedJournalProof, &error);
            const bool journalUnchanged = journalValid &&
                pausedJournalProof.value(QStringLiteral("nativeResultJournalSha256")) ==
                    journalProof.value(QStringLiteral("nativeResultJournalSha256"));
            QStringList failedPredicates;
            if (!stateFileStable) failedPredicates.append(QStringLiteral("execution_state_changed"));
            if (pausedJobState != QStringLiteral("running")) failedPredicates.append(QStringLiteral("job_state_not_running"));
            if (!processStarted) failedPredicates.append(QStringLiteral("process_started_not_reported"));
            if (processExited) failedPredicates.append(QStringLiteral("process_reported_exited"));
            if (!checkpointValid) failedPredicates.append(QStringLiteral("latest_checkpoint_invalid"));
            else if (!checkpointUnchanged) failedPredicates.append(QStringLiteral("latest_checkpoint_advanced"));
            if (!journalValid) failedPredicates.append(QStringLiteral("result_journal_invalid"));
            else if (!journalUnchanged) failedPredicates.append(QStringLiteral("result_journal_advanced"));
            if (!failedPredicates.isEmpty())
            {
                const QJsonObject failureFacts{
                    {QStringLiteral("ackState"), executionState.value(QStringLiteral("state"))},
                    {QStringLiteral("ackGeneration"), executionState.value(QStringLiteral("generation"))},
                    {QStringLiteral("ackAcceptedSeq"), executionState.value(QStringLiteral("acceptedSeq"))},
                    {QStringLiteral("ackCheckpointSeq"), executionState.value(QStringLiteral("checkpointAcceptedSeq"))},
                    {QStringLiteral("stateAfterWait"), stateAfterWait.value(QStringLiteral("state"))},
                    {QStringLiteral("generationAfterWait"), stateAfterWait.value(QStringLiteral("generation"))},
                    {QStringLiteral("acceptedSeqAfterWait"), stateAfterWait.value(QStringLiteral("acceptedSeq"))},
                    {QStringLiteral("jobState"), pausedJobState},
                    {QStringLiteral("processStarted"), processStarted},
                    {QStringLiteral("processExited"), processExited},
                    {QStringLiteral("executionStateStable"), stateFileStable},
                    {QStringLiteral("checkpointValid"), checkpointValid},
                    {QStringLiteral("checkpointUnchanged"), checkpointUnchanged},
                    {QStringLiteral("latestCheckpointSeq"), checkpointValid
                        ? pausedCheckpointProof.value(QStringLiteral("acceptedSeq")) : QJsonValue(-1)},
                    {QStringLiteral("journalValid"), journalValid},
                    {QStringLiteral("journalUnchanged"), journalUnchanged},
                    {QStringLiteral("journalLineCount"), boundedJournalLineCount(pausedJob)},
                    {QStringLiteral("journalFrameCount"), journalValid
                        ? pausedJournalProof.value(QStringLiteral("nativeResultJournalFrameCount")) : QJsonValue(-1)},
                    {QStringLiteral("failedPredicates"), QJsonArray::fromStringList(failedPredicates)}};
                writeProducerFailureFacts(testRoot, QStringLiteral("paused-hold"), failureFacts);
                return producerFailure(QStringLiteral("paused-hold failure: %1")
                    .arg(failedPredicates.join(QLatin1Char(','))));
            }

            const QJsonObject liveQualification = manager.nativeJobRecoveryQuery(jobId);
            if (liveQualification.value(QStringLiteral("eligibility")).toString() != QStringLiteral("reconcile_required"))
                return producerFailure(QStringLiteral("the live native owner was not fail-closed before the deliberate crash."));
            quint64 solverPid = 0;
            if (!terminateOwnedSolverChild(QCoreApplication::applicationPid(), solverExecutable,
                                           solverExecutableSha256, &solverPid, &error))
                return producerFailure(error);
            QJsonObject finalProof;
            const QJsonObject postKillJob = manager.jobQuery(jobId);
            if (!readAcceptedTenCheckpoint(postKillJob, &finalProof, &error))
                return producerFailure(QStringLiteral("accepted step 10 was not preserved after terminating the exact solver child."));
            QJsonObject finalJournalProof;
            if (!verifyTenFrameJournal(postKillJob, jobId, testRoot, &finalJournalProof, &error) ||
                finalJournalProof.value(QStringLiteral("nativeResultJournalSha256")) !=
                    journalProof.value(QStringLiteral("nativeResultJournalSha256")))
                return producerFailure(QStringLiteral("the complete accepted-10 result journal changed after terminating the solver child."));
            finalProof.insert(QStringLiteral("resultArtifactRef"), postKillJob.value(QStringLiteral("resultArtifactRef")));
            for (auto field = finalJournalProof.begin(); field != finalJournalProof.end(); ++field)
                finalProof.insert(field.key(), field.value());
            finalProof.insert(QStringLiteral("ownerPid"), double(QCoreApplication::applicationPid()));
            finalProof.insert(QStringLiteral("solverPid"), double(solverPid));
            finalProof.insert(QStringLiteral("solverExecutableRef"), solverExecutable);
            finalProof.insert(QStringLiteral("solverExecutableSha256"), solverExecutableSha256);
            finalProof.insert(QStringLiteral("solverTerminationExitCode"), solverAbruptExitCode);
            finalProof.insert(QStringLiteral("solverProcessTerminated"), true);
            finalProof.insert(QStringLiteral("nativeExecutionStateRef"), executionStatePath);
            finalProof.insert(QStringLiteral("nativeExecutionStateSha256"), sha256File(executionStatePath));
            finalProof.insert(QStringLiteral("nativeExecutionStateAck"), executionState);
            finalProof.insert(QStringLiteral("liveOwnerEligibility"),
                              liveQualification.value(QStringLiteral("eligibility")));
            const QString checkpointDirectory = finalProof.value(QStringLiteral("checkpointDirectory")).toString();
            const QString modelPath = finalProof.value(QStringLiteral("checkpointModelRef")).toString();
            const QString envelopePath = finalProof.value(QStringLiteral("envelopeRef")).toString();
            if (!pathIsWithin(checkpointDirectory, testRoot) ||
                !pathIsWithin(modelPath, checkpointDirectory) ||
                !pathIsWithin(envelopePath, checkpointDirectory))
                return producerFailure(QStringLiteral("published checkpoint paths escaped their managed test roots."));
            const QString proofPath = QDir(testRoot).filePath(QStringLiteral("producer-ready.json"));
            if (!writeJsonAtomically(proofPath, finalProof) || !pathIsWithin(proofPath, testRoot))
                return producerFailure(QStringLiteral("the bounded checkpoint proof could not be published."));
            if (!hasVerifiedPausedExecutionProof(finalProof, testRoot))
                return producerFailure(QStringLiteral("the accepted-boundary pause ACK changed before the crash proof was published."));

            // The fea-solver child was terminated only after its own PID, parent
            // PID, full image path and SHA-256 were verified. This then abandons
            // the GUI owner without running any C++/Qt cleanup path. The solver
            // ACK proves it paused after the complete accepted-10 journal and
            // checkpoint publications, before trial 11.
            std::_Exit(producerAbruptExitCode);
        }
        const QString state = job.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
            state == QStringLiteral("cancelled"))
            return producerFailure(QStringLiteral("the 20-step native job ended before the producer observed accepted step 10."));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QTest::qWait(1);
    }
    return producerFailure(QStringLiteral("the 20-step native job did not publish accepted step 10 before timeout."));
}

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), operation}, {QStringLiteral("parameters"), parameters}};
}

QJsonObject documentTarget(Session *session, const QString &documentId)
{
    return {{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), session->getModelCommandHost()->sessionEpoch()}};
}

QJsonObject modalPreview(Session *session, const QString &documentId, uint iterations)
{
    const uint modelId = session->findModelByCommandIdentity(documentId);
    if (modelId == RConstants::eod) return {};
    const auto stamp = session->modelCommandStamp(modelId);
    return session->getModelCommandHost()->handle(hostRequest(QStringLiteral("command.preview"),
        {{QStringLiteral("target"), documentTarget(session, documentId)},
         {QStringLiteral("expectedDocumentRevision"), double(stamp.second)},
         {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
         {QStringLiteral("commandVersion"), 1},
         {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("method"), QStringLiteral("multiple")},
             {QStringLiteral("iterations"), int(iterations)}, {QStringLiteral("modes"), 2},
             {QStringLiteral("tolerance"), 1e-6}, {QStringLiteral("unit"), QStringLiteral("1")}}}}));
}

QJsonObject executePreview(Session *session, const QString &previewId, const QString &key)
{
    return session->getModelCommandHost()->handle(hostRequest(QStringLiteral("command.execute"),
        {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), key}}));
}

ValueLineEdit *namedProblemEditor(ProblemTree *tree, const QString &name)
{
    if (!tree) return nullptr;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
        if (auto *cell = tree->itemWidget(tree->topLevelItem(index), 0))
            if (auto *editor = cell->findChild<ValueLineEdit *>(name)) return editor;
    return nullptr;
}

QJsonObject recoveryAttemptFor(const QJsonObject &page, const QString &sourceJobId)
{
    for (const QJsonValue &value : page.value(QStringLiteral("jobs")).toArray())
    {
        const QJsonObject job = value.toObject();
        if (job.value(QStringLiteral("recoverySourceJobId")).toString() == sourceJobId)
            return job;
    }
    return {};
}

QProcessEnvironment childEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : environment.keys())
    {
        const QString upper = name.toUpper();
        if (upper.contains(QStringLiteral("KEY")) || upper.contains(QStringLiteral("TOKEN")) ||
            upper.contains(QStringLiteral("SECRET")) || upper.contains(QStringLiteral("PASSWORD")) ||
            upper.contains(QStringLiteral("CREDENTIAL")) || upper.contains(QStringLiteral("AUTH")))
            environment.remove(name);
    }
    return environment;
}

struct EnvironmentRestore
{
    QByteArray appData = qgetenv("APPDATA");
    QByteArray testRoot = qgetenv("RANGE_RECOVERY_INTERACTION_ROOT");
    bool hadAppData = qEnvironmentVariableIsSet("APPDATA");
    bool hadTestRoot = qEnvironmentVariableIsSet("RANGE_RECOVERY_INTERACTION_ROOT");
    ~EnvironmentRestore()
    {
        if (hadAppData) qputenv("APPDATA", appData); else qunsetenv("APPDATA");
        if (hadTestRoot) qputenv("RANGE_RECOVERY_INTERACTION_ROOT", testRoot);
        else qunsetenv("RANGE_RECOVERY_INTERACTION_ROOT");
    }
};

int contenderFailure(const QString &testRoot, int contenderIndex,
                     const QString &stage, const QString &reasonCode,
                     QJsonObject details = {})
{
    QJsonObject facts{{QStringLiteral("schema"), QStringLiteral("range.recovery-interaction-contender-failure/v1")},
        {QStringLiteral("phase"), QStringLiteral("failure")},
        {QStringLiteral("contenderIndex"), contenderIndex},
        {QStringLiteral("ownerPid"), double(QCoreApplication::applicationPid())},
        {QStringLiteral("stage"), stage.left(64)}, {QStringLiteral("reasonCode"), reasonCode.left(64)}};
    for (auto it = details.begin(); it != details.end(); ++it) facts.insert(it.key(), it.value());
    if (facts.contains(QStringLiteral("schema")))
    {
        const QByteArray bytes = QJsonDocument(facts).toJson(QJsonDocument::Compact);
        const QString path = QDir(testRoot).filePath(QStringLiteral("contender-failure-%1.json").arg(contenderIndex));
        if (!testRoot.isEmpty() && QDir(testRoot).exists() && bytes.size() <= 8192 &&
            writeJsonAtomically(path, facts) && pathIsWithin(path, testRoot))
            qCritical().noquote() << "WF05 recovery contender failed; bounded facts:" << QString::fromUtf8(bytes);
        else
            qCritical().noquote() << "WF05 recovery contender failed at" << stage << "code" << reasonCode;
    }
    return 91;
}

QString recoveryRejectionClass(const QString &error)
{
    const QString lower = error.toLower();
    if (lower.contains(QStringLiteral("reconcile_required"))) return QStringLiteral("reconcile_required");
    if (lower.contains(QStringLiteral("lock")) || lower.contains(QStringLiteral("lease")))
        return QStringLiteral("logical_lock_rejected");
    if (lower.contains(QStringLiteral("active")) || lower.contains(QStringLiteral("staging")) ||
        lower.contains(QStringLiteral("successor")))
        return QStringLiteral("active_successor_rejected");
    return QStringLiteral("other_rejection");
}

QString sanitizedQualificationReason(QString reason)
{
    reason = reason.left(4096);
    static const QRegularExpression windowsPath(QStringLiteral(R"([A-Za-z]:[\\/][^\s,;]+)"));
    static const QRegularExpression uncPath(QStringLiteral(R"(\\\\[^\s,;]+)"));
    static const QRegularExpression sha256(QStringLiteral(R"(\b[0-9a-fA-F]{64}\b)"));
    static const QRegularExpression secretAssignment(
        QStringLiteral(R"(\b(token|password|secret|api[_-]?key)\s*[:=]\s*[^\s,;]+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bearerCredential(
        QStringLiteral(R"(\bBearer\s+[^\s,;]+)"), QRegularExpression::CaseInsensitiveOption);
    reason.replace(windowsPath, QStringLiteral("<path>"));
    reason.replace(uncPath, QStringLiteral("<path>"));
    reason.replace(sha256, QStringLiteral("<sha256>"));
    reason.replace(secretAssignment, QStringLiteral("<credential-redacted>"));
    reason.replace(bearerCredential, QStringLiteral("<credential-redacted>"));
    return reason.left(1024);
}

QString qualificationReasonClass(const QString &reason)
{
    const QString lower = reason.toLower();
    if (lower.contains(QStringLiteral("logical recovery lineage"))) return QStringLiteral("logical_lineage");
    if (lower.contains(QStringLiteral("logical recovery lock")) || lower.contains(QStringLiteral("another gui process")))
        return QStringLiteral("logical_lock");
    if (lower.contains(QStringLiteral("checkpoint pointer")) || lower.contains(QStringLiteral("checkpoint envelope")))
        return QStringLiteral("checkpoint_integrity");
    if (lower.contains(QStringLiteral("result manifest")) ||
        lower.contains(QStringLiteral("result artifact")) ||
        lower.contains(QStringLiteral("working result")))
        return QStringLiteral("result_reconciliation");
    if (lower.contains(QStringLiteral("source")) || lower.contains(QStringLiteral("frozen input")))
        return QStringLiteral("source_binding");
    return QStringLiteral("other");
}

QString qualificationReasonCode(const QString &reason)
{
    const QString lower = reason.toLower();
    if (lower.contains(QStringLiteral("logical recovery lineage")) ||
        lower.contains(QStringLiteral("logical recovery identity")))
        return QStringLiteral("logical_lineage_conflict");
    if (lower.contains(QStringLiteral("logical recovery lock")) ||
        lower.contains(QStringLiteral("another gui process")) ||
        lower.contains(QStringLiteral("owner lock is still held")))
        return QStringLiteral("logical_recovery_lock_held");
    if (lower.contains(QStringLiteral("checkpoint pointer")) ||
        lower.contains(QStringLiteral("checkpoint envelope")))
        return QStringLiteral("checkpoint_integrity_failure");
    if (lower.contains(QStringLiteral("result manifest")) ||
        lower.contains(QStringLiteral("result artifact")) ||
        lower.contains(QStringLiteral("working result")))
        return QStringLiteral("result_reconciliation_required");
    if (lower.contains(QStringLiteral("frozen input")) || lower.contains(QStringLiteral("source binding")))
        return QStringLiteral("source_binding_failure");
    if (lower.contains(QStringLiteral("checkpoint")))
        return QStringLiteral("checkpoint_qualification_failure");
    return QStringLiteral("unclassified_qualification_failure");
}

bool cancelAndReapQueuedAttempt(SolverManager &manager, ThreadPoolGate &gate,
                               const QString &jobId, QString *error)
{
    if (error) error->clear();
    QString cancelError;
    if (!manager.cancelJob(jobId, &cancelError))
    {
        if (error) *error = cancelError;
        gate.releaseNow();
        return false;
    }
    gate.releaseNow();
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 30000)
    {
        if (manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("cancelled"))
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(5);
    }
    if (error) *error = QStringLiteral("The contender's queued native attempt did not reach cancelled state.");
    return false;
}

int runContender(const QString &testRoot, int contenderIndex)
{
    const auto fail = [&](const QString &stage, const QString &reasonCode,
                          const QJsonObject &details = QJsonObject{})
    {
        return contenderFailure(testRoot, contenderIndex, stage, reasonCode, details);
    };
    if (contenderIndex < 0 || contenderIndex > 1 || testRoot.isEmpty() || !QDir(testRoot).exists() ||
        !pathEquals(qEnvironmentVariable("RANGE_RECOVERY_INTERACTION_ROOT"), testRoot) ||
        QCoreApplication::applicationName() != recoveryApplicationName(testRoot) ||
        !QStandardPaths::isTestModeEnabled())
        return fail(QStringLiteral("startup"), QStringLiteral("role_or_namespace_invalid"),
                    {{QStringLiteral("rootExists"), !testRoot.isEmpty() && QDir(testRoot).exists()},
                     {QStringLiteral("rootEnvironmentMatches"), pathEquals(
                         qEnvironmentVariable("RANGE_RECOVERY_INTERACTION_ROOT"), testRoot)},
                     {QStringLiteral("applicationNameMatches"), QCoreApplication::applicationName() == recoveryApplicationName(testRoot)},
                     {QStringLiteral("testModeEnabled"), QStandardPaths::isTestModeEnabled()}});

    QString dataError;
    const QString dataRoot = applicationDataRoot(&dataError);
    if (dataRoot.isEmpty())
        return fail(QStringLiteral("application_data"), QStringLiteral("test_namespace_unavailable"));
    const QJsonObject producerProof = readBoundedJsonObject(
        QDir(testRoot).filePath(QStringLiteral("producer-ready.json")));
    const QString originalJobId = producerProof.value(QStringLiteral("jobId")).toString();
    if (producerProof.value(QStringLiteral("schema")).toString() != QString::fromLatin1(producerProofSchema) ||
        producerProof.value(QStringLiteral("acceptedSeq")).toInt(-1) != 10 ||
        producerProof.value(QStringLiteral("targetSteps")).toInt(-1) != 20 ||
        producerProof.value(QStringLiteral("solverProcessTerminated")).toBool() != true ||
        !hasVerifiedPausedExecutionProof(producerProof, testRoot) ||
        !pathEquals(producerProof.value(QStringLiteral("applicationDataRoot")).toString(), dataRoot) ||
        originalJobId.isEmpty())
        return fail(QStringLiteral("producer_proof"), QStringLiteral("pause_proof_missing_or_mismatched"),
            {{QStringLiteral("proofReadable"), !producerProof.isEmpty()},
             {QStringLiteral("schemaMatches"), producerProof.value(QStringLiteral("schema")).toString() == QString::fromLatin1(producerProofSchema)},
             {QStringLiteral("acceptedSeq"), producerProof.value(QStringLiteral("acceptedSeq"))},
             {QStringLiteral("targetSteps"), producerProof.value(QStringLiteral("targetSteps"))},
             {QStringLiteral("solverTerminated"), producerProof.value(QStringLiteral("solverProcessTerminated")).toBool()},
             {QStringLiteral("pausedExecutionProofValid"), hasVerifiedPausedExecutionProof(producerProof, testRoot)},
             {QStringLiteral("applicationNamespaceMatches"), pathEquals(
                 producerProof.value(QStringLiteral("applicationDataRoot")).toString(), dataRoot)},
             {QStringLiteral("jobIdPresent"), !originalJobId.isEmpty()}});

    ThreadPoolGate gate;
    if (!gate.start())
        return fail(QStringLiteral("worker_gate"), QStringLiteral("queue_hold_failed"));
    SolverManager &manager = SolverManager::getInstance();
    const QJsonObject qualification = manager.nativeJobRecoveryQuery(originalJobId);
    const QJsonObject checkpoint = qualification.value(QStringLiteral("checkpoint")).toObject();
    const QString checkpointSha256 = checkpoint.value(QStringLiteral("checkpointSha256")).toString();
    const int remainingSteps = checkpoint.value(QStringLiteral("remainingSteps")).toInt(-1);
    if (qualification.value(QStringLiteral("eligibility")).toString() != QStringLiteral("recoverable") ||
        checkpoint.value(QStringLiteral("acceptedSeq")).toInt(-1) != 10 ||
        checkpoint.value(QStringLiteral("targetSteps")).toInt(-1) != 20 || remainingSteps != 10 ||
        checkpointSha256.isEmpty())
    {
        const QString qualificationReason = qualification.value(QStringLiteral("reason")).toString();
        return fail(QStringLiteral("pre_submit_qualification"), QStringLiteral("not_recoverable"),
            {{QStringLiteral("eligibility"), qualification.value(QStringLiteral("eligibility"))},
             {QStringLiteral("reasonClass"), qualificationReasonClass(qualificationReason)},
             {QStringLiteral("reason"), sanitizedQualificationReason(qualificationReason)},
             {QStringLiteral("qualification"), QJsonObject{
                 {QStringLiteral("eligibility"), qualification.value(QStringLiteral("eligibility"))},
                 {QStringLiteral("reasonCode"), qualificationReasonCode(qualificationReason)},
                 {QStringLiteral("invariantKind"), qualificationReasonClass(qualificationReason)},
                 {QStringLiteral("reason"), sanitizedQualificationReason(qualificationReason)},
                 {QStringLiteral("acceptedSeq"), checkpoint.value(QStringLiteral("acceptedSeq"))},
                 {QStringLiteral("targetSteps"), checkpoint.value(QStringLiteral("targetSteps"))},
                 {QStringLiteral("remainingSteps"), remainingSteps}}},
             {QStringLiteral("acceptedSeq"), checkpoint.value(QStringLiteral("acceptedSeq"))},
             {QStringLiteral("targetSteps"), checkpoint.value(QStringLiteral("targetSteps"))},
             {QStringLiteral("remainingSteps"), remainingSteps},
             {QStringLiteral("checkpointShaPresent"), !checkpointSha256.isEmpty()}});
    }

    const QString qualifiedPath = QDir(testRoot).filePath(
        QStringLiteral("contender-qualified-%1.json").arg(contenderIndex));
    const QJsonObject qualifiedProof{{QStringLiteral("schema"), QString::fromLatin1(contenderProofSchema)},
        {QStringLiteral("phase"), QStringLiteral("qualified")}, {QStringLiteral("contenderIndex"), contenderIndex},
        {QStringLiteral("ownerPid"), double(QCoreApplication::applicationPid())},
        {QStringLiteral("jobId"), originalJobId}, {QStringLiteral("eligibility"), QStringLiteral("recoverable")},
        {QStringLiteral("acceptedSeq"), 10}, {QStringLiteral("targetSteps"), 20},
        {QStringLiteral("remainingSteps"), remainingSteps}, {QStringLiteral("checkpointSha256"), checkpointSha256},
        {QStringLiteral("applicationName"), QCoreApplication::applicationName()},
        {QStringLiteral("applicationDataRoot"), dataRoot}};
    if (!writeJsonAtomically(qualifiedPath, qualifiedProof) || !pathIsWithin(qualifiedPath, testRoot))
        return fail(QStringLiteral("pre_submit_proof"), QStringLiteral("bounded_proof_publish_failed"));

    const QString submitBarrierPath = QDir(testRoot).filePath(QStringLiteral("submit-barrier.json"));
    if (!waitForFile(submitBarrierPath, 90000))
        return fail(QStringLiteral("submit_barrier"), QStringLiteral("parent_release_timeout"));
    const QJsonObject barrier = readBoundedJsonObject(submitBarrierPath);
    if (barrier.value(QStringLiteral("schema")).toString() != QString::fromLatin1(contenderBarrierSchema) ||
        barrier.value(QStringLiteral("jobId")).toString() != originalJobId ||
        barrier.value(QStringLiteral("checkpointSha256")).toString() != checkpointSha256 ||
        barrier.value(QStringLiteral("remainingSteps")).toInt(-1) != remainingSteps)
        return fail(QStringLiteral("submit_barrier"), QStringLiteral("barrier_binding_mismatch"),
            {{QStringLiteral("barrierReadable"), !barrier.isEmpty()},
             {QStringLiteral("schemaMatches"), barrier.value(QStringLiteral("schema")).toString() == QString::fromLatin1(contenderBarrierSchema)},
             {QStringLiteral("jobMatches"), barrier.value(QStringLiteral("jobId")).toString() == originalJobId},
             {QStringLiteral("checkpointMatches"), barrier.value(QStringLiteral("checkpointSha256")).toString() == checkpointSha256},
             {QStringLiteral("remainingSteps"), barrier.value(QStringLiteral("remainingSteps"))}});

    // One call only: the test must expose cross-process exclusion, not retry
    // until one process happens to win.
    QString submitError;
    const QString recoveryJobId = manager.submitNativeRecovery(originalJobId, checkpointSha256,
                                                               uint(remainingSteps), &submitError);
    const bool accepted = !recoveryJobId.isEmpty();
    const QString rejectionClass = accepted ? QStringLiteral("accepted") : recoveryRejectionClass(submitError);
    const QJsonObject attempt = accepted ? manager.jobQuery(recoveryJobId) : QJsonObject{};
    const QJsonObject headQualification = manager.nativeJobRecoveryQuery(originalJobId);
    const QJsonObject submissionProof{{QStringLiteral("schema"), QString::fromLatin1(contenderProofSchema)},
        {QStringLiteral("phase"), QStringLiteral("submitted")}, {QStringLiteral("contenderIndex"), contenderIndex},
        {QStringLiteral("ownerPid"), double(QCoreApplication::applicationPid())},
        {QStringLiteral("jobId"), originalJobId}, {QStringLiteral("accepted"), accepted},
        {QStringLiteral("recoveryJobId"), recoveryJobId},
        {QStringLiteral("recoveryJobState"), attempt.value(QStringLiteral("state"))},
        {QStringLiteral("recoverySourceJobId"), attempt.value(QStringLiteral("recoverySourceJobId"))},
        {QStringLiteral("logicalJobId"), attempt.value(QStringLiteral("logicalJobId"))},
        {QStringLiteral("headEligibility"), headQualification.value(QStringLiteral("eligibility"))},
        {QStringLiteral("rejectionClass"), rejectionClass},
        {QStringLiteral("applicationName"), QCoreApplication::applicationName()},
        {QStringLiteral("applicationDataRoot"), dataRoot}};
    const QString submissionPath = QDir(testRoot).filePath(
        QStringLiteral("contender-submitted-%1.json").arg(contenderIndex));
    if (!writeJsonAtomically(submissionPath, submissionProof) || !pathIsWithin(submissionPath, testRoot))
    {
        if (accepted)
        {
            QString cleanupError;
            cancelAndReapQueuedAttempt(manager, gate, recoveryJobId, &cleanupError);
        }
        return fail(QStringLiteral("submit_outcome_proof"), QStringLiteral("bounded_proof_publish_failed"),
            {{QStringLiteral("submitAccepted"), accepted}});
    }

    if (!accepted) return 0;
    if (attempt.value(QStringLiteral("jobId")).toString() != recoveryJobId ||
        attempt.value(QStringLiteral("state")).toString() != QStringLiteral("queued") ||
        attempt.value(QStringLiteral("recoverySourceJobId")).toString() != originalJobId ||
        attempt.value(QStringLiteral("logicalJobId")).toString() != originalJobId ||
        headQualification.value(QStringLiteral("eligibility")).toString() != QStringLiteral("reconcile_required"))
    {
        QString cleanupError;
        const bool cleanedUp = cancelAndReapQueuedAttempt(manager, gate, recoveryJobId, &cleanupError);
        return fail(QStringLiteral("winning_task_state"), QStringLiteral("queued_lease_invariant_failed"),
            {{QStringLiteral("recoveryState"), attempt.value(QStringLiteral("state"))},
             {QStringLiteral("sourceMatches"), attempt.value(QStringLiteral("recoverySourceJobId")).toString() == originalJobId},
             {QStringLiteral("logicalIdMatches"), attempt.value(QStringLiteral("logicalJobId")).toString() == originalJobId},
             {QStringLiteral("headEligibility"), headQualification.value(QStringLiteral("eligibility"))},
             {QStringLiteral("cleanupCompleted"), cleanedUp}});
    }

    const QString releasePath = QDir(testRoot).filePath(QStringLiteral("release-winner.json"));
    if (!waitForFile(releasePath, 90000))
    {
        QString cleanupError;
        const bool cleanedUp = cancelAndReapQueuedAttempt(manager, gate, recoveryJobId, &cleanupError);
        return fail(QStringLiteral("winner_hold"), QStringLiteral("parent_collection_timeout"),
            {{QStringLiteral("cleanupCompleted"), cleanedUp}});
    }
    const QJsonObject release = readBoundedJsonObject(releasePath);
    if (release.value(QStringLiteral("schema")).toString() != QString::fromLatin1(contenderBarrierSchema) ||
        release.value(QStringLiteral("winnerIndex")).toInt(-1) != contenderIndex ||
        release.value(QStringLiteral("recoveryJobId")).toString() != recoveryJobId)
    {
        QString cleanupError;
        const bool cleanedUp = cancelAndReapQueuedAttempt(manager, gate, recoveryJobId, &cleanupError);
        return fail(QStringLiteral("winner_release"), QStringLiteral("release_binding_mismatch"),
            {{QStringLiteral("releaseReadable"), !release.isEmpty()},
             {QStringLiteral("winnerIndexMatches"), release.value(QStringLiteral("winnerIndex")).toInt(-1) == contenderIndex},
             {QStringLiteral("jobMatches"), release.value(QStringLiteral("recoveryJobId")).toString() == recoveryJobId},
             {QStringLiteral("cleanupCompleted"), cleanedUp}});
    }

    QString cancellationError;
    if (!cancelAndReapQueuedAttempt(manager, gate, recoveryJobId, &cancellationError))
        return fail(QStringLiteral("winner_cleanup"), QStringLiteral("own_attempt_cancel_or_reap_failed"));
    const QJsonObject finalQualification = manager.nativeJobRecoveryQuery(originalJobId);
    const QJsonArray jobs = manager.jobsQuery(0, 200).value(QStringLiteral("jobs")).toArray();
    int successorCount = 0;
    for (const QJsonValue &value : jobs)
        if (value.toObject().value(QStringLiteral("recoverySourceJobId")).toString() == originalJobId)
            ++successorCount;
    const QJsonObject finalProof{{QStringLiteral("schema"), QString::fromLatin1(contenderProofSchema)},
        {QStringLiteral("phase"), QStringLiteral("final")}, {QStringLiteral("contenderIndex"), contenderIndex},
        {QStringLiteral("jobId"), originalJobId}, {QStringLiteral("recoveryJobId"), recoveryJobId},
        {QStringLiteral("recoveryJobState"), manager.jobQuery(recoveryJobId).value(QStringLiteral("state"))},
        {QStringLiteral("successorCount"), successorCount},
        {QStringLiteral("logicalHeadEligibility"), finalQualification.value(QStringLiteral("eligibility"))}};
    const QString finalPath = QDir(testRoot).filePath(QStringLiteral("contender-final-%1.json").arg(contenderIndex));
    if (!writeJsonAtomically(finalPath, finalProof) || !pathIsWithin(finalPath, testRoot))
        return fail(QStringLiteral("final_proof"), QStringLiteral("bounded_proof_publish_failed"));
    if (finalProof.value(QStringLiteral("recoveryJobState")).toString() != QStringLiteral("cancelled") ||
        successorCount != 1 ||
        finalQualification.value(QStringLiteral("eligibility")).toString() != QStringLiteral("reconcile_required"))
        return fail(QStringLiteral("logical_head_after_cancel"), QStringLiteral("successor_head_invariant_failed"),
            {{QStringLiteral("recoveryState"), finalProof.value(QStringLiteral("recoveryJobState"))},
             {QStringLiteral("successorCount"), successorCount},
             {QStringLiteral("headEligibility"), finalQualification.value(QStringLiteral("eligibility"))}});
    return 0;
}

class TestRecoveryInteractionConsumer : public QObject
{
    Q_OBJECT
    QString testRoot;
    QString jobId;
    QString producerDataRoot;
    QString evidenceDirectory;
    QString projectPath;
    WorkflowWidget *workflow = nullptr;

    bool captureWindow(const QString &name)
    {
        if (QApplication::platformName() != QStringLiteral("windows")) return true;
        auto *window = Application::instance()->getMainWindow();
        QCoreApplication::processEvents();
        return window && window->grab().save(QDir(evidenceDirectory).filePath(name + QStringLiteral(".png")));
    }

private slots:
    void initTestCase()
    {
        testRoot = qEnvironmentVariable("RANGE_RECOVERY_INTERACTION_ROOT");
        QVERIFY(!testRoot.isEmpty());
        QVERIFY(QDir(testRoot).exists());
        const QString expectedApplicationName = recoveryApplicationName(testRoot);
        QCOMPARE(QCoreApplication::applicationName(), expectedApplicationName);
        QString dataError;
        const QString dataRoot = applicationDataRoot(&dataError);
        QVERIFY2(!dataRoot.isEmpty(), qPrintable(dataError));
        QVERIFY(pathEquals(dataRoot, producerDataRoot));
        auto *window = Application::instance()->getMainWindow();
        QVERIFY(window);
        window->resize(1600, 1200);
        auto *controlDock = window->findChild<QDockWidget *>(QStringLiteral("controlDock"));
        QVERIFY(controlDock);
        controlDock->hide();
        window->show();
        QCoreApplication::processEvents();
        if (QApplication::platformName() == QStringLiteral("windows"))
            QVERIFY2(QTest::qWaitForWindowExposed(window, 10000), "The native recovery window was not exposed.");

        evidenceDirectory = QDir(testRoot).filePath(QStringLiteral("evidence"));
        QVERIFY(QDir().mkpath(evidenceDirectory));
        QVERIFY(pathIsWithin(evidenceDirectory, testRoot));

        auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("workflowPages"));
        workflow = window->findChild<WorkflowWidget *>(QStringLiteral("workflowWidget"));
        QVERIFY(tabs && workflow);
        auto *jobChoice = workflow->findChild<QComboBox *>(QStringLiteral("nativeResultJobChoice"));
        QVERIFY(jobChoice);
        Q_UNUSED(dataRoot);
    }

    void cleanup()
    {
        if (workflow) workflow->transport()->stop();
        if (auto *session = Application::instance()->getSession()) session->clear();
    }

    void freshGuiRecoversOnlyAfterVisibleApproval()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        const QJsonObject original = manager.jobQuery(jobId);
        QVERIFY2(original.value(QStringLiteral("found")).toBool(), "The fresh GUI process did not reopen the producer's native job record.");
        QCOMPARE(original.value(QStringLiteral("state")).toString(), QStringLiteral("interrupted"));
        const QJsonObject qualification = manager.nativeJobRecoveryQuery(jobId);
        QCOMPARE(qualification.value(QStringLiteral("eligibility")).toString(), QStringLiteral("recoverable"));
        QCOMPARE(qualification.value(QStringLiteral("checkpoint")).toObject().value(QStringLiteral("acceptedSeq")).toInt(), 10);
        QCOMPARE(qualification.value(QStringLiteral("checkpoint")).toObject().value(QStringLiteral("targetSteps")).toInt(), 20);
        QCOMPARE(qualification.value(QStringLiteral("checkpoint")).toObject().value(QStringLiteral("remainingSteps")).toInt(), 10);
        QVERIFY(recoveryAttemptFor(manager.jobsQuery(0, 200), jobId).isEmpty());

        projectPath = QDir(testRoot).filePath(QStringLiteral("project"));
        QVERIFY(QDir().mkpath(projectPath));
        QVERIFY(pathIsWithin(projectPath, testRoot));
        session->clear();
        QString projectError;
        QVERIFY2(session->openProject(projectPath, &projectError), qPrintable(projectError));
        session->addModel(modalFixture(QStringLiteral("WF05 source document")));
        session->addModel(modalFixture(QStringLiteral("WF05 unrelated draft document")));
        const QString sourceIdentity = session->modelCommandStamp(0).first;
        const QString draftIdentity = session->modelCommandStamp(1).first;
        QVERIFY(!sourceIdentity.isEmpty() && !draftIdentity.isEmpty() && sourceIdentity != draftIdentity);

        // Keep one previously approved proposal pending. A second fresh preview
        // commits a real model edit through the GUI host and its durable ledger.
        const QJsonObject oldProposal = modalPreview(session, sourceIdentity, 19);
        QCOMPARE(oldProposal.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString oldPreviewId = oldProposal.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString();
        QVERIFY(!oldPreviewId.isEmpty());
        QVERIFY(host->approvePreview(oldPreviewId));
        const QJsonObject freshProposal = modalPreview(session, sourceIdentity, 22);
        QCOMPARE(freshProposal.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString freshPreviewId = freshProposal.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString();
        QVERIFY(!freshPreviewId.isEmpty());
        QVERIFY(host->approvePreview(freshPreviewId));
        const QString idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject acceptedEdit = executePreview(session, freshPreviewId, idempotencyKey);
        QCOMPARE(acceptedEdit.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject acceptedResult = acceptedEdit.value(QStringLiteral("result")).toObject();
        const QString operationId = acceptedResult.value(QStringLiteral("operationId")).toString();
        const QJsonObject acceptedSnapshot = acceptedResult.value(QStringLiteral("acceptedSnapshotRef")).toObject();
        QVERIFY(!operationId.isEmpty());
        QVERIFY(!acceptedSnapshot.isEmpty());
        const QString acceptedSnapshotPath = QDir(projectPath).filePath(
            acceptedSnapshot.value(QStringLiteral("relativePath")).toString());
        QVERIFY(pathIsWithin(acceptedSnapshotPath, projectPath));
        QCOMPARE(sha256File(acceptedSnapshotPath), acceptedSnapshot.value(QStringLiteral("modelSha256")).toString());
        QCOMPARE(acceptedSnapshot.value(QStringLiteral("persistentDocumentId")).toString(),
                 session->documentProjectContext(session->findModelByCommandIdentity(sourceIdentity))
                     .value(QStringLiteral("persistentDocumentId")).toString());
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(sourceIdentity))
                     .getProblemSetup().getModalSetup().getNIterations(), 22u);
        const QJsonObject operationQuery = host->handle(hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("operationId"), operationId}}));
        QCOMPARE(operationQuery.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject queriedOperationResult = operationQuery.value(QStringLiteral("result")).toObject();
        QCOMPARE(queriedOperationResult, acceptedResult);
        QString ledgerError;
        auto *catalog = session->getProjectCatalog();
        QVERIFY(catalog);
        const auto persistedOperation = catalog->queryOperation(operationId, {}, &ledgerError);
        QVERIFY2(persistedOperation.has_value(), qPrintable(ledgerError));
        QCOMPARE(persistedOperation->phase, RProjectCatalog::OperationPhase::Observed);
        QCOMPARE(persistedOperation->association.acceptedModelSha256,
                 acceptedSnapshot.value(QStringLiteral("modelSha256")).toString());
        const QJsonObject expectedObservation{
            {QStringLiteral("status"), acceptedEdit.value(QStringLiteral("status"))},
            {QStringLiteral("code"), acceptedEdit.value(QStringLiteral("code"))},
            {QStringLiteral("result"), acceptedResult}};
        QCOMPARE(persistedOperation->association.observation, expectedObservation);

        const uint sourceModelId = session->findModelByCommandIdentity(sourceIdentity);
        const uint draftModelId = session->findModelByCommandIdentity(draftIdentity);
        QVERIFY(sourceModelId != RConstants::eod && draftModelId != RConstants::eod);
        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(sourceModelId);
        const QPair<QString, quint64> draftStamp = session->modelCommandStamp(draftModelId);
        session->setModelSelected(sourceModelId, false);
        session->setModelSelected(draftModelId, true);
        QCoreApplication::processEvents();
        ProblemTree draftTree;
        auto *draftEditor = namedProblemEditor(&draftTree, QStringLiteral("modalTolerance"));
        QVERIFY(draftEditor);
        draftEditor->setText(QStringLiteral("1e-"));
        QTRY_VERIFY_WITH_TIMEOUT(session->hasDocumentDraft(draftModelId), 5000);
        QCOMPARE(session->modelCommandStamp(draftModelId), draftStamp);
        QVERIFY(session->canMutateDocument(sourceModelId));

        auto *window = Application::instance()->getMainWindow();
        auto *controlDock = window->findChild<QDockWidget *>(QStringLiteral("controlDock"));
        auto *pages = workflow->findChild<QTabWidget *>(QStringLiteral("workflowPages"));
        auto *jobChoice = workflow->findChild<QComboBox *>(QStringLiteral("nativeResultJobChoice"));
        auto *checkButton = workflow->findChild<QPushButton *>(QStringLiteral("nativeRecoveryCheck"));
        auto *approveButton = workflow->findChild<QPushButton *>(QStringLiteral("nativeRecoveryApply"));
        auto *recoveryStatus = workflow->findChild<QLabel *>(QStringLiteral("nativeRecoveryStatus"));
        QVERIFY(controlDock && pages && jobChoice && checkButton && approveButton && recoveryStatus);
        controlDock->show();
        controlDock->raise();
        window->resizeDocks({controlDock}, {420}, Qt::Vertical);
        window->raise();
        window->activateWindow();
        const int documentOutputPage = pages->indexOf(checkButton->parentWidget());
        QVERIFY(documentOutputPage >= 0);
        pages->setCurrentIndex(documentOutputPage); // Document FE output contains the native recovery controls.
        QCoreApplication::processEvents();
        QVERIFY(workflow->isVisible());
        if (QApplication::platformName() == QStringLiteral("windows"))
            QVERIFY2(QTest::qWaitForWindowExposed(window, 10000), "The recovery controls are not exposed in the native GUI.");

        QTRY_VERIFY_WITH_TIMEOUT(jobChoice->findData(jobId) >= 0, 15000);
        jobChoice->setCurrentIndex(jobChoice->findData(jobId));
        QCoreApplication::processEvents();
        QVERIFY(checkButton->isVisible() && checkButton->isEnabled());
        auto *resultSource = checkButton->parentWidget()->findChild<QLabel *>(QStringLiteral("nativeResultSource"));
        QVERIFY(resultSource);
        QVERIFY(resultSource->text().contains(QStringLiteral("No verified native result provenance")));
        QVERIFY(!resultSource->text().contains(QStringLiteral("Template snapshot")));
        QVERIFY(captureWindow(QStringLiteral("wf05-before-recovery-check")));
        QTest::mouseClick(checkButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(approveButton->isEnabled(), 15000);
        QVERIFY(recoveryStatus->text().contains(QStringLiteral("step 10"), Qt::CaseInsensitive));
        QVERIFY(recoveryStatus->text().contains(QStringLiteral("target 20"), Qt::CaseInsensitive));
        QVERIFY(recoveryAttemptFor(manager.jobsQuery(0, 200), jobId).isEmpty());
        QVERIFY(captureWindow(QStringLiteral("wf05-recovery-qualified-awaiting-approval")));

        ThreadPoolGate gate;
        QVERIFY(gate.start());
        QVERIFY(approveButton->isVisible() && approveButton->isEnabled());
        QTest::mouseClick(approveButton, Qt::LeftButton);
        QVERIFY(recoveryStatus->text().contains(QStringLiteral("Recovery attempt")));
        QJsonObject attempt = recoveryAttemptFor(manager.jobsQuery(0, 200), jobId);
        QTRY_VERIFY_WITH_TIMEOUT(!attempt.isEmpty(), 15000);
        const QString recoveryJobId = attempt.value(QStringLiteral("jobId")).toString();
        QVERIFY(!recoveryJobId.isEmpty() && recoveryJobId != jobId);
        QCOMPARE(attempt.value(QStringLiteral("state")).toString(), QStringLiteral("queued"));

        const QJsonObject progressResponse = host->handle(hostRequest(QStringLiteral("native.job.query"),
            {{QStringLiteral("jobId"), recoveryJobId}}));
        QCOMPARE(progressResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject progress = progressResponse.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("job")).toObject();
        QCOMPARE(progress.value(QStringLiteral("jobId")).toString(), recoveryJobId);
        QCOMPARE(progress.value(QStringLiteral("state")).toString(), QStringLiteral("queued"));
        QVERIFY(progress.contains(QStringLiteral("progressSequence")));
        QVERIFY(progress.contains(QStringLiteral("progressStatus")));

        QString cancellationError;
        QVERIFY2(manager.cancelJob(recoveryJobId, &cancellationError), qPrintable(cancellationError));
        gate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(recoveryJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        QCOMPARE(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("interrupted"));
        QCOMPARE(manager.nativeJobRecoveryQuery(jobId).value(QStringLiteral("eligibility")).toString(),
                 QStringLiteral("reconcile_required"));

        QCOMPARE(session->findModelByCommandIdentity(sourceIdentity), sourceModelId);
        QCOMPARE(session->findModelByCommandIdentity(draftIdentity), draftModelId);
        QCOMPARE(session->modelCommandStamp(sourceModelId), sourceStamp);
        QCOMPARE(session->modelCommandStamp(draftModelId), draftStamp);
        QCOMPARE(session->getModel(sourceModelId).getProblemSetup().getModalSetup().getNIterations(), 22u);
        QCOMPARE(session->getModel(draftModelId).getProblemSetup().getModalSetup().getNIterations(), 12u);
        QVERIFY(session->hasDocumentDraft(draftModelId));
        QVERIFY(captureWindow(QStringLiteral("wf05-recovery-attempt-cancelled")));
    }

public:
    TestRecoveryInteractionConsumer(QString sourceJobId, QString dataRoot)
        : jobId(std::move(sourceJobId)), producerDataRoot(std::move(dataRoot)) {}
};

class OwnChildProcessReaper
{
public:
    OwnChildProcessReaper(QProcess *processes, int count) : processes(processes), count(count) {}
    ~OwnChildProcessReaper()
    {
        for (int index = 0; index < count; ++index)
        {
            QProcess *process = processes ? &processes[index] : nullptr;
            if (!process || process->state() == QProcess::NotRunning) continue;
            // These are only the two direct child processes created by this test.
            process->kill();
            process->waitForFinished(5000);
        }
    }
private:
    QProcess *processes;
    int count;
};

class TestRecoveryInteractionOrchestrator : public QObject
{
    Q_OBJECT
private slots:
    void abruptOwnerExitThenFreshGuiRecovery()
    {
#ifndef Q_OS_WIN
        QSKIP("The owner-plus-solver crash boundary requires Windows direct-child PID and full-image-path verification.");
#endif
        QTemporaryDir testDirectory(QDir::tempPath() + QStringLiteral("/range-wf05-recovery-XXXXXX"));
        QVERIFY(testDirectory.isValid());
        const bool retainNativeEvidence = QApplication::platformName() == QStringLiteral("windows");
        const bool retainRoot = retainNativeEvidence || keepRecoveryTestRoot();
        testDirectory.setAutoRemove(false);
        const QString testRoot = QFileInfo(testDirectory.path()).canonicalFilePath();
        QVERIFY(!testRoot.isEmpty());
        const QString applicationName = recoveryApplicationName(testRoot);
        const QString producerProofPath = QDir(testRoot).filePath(QStringLiteral("producer-ready.json"));
        QVERIFY(pathIsWithin(testDirectory.path(), QDir::tempPath()));
        if (retainNativeEvidence || keepRecoveryTestRoot())
            qInfo().noquote() << "WF05 native GUI test root:" << testRoot;

        EnvironmentRestore restore;
        const QByteArray rootBytes = testRoot.toUtf8();
        qputenv("RANGE_RECOVERY_INTERACTION_ROOT", rootBytes);
        QProcessEnvironment environment = childEnvironment();
        environment.insert(QStringLiteral("RANGE_RECOVERY_INTERACTION_ROOT"), testRoot);

        qint64 producerPid = 0;
        const auto runChild = [&](const QStringList &arguments, int timeoutMs, int expectedCode) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.setProgram(QCoreApplication::applicationFilePath());
            process.setArguments(arguments);
            process.setProcessChannelMode(QProcess::SeparateChannels);
            process.start();
            if (!process.waitForStarted(10000))
            {
                qWarning("Recovery interaction child process failed to start.");
                return false;
            }
            if (arguments.contains(QStringLiteral("--producer"))) producerPid = process.processId();
            QElapsedTimer deadline;
            deadline.start();
            while (process.state() != QProcess::NotRunning && deadline.elapsed() < timeoutMs)
            {
                process.waitForFinished(20);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            if (process.state() != QProcess::NotRunning)
            {
                if (arguments.contains(QStringLiteral("--producer")))
                {
                    QString solverPath;
                    QString solverSha;
                    QString cleanupError;
                    quint64 solverPid = 0;
                    if (configuredSolverExecutable(&solverPath, &solverSha, &cleanupError))
                        terminateOwnedSolverChild(process.processId(), solverPath, solverSha,
                                                  &solverPid, &cleanupError);
                }
                process.kill();
                process.waitForFinished(10000);
                testDirectory.setAutoRemove(false);
                qInfo().noquote() << "WF05 failure artifacts retained:" << testRoot;
                qWarning("Recovery interaction child process timed out.");
                return false;
            }
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expectedCode)
            {
                testDirectory.setAutoRemove(false);
                qInfo().noquote() << "WF05 failure artifacts retained:" << testRoot;
                const QJsonObject failureFacts = readBoundedJsonObject(
                    QDir(testRoot).filePath(QStringLiteral("producer-failure.json")), 8192);
                qWarning().noquote() << "Recovery interaction child returned exit code" << process.exitCode()
                    << "bounded facts:" << QString::fromUtf8(
                        QJsonDocument(failureFacts).toJson(QJsonDocument::Compact));
                if (arguments.contains(QStringLiteral("--consumer")))
                    qInfo().noquote() << "WF05 consumer QtTest report retained at:"
                        << QDir(testRoot).filePath(QStringLiteral("consumer-results.txt"));
                return false;
            }
            return true;
        };

        QVERIFY(runChild({QStringLiteral("--recovery-interaction"), QStringLiteral("--producer"), testRoot},
                         150000, producerAbruptExitCode));
        const QJsonObject producerProof = readJsonObject(producerProofPath);
        QCOMPARE(producerProof.value(QStringLiteral("schema")).toString(), QString::fromLatin1(producerProofSchema));
        QCOMPARE(producerProof.value(QStringLiteral("applicationName")).toString(), applicationName);
        QCOMPARE(producerProof.value(QStringLiteral("acceptedSeq")).toInt(), 10);
        QCOMPARE(producerProof.value(QStringLiteral("targetSteps")).toInt(), 20);
        QCOMPARE(qint64(producerProof.value(QStringLiteral("ownerPid")).toDouble()), producerPid);
        QVERIFY(producerProof.value(QStringLiteral("solverPid")).toDouble() > 0.0);
        QCOMPARE(producerProof.value(QStringLiteral("solverTerminationExitCode")).toInt(), solverAbruptExitCode);
        QVERIFY(producerProof.value(QStringLiteral("solverProcessTerminated")).toBool());
        QVERIFY(hasVerifiedPausedExecutionProof(producerProof, testRoot));
        QCOMPARE(producerProof.value(QStringLiteral("liveOwnerEligibility")).toString(),
                 QStringLiteral("reconcile_required"));
        QVERIFY(pathEquals(producerProof.value(QStringLiteral("solverExecutableRef")).toString(),
                           QStringLiteral(RANGE_RECOVERY_INTERACTION_SOLVER)));
        QCOMPARE(producerProof.value(QStringLiteral("solverExecutableSha256")).toString(),
                 sha256File(producerProof.value(QStringLiteral("solverExecutableRef")).toString()));
        QVERIFY(containsPathComponent(producerProof.value(QStringLiteral("applicationDataRoot")).toString(),
                                     QStringLiteral("qttest")));
        QVERIFY(containsPathComponent(producerProof.value(QStringLiteral("applicationDataRoot")).toString(),
                                     applicationName));
        const QString jobId = producerProof.value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());
        QVERIFY(pathIsWithin(producerProofPath, testRoot));
        QVERIFY(pathIsWithin(producerProof.value(QStringLiteral("checkpointDirectory")).toString(), testRoot));
        QCOMPARE(sha256File(producerProof.value(QStringLiteral("envelopeRef")).toString()),
                 producerProof.value(QStringLiteral("envelopeSha256")).toString());
        QCOMPARE(sha256File(producerProof.value(QStringLiteral("checkpointModelRef")).toString()),
                 producerProof.value(QStringLiteral("checkpointModelSha256")).toString());
        qInfo("WF05 crash boundary deliberately terminated the verified fea-solver child at accepted step 10, then abandoned its GUI owner with _Exit(86).");

        const QString consumerReportPath = QDir(testRoot).filePath(QStringLiteral("consumer-results.txt"));
        QVERIFY(runChild({QStringLiteral("--recovery-interaction"), QStringLiteral("--consumer"), testRoot,
                          QStringLiteral("-o"), consumerReportPath + QStringLiteral(",txt")}, 180000, 0));
        if (QApplication::platformName() == QStringLiteral("windows"))
        {
            qInfo().noquote() << "WF05 native GUI evidence:" << QDir(testRoot).filePath(QStringLiteral("evidence"));
        }
        if (!retainRoot) testDirectory.setAutoRemove(true);
        Q_UNUSED(applicationName);
    }

    void competingFreshProcessesPublishOneRecoverySuccessor()
    {
#ifndef Q_OS_WIN
        QSKIP("Cross-process recovery exclusion requires Windows direct-child solver termination evidence.");
#endif
        QTemporaryDir testDirectory(QDir::tempPath() + QStringLiteral("/range-wf05-recovery-race-XXXXXX"));
        QVERIFY(testDirectory.isValid());
        const bool retainEvidence = QApplication::platformName() == QStringLiteral("windows");
        const bool retainRoot = retainEvidence || keepRecoveryTestRoot();
        testDirectory.setAutoRemove(false);
        const QString testRoot = QFileInfo(testDirectory.path()).canonicalFilePath();
        QVERIFY(!testRoot.isEmpty());
        QVERIFY(pathIsWithin(testDirectory.path(), QDir::tempPath()));
        if (retainEvidence || keepRecoveryTestRoot())
            qInfo().noquote() << "WF05 recovery race test root:" << testRoot;
        const QString applicationName = recoveryApplicationName(testRoot);

        EnvironmentRestore restore;
        qputenv("RANGE_RECOVERY_INTERACTION_ROOT", testRoot.toUtf8());
        QProcessEnvironment environment = childEnvironment();
        environment.insert(QStringLiteral("RANGE_RECOVERY_INTERACTION_ROOT"), testRoot);

        const auto waitChild = [&](QProcess &process, int timeoutMs, int expectedCode, int contenderIndex) {
            QElapsedTimer deadline;
            deadline.start();
            while (process.state() != QProcess::NotRunning && deadline.elapsed() < timeoutMs)
            {
                process.waitForFinished(20);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            if (process.state() != QProcess::NotRunning) return false;
            if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == expectedCode) return true;
            testDirectory.setAutoRemove(false);
            qInfo().noquote() << "WF05 recovery race failure artifacts retained:" << testRoot;
            const QJsonObject failureFacts = readBoundedJsonObject(QDir(testRoot).filePath(
                QStringLiteral("contender-failure-%1.json").arg(contenderIndex)), 8192);
            qWarning().noquote() << "Recovery race child returned exit code" << process.exitCode()
                << "bounded facts:" << QString::fromUtf8(QJsonDocument(failureFacts).toJson(QJsonDocument::Compact));
            return false;
        };

        // This is a distinct producer/test root. The GUI-story test's cancelled
        // recovery successor must not become this race's logical-job head.
        QProcess producer;
        producer.setProcessEnvironment(environment);
        producer.setProgram(QCoreApplication::applicationFilePath());
        producer.setArguments({QStringLiteral("--recovery-interaction"), QStringLiteral("--producer"), testRoot});
        producer.setProcessChannelMode(QProcess::SeparateChannels);
        producer.start();
        QVERIFY2(producer.waitForStarted(10000), "The independent recovery producer did not start.");
        const qint64 producerPid = producer.processId();
        QElapsedTimer producerDeadline;
        producerDeadline.start();
        while (producer.state() != QProcess::NotRunning && producerDeadline.elapsed() < 150000)
        {
            producer.waitForFinished(20);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        if (producer.state() != QProcess::NotRunning)
        {
            QString solverPath;
            QString solverSha;
            QString cleanupError;
            quint64 solverPid = 0;
            if (configuredSolverExecutable(&solverPath, &solverSha, &cleanupError))
                terminateOwnedSolverChild(producerPid, solverPath, solverSha, &solverPid, &cleanupError);
            producer.kill();
            producer.waitForFinished(10000);
            testDirectory.setAutoRemove(false);
            qInfo().noquote() << "WF05 recovery race failure artifacts retained:" << testRoot;
            QFAIL("The independent native checkpoint producer timed out.");
        }
        if (producer.exitStatus() != QProcess::NormalExit || producer.exitCode() != producerAbruptExitCode)
        {
            testDirectory.setAutoRemove(false);
            qInfo().noquote() << "WF05 recovery race failure artifacts retained:" << testRoot;
            const QJsonObject failureFacts = readBoundedJsonObject(
                QDir(testRoot).filePath(QStringLiteral("producer-failure.json")), 8192);
            qWarning().noquote() << "Recovery race producer returned exit code" << producer.exitCode()
                << "bounded facts:" << QString::fromUtf8(QJsonDocument(failureFacts).toJson(QJsonDocument::Compact));
            QFAIL("The independent native checkpoint producer did not stop at its verified crash boundary.");
        }
        const QJsonObject producerProof = readBoundedJsonObject(
            QDir(testRoot).filePath(QStringLiteral("producer-ready.json")));
        const QString originalJobId = producerProof.value(QStringLiteral("jobId")).toString();
        const QString checkpointSha256 = producerProof.value(QStringLiteral("checkpointModelSha256")).toString();
        QCOMPARE(producerProof.value(QStringLiteral("schema")).toString(), QString::fromLatin1(producerProofSchema));
        QCOMPARE(producerProof.value(QStringLiteral("applicationName")).toString(), applicationName);
        QCOMPARE(qint64(producerProof.value(QStringLiteral("ownerPid")).toDouble()), producerPid);
        QCOMPARE(producerProof.value(QStringLiteral("acceptedSeq")).toInt(), 10);
        QCOMPARE(producerProof.value(QStringLiteral("targetSteps")).toInt(), 20);
        QVERIFY(!originalJobId.isEmpty() && checkpointSha256.size() == 64);
        QVERIFY(producerProof.value(QStringLiteral("solverProcessTerminated")).toBool());
        QVERIFY(hasVerifiedPausedExecutionProof(producerProof, testRoot));

        QProcess contenders[2];
        OwnChildProcessReaper contenderReaper(contenders, 2);
        const QString qualifiedPath0 = QDir(testRoot).filePath(QStringLiteral("contender-qualified-0.json"));
        const QString qualifiedPath1 = QDir(testRoot).filePath(QStringLiteral("contender-qualified-1.json"));
        const QString qualifiedPaths[] = {qualifiedPath0, qualifiedPath1};
        const auto waitForQualification = [&](int index) {
            QElapsedTimer deadline;
            deadline.start();
            while (!QFileInfo::exists(qualifiedPaths[index]) && deadline.elapsed() < 90000)
            {
                if (contenders[index].state() == QProcess::NotRunning)
                {
                    testDirectory.setAutoRemove(false);
                    qInfo().noquote() << "WF05 recovery race failure artifacts retained:" << testRoot;
                    const QJsonObject failureFacts = readBoundedJsonObject(QDir(testRoot).filePath(
                        QStringLiteral("contender-failure-%1.json").arg(index)), 8192);
                    qWarning().noquote() << "Contender exited before qualification; index:" << index
                        << "exit code:" << contenders[index].exitCode() << "bounded facts:"
                        << QString::fromUtf8(QJsonDocument(failureFacts).toJson(QJsonDocument::Compact));
                    return false;
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                QTest::qWait(5);
            }
            if (!QFileInfo::exists(qualifiedPaths[index]))
            {
                testDirectory.setAutoRemove(false);
                qInfo().noquote() << "WF05 recovery race failure artifacts retained:" << testRoot;
                qWarning() << "Contender qualification timed out; index:" << index;
                return false;
            }
            return true;
        };
        for (int index = 0; index < 2; ++index)
        {
            contenders[index].setProcessEnvironment(environment);
            contenders[index].setProgram(QCoreApplication::applicationFilePath());
            contenders[index].setArguments({QStringLiteral("--recovery-interaction"),
                QStringLiteral("--contender"), testRoot, QString::number(index)});
            contenders[index].setProcessChannelMode(QProcess::SeparateChannels);
            contenders[index].start();
            QVERIFY2(contenders[index].waitForStarted(10000), "Both independent recovery contenders must start.");
            // Serialize only the read-only qualification work. Each process is
            // held at the same submit barrier, so the public submissions still race.
            QVERIFY2(waitForQualification(index),
                     "Each contender must independently qualify before the submit barrier opens.");
        }
        QVERIFY2(QFileInfo::exists(qualifiedPath0) && QFileInfo::exists(qualifiedPath1),
                 "Both fresh processes must qualify the checkpoint before either can submit.");
        QVERIFY(pathIsWithin(qualifiedPath0, testRoot) && pathIsWithin(qualifiedPath1, testRoot));
        const QJsonObject qualified0 = readBoundedJsonObject(qualifiedPath0);
        const QJsonObject qualified1 = readBoundedJsonObject(qualifiedPath1);
        for (const QJsonObject &proof : {qualified0, qualified1})
        {
            QCOMPARE(proof.value(QStringLiteral("schema")).toString(), QString::fromLatin1(contenderProofSchema));
            QCOMPARE(proof.value(QStringLiteral("phase")).toString(), QStringLiteral("qualified"));
            QCOMPARE(proof.value(QStringLiteral("jobId")).toString(), originalJobId);
            QCOMPARE(proof.value(QStringLiteral("eligibility")).toString(), QStringLiteral("recoverable"));
            QCOMPARE(proof.value(QStringLiteral("acceptedSeq")).toInt(), 10);
            QCOMPARE(proof.value(QStringLiteral("targetSteps")).toInt(), 20);
            QCOMPARE(proof.value(QStringLiteral("remainingSteps")).toInt(), 10);
            QCOMPARE(proof.value(QStringLiteral("checkpointSha256")).toString(), checkpointSha256);
            QCOMPARE(proof.value(QStringLiteral("applicationName")).toString(), applicationName);
            QCOMPARE(proof.value(QStringLiteral("applicationDataRoot")).toString(),
                     producerProof.value(QStringLiteral("applicationDataRoot")).toString());
        }
        QVERIFY(qualified0.value(QStringLiteral("ownerPid")).toDouble() !=
                qualified1.value(QStringLiteral("ownerPid")).toDouble());

        const QString submitBarrierPath = QDir(testRoot).filePath(QStringLiteral("submit-barrier.json"));
        QVERIFY(pathIsWithin(QFileInfo(submitBarrierPath).absolutePath(), testRoot));
        QVERIFY(writeJsonAtomically(submitBarrierPath, {{QStringLiteral("schema"), QString::fromLatin1(contenderBarrierSchema)},
            {QStringLiteral("jobId"), originalJobId}, {QStringLiteral("checkpointSha256"), checkpointSha256},
            {QStringLiteral("remainingSteps"), 10}}));

        const QString submittedPath0 = QDir(testRoot).filePath(QStringLiteral("contender-submitted-0.json"));
        const QString submittedPath1 = QDir(testRoot).filePath(QStringLiteral("contender-submitted-1.json"));
        QElapsedTimer submitDeadline;
        submitDeadline.start();
        while ((!QFileInfo::exists(submittedPath0) || !QFileInfo::exists(submittedPath1)) &&
               submitDeadline.elapsed() < 90000)
        {
            for (int index = 0; index < 2; ++index)
            {
                const QString proofPath = index == 0 ? submittedPath0 : submittedPath1;
                if (contenders[index].state() == QProcess::NotRunning && !QFileInfo::exists(proofPath))
                {
                    testDirectory.setAutoRemove(false);
                    qInfo().noquote() << "WF05 recovery race failure artifacts retained:" << testRoot;
                    const QJsonObject failureFacts = readBoundedJsonObject(QDir(testRoot).filePath(
                        QStringLiteral("contender-failure-%1.json").arg(index)), 8192);
                    qWarning() << "Contender exited before its submit outcome; index:" << index
                        << "exit code:" << contenders[index].exitCode()
                        << "bounded facts:" << QString::fromUtf8(QJsonDocument(failureFacts).toJson(QJsonDocument::Compact));
                    QFAIL("A contender exited before publishing one submit outcome.");
                }
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QTest::qWait(5);
        }
        QVERIFY2(QFileInfo::exists(submittedPath0) && QFileInfo::exists(submittedPath1),
                 "Both submit outcomes must be collected while the winning recovery task remains queued.");
        QVERIFY(pathIsWithin(submittedPath0, testRoot) && pathIsWithin(submittedPath1, testRoot));
        const QJsonObject submitted0 = readBoundedJsonObject(submittedPath0);
        const QJsonObject submitted1 = readBoundedJsonObject(submittedPath1);
        QCOMPARE(submitted0.value(QStringLiteral("phase")).toString(), QStringLiteral("submitted"));
        QCOMPARE(submitted1.value(QStringLiteral("phase")).toString(), QStringLiteral("submitted"));
        QCOMPARE(submitted0.value(QStringLiteral("jobId")).toString(), originalJobId);
        QCOMPARE(submitted1.value(QStringLiteral("jobId")).toString(), originalJobId);
        const bool accepted0 = submitted0.value(QStringLiteral("accepted")).toBool();
        const bool accepted1 = submitted1.value(QStringLiteral("accepted")).toBool();
        QVERIFY2(accepted0 != accepted1, "Exactly one public recovery submit may create a successor task.");
        const int winnerIndex = accepted0 ? 0 : 1;
        const QJsonObject winnerProof = accepted0 ? submitted0 : submitted1;
        const QJsonObject loserProof = accepted0 ? submitted1 : submitted0;
        const QString recoveryJobId = winnerProof.value(QStringLiteral("recoveryJobId")).toString();
        QVERIFY(!recoveryJobId.isEmpty() && recoveryJobId != originalJobId);
        QCOMPARE(winnerProof.value(QStringLiteral("recoveryJobState")).toString(), QStringLiteral("queued"));
        QCOMPARE(winnerProof.value(QStringLiteral("recoverySourceJobId")).toString(), originalJobId);
        QCOMPARE(winnerProof.value(QStringLiteral("logicalJobId")).toString(), originalJobId);
        QCOMPARE(winnerProof.value(QStringLiteral("headEligibility")).toString(), QStringLiteral("reconcile_required"));
        QVERIFY(loserProof.value(QStringLiteral("recoveryJobId")).toString().isEmpty());
        const QString loserRejectionClass = loserProof.value(QStringLiteral("rejectionClass")).toString();
        QVERIFY(loserRejectionClass == QStringLiteral("reconcile_required") ||
                loserRejectionClass == QStringLiteral("logical_lock_rejected") ||
                loserRejectionClass == QStringLiteral("active_successor_rejected"));

        const QString releasePath = QDir(testRoot).filePath(QStringLiteral("release-winner.json"));
        QVERIFY(writeJsonAtomically(releasePath, {{QStringLiteral("schema"), QString::fromLatin1(contenderBarrierSchema)},
            {QStringLiteral("winnerIndex"), winnerIndex}, {QStringLiteral("recoveryJobId"), recoveryJobId}}));
        QVERIFY(pathIsWithin(releasePath, testRoot));
        QVERIFY(waitChild(contenders[0], 90000, 0, 0));
        QVERIFY(waitChild(contenders[1], 90000, 0, 1));
        const QString finalPath = QDir(testRoot).filePath(QStringLiteral("contender-final-%1.json").arg(winnerIndex));
        QVERIFY(pathIsWithin(finalPath, testRoot));
        const QJsonObject finalProof = readBoundedJsonObject(finalPath);
        QCOMPARE(finalProof.value(QStringLiteral("schema")).toString(), QString::fromLatin1(contenderProofSchema));
        QCOMPARE(finalProof.value(QStringLiteral("phase")).toString(), QStringLiteral("final"));
        QCOMPARE(finalProof.value(QStringLiteral("recoveryJobId")).toString(), recoveryJobId);
        QCOMPARE(finalProof.value(QStringLiteral("recoveryJobState")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(finalProof.value(QStringLiteral("successorCount")).toInt(-1), 1);
        QCOMPARE(finalProof.value(QStringLiteral("logicalHeadEligibility")).toString(),
                 QStringLiteral("reconcile_required"));
        if (!retainRoot) testDirectory.setAutoRemove(true);
    }
};

QString rolePath(int argc, char **argv, const QByteArray &role)
{
    for (int index = 1; index + 1 < argc; ++index)
        if (QByteArray(argv[index]) == role) return QString::fromLocal8Bit(argv[index + 1]);
    return {};
}

int runQtTestWithoutRoleArguments(QObject *test, int argc, char **argv, const QByteArray &role)
{
    QVector<char *> arguments;
    arguments.reserve(argc);
    arguments.append(argv[0]);
    for (int index = 1; index < argc; ++index)
    {
        if (QByteArray(argv[index]) == role)
        {
            ++index; // Skip the role value and its bounded temporary root.
            continue;
        }
        arguments.append(argv[index]);
    }
    return QTest::qExec(test, arguments.size(), arguments.data());
}
}

bool terminateVerifiedOwnRecoverySolver(const QString &solverPath, QString *error)
{
    if (error) error->clear();
#ifdef Q_OS_WIN
    if (!Application::instance() || !error)
    {
        if (error) *error = QStringLiteral("The application or bounded process error output is unavailable.");
        return false;
    }
    QString configuredPath;
    QString configuredSha256;
    QString localError;
    if (!configuredSolverExecutable(&configuredPath, &configuredSha256, &localError) ||
        !pathEquals(QFileInfo(solverPath).canonicalFilePath(), configuredPath))
    {
        *error = localError.isEmpty()
            ? QStringLiteral("The supplied solver path does not match the configured build target.")
            : localError.left(512);
        return false;
    }
    quint64 childPid = 0;
    const bool terminated = terminateOwnedSolverChild(QCoreApplication::applicationPid(),
        configuredPath, configuredSha256, &childPid, &localError);
    if (!terminated) *error = localError.left(512);
    else if (childPid == 0)
    {
        *error = QStringLiteral("The terminated solver child PID was not returned by the bounded process check.");
        return false;
    }
    return terminated;
#else
    Q_UNUSED(solverPath);
    if (error) *error = QStringLiteral("Verified direct-child solver termination is supported only on Windows.");
    return false;
#endif
}

int runRecoveryInteractionTests(int argc, char **argv)
{
    for (int index = 1; index + 2 < argc; ++index)
        if (QByteArray(argv[index]) == QByteArrayLiteral("--contender"))
        {
            bool validIndex = false;
            const int contenderIndex = QString::fromLocal8Bit(argv[index + 2]).toInt(&validIndex);
            if (!validIndex) return 90;
            return runContender(QString::fromLocal8Bit(argv[index + 1]), contenderIndex);
        }

    const QString producerRoot = rolePath(argc, argv, QByteArrayLiteral("--producer"));
    if (!producerRoot.isEmpty()) return runProducer(producerRoot);

    const QString consumerRoot = rolePath(argc, argv, QByteArrayLiteral("--consumer"));
    if (!consumerRoot.isEmpty())
    {
        const QString expectedName = recoveryApplicationName(consumerRoot);
        if (QCoreApplication::applicationName() != expectedName ||
            !pathEquals(qEnvironmentVariable("RANGE_RECOVERY_INTERACTION_ROOT"), consumerRoot))
            return 2;
        const QJsonObject producerProof = readJsonObject(QDir(consumerRoot).filePath(QStringLiteral("producer-ready.json")));
        const QString jobId = producerProof.value(QStringLiteral("jobId")).toString();
        const QString dataRoot = producerProof.value(QStringLiteral("applicationDataRoot")).toString();
        if (producerProof.value(QStringLiteral("schema")).toString() != QString::fromLatin1(producerProofSchema) ||
            producerProof.value(QStringLiteral("acceptedSeq")).toInt(-1) != 10 ||
            producerProof.value(QStringLiteral("targetSteps")).toInt(-1) != 20 || jobId.isEmpty() || dataRoot.isEmpty())
            return 3;
        if (!producerProof.value(QStringLiteral("solverProcessTerminated")).toBool() ||
            producerProof.value(QStringLiteral("liveOwnerEligibility")).toString() != QStringLiteral("reconcile_required"))
            return 4;
        TestRecoveryInteractionConsumer test(jobId, dataRoot);
        return runQtTestWithoutRoleArguments(&test, argc, argv, QByteArrayLiteral("--consumer"));
    }

    TestRecoveryInteractionOrchestrator test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_recovery_interaction.moc"
