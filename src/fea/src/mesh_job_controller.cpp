#include "mesh_job_controller.h"
#include "model_command_host.h"
#include "session.h"
#include <rmesh_candidate_service.h>
#include <rmesh_job_service.h>
#include <rmodel_command_service.h>
#include <rbl_error.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QMap>
#include <QScopeGuard>
#include <string>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject reply(bool valid, const QString &code, const QString &message, const QJsonObject &result = {})
{
    return {{"valid",valid},{"code",code},{"message",message},{"result",result}};
}
bool saveObject(const QString &path, const QJsonObject &object, QString *error)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    { if (error) *error = file.errorString(); return false; }
    return true;
}
QJsonObject readObject(const QString &path, qint64 limit = 1024*1024)
{
    const QFileInfo info(path);
    QFile file(path);
    if (!info.isFile() || info.isSymLink() || info.size() > limit || !file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.read(limit+1), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}
bool canonicalDirectory(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && QDir::cleanPath(info.canonicalFilePath()) == QDir::cleanPath(path);
}
QJsonObject stamp(Session *session, uint id)
{
    const auto value = session->modelCommandStamp(id);
    return {{"documentId",value.first},{"sessionEpoch",session->getModelCommandHost()->sessionEpoch()},
            {"documentRevision",double(value.second)}};
}
bool terminal(const QString &status)
{
    return status != "queued" && status != "running" && status != "cancelling" && status != "timing_out";
}
bool stopSuppressed(const QJsonObject &entry)
{
    const QString decision = entry.value("terminalDecision").toString();
    const QString status = entry.value("status").toString();
    return !decision.isEmpty() || status == "cancelled" || status == "timed_out"
        || status == "cancelled_unknown" || status == "timed_out_unknown"
        || status == "interrupted_unknown" || entry.value("code") == QJsonValue("worker_timeout");
}
QJsonObject physicalIds(const RModel &model)
{
    QJsonArray nodes, elements;
    for (uint i=0;i<model.getNNodes() && i<64;++i) nodes.append(double(model.getNodePhysicalId(i)));
    for (uint i=0;i<model.getNElements() && i<64;++i) elements.append(double(model.getElementPhysicalId(i)));
    return {{"nodeCount",double(model.getNNodes())},{"elementCount",double(model.getNElements())},
        {"nodePhysicalIdsFirst64",nodes},{"elementPhysicalIdsFirst64",elements},
        {"truncated",model.getNNodes()>64 || model.getNElements()>64}};
}

//! The worker is attached to KILL_ON_JOB_CLOSE atomically at creation, and
//! cannot execute any instruction before membership is checked and resumed.
struct Worker
{
    QElapsedTimer elapsed;
#ifdef Q_OS_WIN
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    ~Worker() { close(); }
    void close()
    {
        if (job) CloseHandle(job);
        if (process) CloseHandle(process);
        job = nullptr; process = nullptr; pid = 0;
    }
    bool launch(const QString &executable, const QString &requestPath, QString *error)
    {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.ProcessMemoryLimit = SIZE_T(512)*1024*1024;
        limits.JobMemoryLimit = SIZE_T(768)*1024*1024;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        { *error = QString("Cannot configure mesh Job object (%1).").arg(GetLastError()); close(); return false; }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        HANDLE nullHandle = CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                       &security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (nullHandle == INVALID_HANDLE_VALUE)
        { *error = QString("Cannot create isolated worker IO (%1).").arg(GetLastError()); close(); return false; }
        auto nullGuard = qScopeGuard([&] { CloseHandle(nullHandle); });
        // Attribute value arrays must outlive the attribute-list cleanup.
        HANDLE jobs[]{job};
        HANDLE handles[]{nullHandle};
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr,2,0,&size);
        QByteArray storage(int(size), '\0');
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes,2,0,&size))
        { *error = QString("Cannot initialize worker attributes (%1).").arg(GetLastError()); close(); return false; }
        auto attributeGuard = qScopeGuard([&] { DeleteProcThreadAttributeList(attributes); });
        if (!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,jobs,sizeof(jobs),nullptr,nullptr)
            || !UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr))
        { *error = QString("Atomic worker Job assignment requires supported Windows (%1).").arg(GetLastError()); close(); return false; }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = nullHandle;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION information{};
        // Both strings are controller-owned canonical paths; quotes cannot occur
        // in Windows file names. No shell or caller-supplied executable is used.
        std::wstring command = ("\""+executable+"\" --request \""+requestPath+"\"").toStdWString();
        const std::wstring application = executable.toStdWString();
        if (!CreateProcessW(application.c_str(),command.data(),nullptr,nullptr,TRUE,
                            CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
                            nullptr,nullptr,&startup.StartupInfo,&information))
        { *error = QString("Cannot create mesh worker (%1).").arg(GetLastError()); close(); return false; }
        process = information.hProcess; pid = information.dwProcessId;
        auto threadGuard = qScopeGuard([&] { CloseHandle(information.hThread); });
        BOOL member = FALSE;
        if (!IsProcessInJob(process,job,&member) || !member || ResumeThread(information.hThread) == DWORD(-1))
        {
            *error = QString("Mesh worker containment/resume failed (%1).").arg(GetLastError());
            TerminateJobObject(job,3); TerminateProcess(process,3); WaitForSingleObject(process,5000); close(); return false;
        }
        elapsed.start(); return true;
    }
    bool exited() const
    {
        if (!process || WaitForSingleObject(process,0) != WAIT_OBJECT_0) return false;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        return QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr)
            && accounting.ActiveProcesses == 0;
    }
    bool terminate() { return job && TerminateJobObject(job,3); }
    bool stopAndWait()
    {
        if (!process) return true;
        terminate();
        if (WaitForSingleObject(process,5000) != WAIT_OBJECT_0) return false;
        for (int i=0;i<500;++i) { if (exited()) return true; Sleep(10); }
        return false;
    }
#else
    quint64 pid = 0;
    bool launch(const QString &, const QString &, QString *error)
    { *error = "Contained mesh workers currently require Windows Job-list process creation."; return false; }
    bool exited() const { return true; }
    bool terminate() { return false; }
    bool stopAndWait() { return true; }
#endif
};
}

struct MeshJobController::State
{
    QString workspace, root, ownerId = uuid(), activeId;
    std::unique_ptr<QLockFile> owner;
    std::unique_ptr<Worker> worker;
    QMap<QString,QJsonObject> entries;
    QJsonObject generationPreview, acceptancePreview;
    QTimer *timer = nullptr;
    bool stopped = false;
};

MeshJobController::MeshJobController(Session *value, QObject *parent)
    : QObject(parent), state(std::make_unique<State>()), session(value)
{
    state->timer = new QTimer(this);
    state->timer->setInterval(200);
    connect(state->timer,&QTimer::timeout,this,&MeshJobController::poll);
    state->timer->start();
    if (session)
    {
        connect(session,&Session::documentRevisionChanged,this,[this] { invalidatePreviews(); emit jobsChanged(); });
        connect(session,&Session::runtimeDocumentRemoved,this,[this] { invalidatePreviews(); emit jobsChanged(); });
        connect(session,&Session::modelSelectionChanged,this,[this] { invalidatePreviews(); });
        connect(session,&QObject::destroyed,this,[this] { shutdown(); });
    }
}
MeshJobController::~MeshJobController() { shutdown(); }
void MeshJobController::invalidatePreviews()
{
    const bool hadPreview = !state->generationPreview.isEmpty() || !state->acceptancePreview.isEmpty();
    state->generationPreview = {}; state->acceptancePreview = {};
    if (hadPreview) emit previewsInvalidated();
}

bool MeshJobController::ensureWorkspace(QString *error)
{
    if (QThread::currentThread() != thread() || !session || session->thread() != thread())
    { *error = "Mesh jobs require the live GUI document thread."; return false; }
    const QString workspace = QFileInfo(session->projectWorkspace()).canonicalFilePath();
    if (workspace.isEmpty()) { *error = "Open a project workspace before generating a mesh."; return false; }
    if (workspace == state->workspace && state->owner && !state->stopped) return true;
    shutdown();
    state->stopped = false;
    state->entries.clear(); state->workspace = workspace;
    state->root = QDir(workspace).filePath("mesh-jobs");
    if (!QDir(workspace).mkpath("mesh-jobs") || !canonicalDirectory(state->root))
    { *error = "Mesh storage must be a canonical workspace-local directory without links."; return false; }
    auto owner = std::make_unique<QLockFile>(QDir(state->root).filePath("owner.lock"));
    owner->setStaleLockTime(0);
    if (!owner->tryLock()) { *error = "Another GUI owns this workspace's mesh jobs."; return false; }
    state->owner = std::move(owner);
    if (!QDir(state->root).mkpath("attempts") || !QDir(state->root).mkpath("captures")
        || !canonicalDirectory(QDir(state->root).filePath("attempts"))
        || !canonicalDirectory(QDir(state->root).filePath("captures")))
    { *error = "Cannot create canonical mesh capture/attempt directories."; state->owner.reset(); return false; }
    const QString registryPath = QDir(state->root).filePath("registry.json");
    if (QFileInfo::exists(registryPath))
    {
        const auto registry = readObject(registryPath,4*1024*1024);
        const auto jobs = registry.value("jobs").toArray();
        if (registry.value("schemaVersion") != QJsonValue(1) || !registry.value("jobs").isArray()
            || jobs.size() > 64 || registry.size() != 3 || !registry.value("ownerRuntimeId").isString())
        { *error = "Mesh registry is invalid or exceeds 64 jobs; no jobs were replayed."; state->owner.reset(); return false; }
        for (const auto &value : jobs)
        {
            auto entry = value.toObject();
            const QString id = entry.value("attemptId").toString();
            const auto frozen = entry.value("frozenRequest").toObject();
            if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id
                || frozen.value("attemptId") != QJsonValue(id)
                || frozen.value("jobDirectory") != QJsonValue(QDir(state->root).filePath("attempts/"+id))
                || state->entries.contains(id))
            { *error = "Mesh registry contains an invalid owned attempt; no jobs were replayed."; state->owner.reset(); state->entries.clear(); return false; }
            const QString decisionPath = QDir(state->root).filePath("captures/"+id+"/owner-decision.json");
            if (QFileInfo::exists(decisionPath))
            {
                const auto decision = readObject(decisionPath,4096);
                if (decision.size() != 4 || decision.value("schemaVersion") != QJsonValue(1)
                    || decision.value("attemptId") != QJsonValue(id)
                    || decision.value("requestSha256") != frozen.value("requestSha256")
                    || (decision.value("decision") != QJsonValue("cancel") && decision.value("decision") != QJsonValue("timeout")))
                { entry.insert("status","interrupted_unknown"); entry.insert("terminalDecision","invalid_stop_record"); }
                else entry.insert("terminalDecision",decision.value("decision"));
            }
            const QString decision = entry.value("terminalDecision").toString();
            if (decision == "cancel") entry.insert("status",entry.value("pid").toDouble() == 0 ? "cancelled" : "cancelled_unknown");
            else if (decision == "timeout" || entry.value("code") == QJsonValue("worker_timeout"))
            { entry.insert("status",entry.value("pid").toDouble() == 0 ? "timed_out" : "timed_out_unknown"); entry.insert("terminalDecision","timeout"); }
            else if (!terminal(entry.value("status").toString()))
            { entry.insert("status","interrupted_unknown"); entry.insert("message","Previous GUI owner ended before a durable tree-exit record; no automatic reconciliation or replay."); }
            state->entries.insert(id,entry);
        }
    }
    state->timer->start();
    return persist(error);
}

bool MeshJobController::persist(QString *error)
{
    if (!state->owner) { if (error) *error = "Mesh registry ownership unavailable."; return false; }
    QJsonArray entries;
    for (const auto &entry : state->entries) entries.append(entry);
    return saveObject(QDir(state->root).filePath("registry.json"),
                      {{"schemaVersion",1},{"ownerRuntimeId",state->ownerId},{"jobs",entries}},error);
}

bool MeshJobController::persistStopDecision(const QString &attemptId,const QString &decision,QString *error)
{
    const auto entry = state->entries.value(attemptId);
    const QString capture = QDir(state->root).filePath("captures/"+attemptId);
    if (!state->owner || entry.isEmpty() || !canonicalDirectory(capture))
    { *error = "Cannot durably bind the owner stop decision to its capture."; return false; }
    const QJsonObject record{{"schemaVersion",1},{"attemptId",attemptId},{"decision",decision},
        {"requestSha256",entry.value("frozenRequest").toObject().value("requestSha256")}};
    const QString path = QDir(capture).filePath("owner-decision.json");
    if (QFileInfo::exists(path))
    {
        if (readObject(path,4096) == record) return true;
        *error = "A different or invalid durable stop decision already exists."; return false;
    }
    return saveObject(path,record,error);
}

bool MeshJobController::sourceIsCurrent(const QJsonObject &capture, bool verifyLiveBytes, QString *error)
{
    if (!session || session->projectWorkspace() != state->workspace || ModelCommandHost::isMutationBlocked())
    { *error = "Source workspace is unavailable or editing is blocked."; return false; }
    const auto expected = capture.value("sourceRuntimeStamp").toObject();
    const uint id = session->findModelByCommandIdentity(expected.value("documentId").toString());
    if (id >= session->getNModels() || stamp(session,id) != expected)
    { *error = "Source document changed or closed. The independent candidate is retained."; return false; }
    if (!session->canMutateDocument(id,error) || session->hasDocumentDraft(id))
    { if (error->isEmpty()) *error = "Resolve the pending source draft before approval."; return false; }
    const auto sourceFiles = capture.value("sourceArtifacts").toArray();
    for (const auto &value : sourceFiles)
    {
        const auto artifact = value.toObject();
        if (RMeshJobService::fileSha256(artifact.value("path").toString()) != artifact.value("sha256").toString())
        { *error = "Saved source bytes changed. The independent candidate is retained."; return false; }
    }
    if (verifyLiveBytes)
    {
        const auto current = session->createDocumentSnapshot(id,error);
        if (current.isEmpty() || current.value("instanceRef") != QJsonValue(expected)
            || current.value("modelSha256") != capture.value("sourceModelSha256"))
        { if (error->isEmpty()) *error = "Full source model bytes changed since capture."; return false; }
    }
    return true;
}

QJsonObject MeshJobController::previewGenerate(uint modelID, const QJsonObject &options)
{
    invalidatePreviews();
    QString error;
    if (!ensureWorkspace(&error)) return reply(false,"workspace_unavailable",error);
    if (state->entries.size() >= 64) return reply(false,"registry_full","The retained mesh registry is limited to 64 jobs.");
    const QDir captures(QDir(state->root).filePath("captures"));
    if (captures.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size() >= 64)
        return reply(false,"capture_storage_full","The workspace retains at most 64 mesh capture attempts, including abandoned previews and partial failures. Existing evidence is preserved.");
    if (modelID >= session->getNModels() || ModelCommandHost::isMutationBlocked())
        return reply(false,"source_unavailable","Select one available source document.");
    const auto ref = session->createDocumentSnapshot(modelID,&error);
    if (ref.isEmpty()) return reply(false,"source_capture_failed",error);
    const QString persistentPath = QDir(state->workspace).filePath(ref.value("relativePath").toString());
    const QString sourceHash = RMeshJobService::fileSha256(persistentPath);
    if (sourceHash != ref.value("modelSha256").toString()) return reply(false,"source_capture_failed","Native snapshot hash differs from the catalog capture.");
    RModel detached;
    try { detached.read(persistentPath); }
    catch (const RError &failure) { return reply(false,"source_capture_failed",failure.getMessage()); }
    const auto preview = RMeshCandidateService::preview(detached,options);
    if (!preview.valid) return reply(false,preview.code,preview.message);
    const QString attempt = uuid();
    const QString captureDirectory = QDir(state->root).filePath("captures/"+attempt);
    if (!QDir().mkdir(captureDirectory)) return reply(false,"source_capture_failed","Cannot create exclusive source capture.");
    const QString sourcePath = QDir(captureDirectory).filePath("source.rbm");
    if (!QFile::copy(persistentPath,sourcePath) || RMeshJobService::fileSha256(sourcePath) != sourceHash)
        return reply(false,"source_capture_failed","Frozen source copy failed its full-byte hash check.");
    const auto runtimeStamp = ref.value("instanceRef").toObject();
    const auto document = session->documentProjectContext(modelID);
    const QString contextPath = QDir(captureDirectory).filePath("runtime.json");
    if (!saveObject(contextPath,{{"sourceSnapshotRef",ref},{"sourceRuntimeStamp",runtimeStamp},{"projectDocument",document}},&error))
        return reply(false,"source_capture_failed",error);
#ifdef Q_OS_WIN
    const QString workerName = "range-mesh-worker.exe";
#else
    const QString workerName = "range-mesh-worker";
#endif
    const QString workerPath = QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(workerName)).canonicalFilePath();
    const QString workerHash = RMeshJobService::fileSha256(workerPath);
    if (workerHash.isEmpty()) return reply(false,"worker_unavailable","The fixed range-mesh-worker executable is unavailable beside the GUI.");
    const QJsonObject context{{"authority","gui-captured-document"},{"persistentModelPath",persistentPath},
        {"persistentModelSha256",sourceHash},{"runtimeSnapshotPath",contextPath},
        {"runtimeSnapshotSha256",RMeshJobService::fileSha256(contextPath)},
        {"sourceSnapshotRef",ref},{"sourceRuntimeStamp",runtimeStamp}};
    const QJsonObject request{{"schemaVersion",1},{"mode","closed_surface_tetrahedra"},{"attemptId",attempt},
        {"sourceSnapshotPath",sourcePath},{"sourceSnapshotSha256",sourceHash},{"options",preview.normalizedArguments},
        {"runtimeContext",context},{"workerPath",workerPath},{"workerSha256",workerHash},
        {"jobDirectory",QDir(state->root).filePath("attempts/"+attempt)}};
    QJsonArray sourceFiles;
    if (session->getModel(modelID).getLoadedSourceBinding())
        for (const auto &artifact : session->getModel(modelID).getLoadedSourceBinding()->sourceArtifacts)
            sourceFiles.append(artifact.toJson());
    const QJsonObject capture{{"sourceRuntimeStamp",runtimeStamp},{"sourceModelSha256",sourceHash},
        {"sourceSnapshotRef",ref},{"sourceArtifacts",sourceFiles},
        {"supportedSourceStateSha256",RModelCommandService::stateFingerprint(detached)},
        {"physicalIds",physicalIds(detached)}};
    state->generationPreview = {{"previewId",uuid()},{"attemptId",attempt},{"request",request},{"capture",capture},
        {"quality",preview.report},{"options",preview.normalizedArguments},{"requiresRebinding",true},
        {"bindingState","REQUIRES_REBINDING"},{"sourceName",detached.getName()},
        {"resourceLimits",QJsonObject{{"workerMemoryMiB",512},{"treeMemoryMiB",768},{"wallTimeSeconds",180}}}};
    return reply(true,"preview_ready","Review the captured source and quality options, then explicitly approve generation.",state->generationPreview);
}

QJsonObject MeshJobController::approveGenerate(const QString &previewId)
{
    QString error;
    if (!ensureWorkspace(&error)) return reply(false,"workspace_unavailable",error);
    const auto preview = state->generationPreview;
    state->generationPreview = {};
    if (preview.isEmpty() || preview.value("previewId") != QJsonValue(previewId))
        return reply(false,"preview_unavailable","Preview generation again before approving.");
    if (!sourceIsCurrent(preview.value("capture").toObject(),true,&error)) return reply(false,"source_stale",error);
    const auto prepared = RMeshJobService::prepare(preview.value("request").toObject());
    if (!prepared.valid) return prepared.toJson();
    const QString id = preview.value("attemptId").toString();
    state->entries.insert(id,{{"attemptId",id},{"status","queued"},{"phase","queued"},{"pid",0},
        {"ownerRuntimeId",state->ownerId},{"capture",preview.value("capture")},
        {"frozenRequest",prepared.descriptor.value("frozenRequest")},{"descriptor",prepared.descriptor},
        {"sourceName",preview.value("sourceName")},{"message","Approved frozen job queued."}});
    if (!persist(&error))
    { state->entries[id].insert("status","error"); return reply(false,"registry_write_failed",error,view(id)); }
    emit jobsChanged();
    // Queueing remains cancellable before the first instruction executes.
    QTimer::singleShot(0,this,&MeshJobController::startNext);
    return reply(true,"queued","Approved job queued; source remains unchanged.",view(id));
}

void MeshJobController::startNext()
{
    if (state->stopped || !state->owner || state->worker) return;
    for (auto it=state->entries.begin();it!=state->entries.end();++it)
    {
        if (it->value("status") != QJsonValue("queued")) continue;
        const auto request = it->value("frozenRequest").toObject();
        QString error;
        const auto context = request.value("runtimeContext").toObject();
        const QString expectedWorker = QFileInfo(QDir(QCoreApplication::applicationDirPath())
            .filePath(QFileInfo(request.value("workerPath").toString()).fileName())).canonicalFilePath();
        if (request.value("workerPath").toString() != expectedWorker
            || RMeshJobService::fileSha256(expectedWorker) != request.value("workerSha256").toString()
            || RMeshJobService::fileSha256(request.value("sourceSnapshotPath").toString()) != request.value("sourceSnapshotSha256").toString()
            || RMeshJobService::fileSha256(context.value("runtimeSnapshotPath").toString()) != context.value("runtimeSnapshotSha256").toString()
            || readObject(QDir(request.value("jobDirectory").toString()).filePath("request.json")) != request)
        {
            it->insert("status","error"); it->insert("message","Frozen source, worker, context or request changed before launch.");
            persist(&error); emit jobsChanged(); continue;
        }
        auto worker = std::make_unique<Worker>();
        if (!worker->launch(request.value("workerPath").toString(),QDir(request.value("jobDirectory").toString()).filePath("request.json"),&error))
        { it->insert("status","error"); it->insert("message",error); persist(&error); emit jobsChanged(); continue; }
        state->activeId = it.key(); state->worker = std::move(worker);
        it->insert("status","running"); it->insert("phase","worker_generation_and_verification");
        it->insert("pid",double(state->worker->pid));
        it->insert("message","Contained worker is generating and independently verifying artifacts.");
        if (!persist(&error))
        {
            const bool exited = state->worker->stopAndWait(); state->worker.reset(); state->activeId.clear();
            it->insert("status",exited ? "interrupted" : "interrupted_unknown");
            it->insert("message","Could not durably record worker ownership: "+error);
        }
        emit jobsChanged(); return;
    }
}

void MeshJobController::poll()
{
    if (session && !state->workspace.isEmpty() && session->projectWorkspace() != state->workspace)
    { shutdown(); emit jobsChanged(); return; }
    QString previewError;
    for (const auto &preview : {state->generationPreview,state->acceptancePreview})
        if (!preview.isEmpty() && !sourceIsCurrent(preview.value("capture").toObject(),false,&previewError))
        { invalidatePreviews(); break; }
    if (!state->worker) return;
    if (state->worker->elapsed.isValid() && state->worker->elapsed.elapsed() > 180000
        && state->entries[state->activeId].value("status") == QJsonValue("running"))
    {
        QString error;
        const bool durableDecision = persistStopDecision(state->activeId,"timeout",&error);
        auto &entry = state->entries[state->activeId];
        entry.insert("terminalDecision","timeout");
        entry.insert("status","timing_out"); entry.insert("code","worker_timeout");
        entry.insert("phase","waiting_for_child_tree_exit");
        entry.insert("message","Worker exceeded the 180-second wall-time bound; waiting for tree exit.");
        if (!durableDecision) entry.insert("persistenceError",error);
        const bool durableRegistry = persist(&error);
        if (!durableRegistry) entry.insert("persistenceError",error);
        entry.insert("stopPersistenceUnknown",!durableDecision || !durableRegistry);
        // The resource bound still requires termination if storage is broken.
        if (!state->worker->terminate()) entry.insert("message","Worker timeout termination failed; owner retains its Job until tree exit.");
        emit jobsChanged();
    }
    if (!state->worker->exited()) return;
    const QString id = state->activeId;
    state->worker.reset(); state->activeId.clear();
    auto &entry = state->entries[id];
    QString error;
    if (entry.value("terminalDecision") == QJsonValue("cancel"))
    { entry.insert("status","cancelled"); entry.insert("phase","child_tree_exited"); entry.insert("message","Cancellation confirmed after all owned worker processes exited."); }
    else if (entry.value("terminalDecision") == QJsonValue("timeout"))
    { entry.insert("status","timed_out"); entry.insert("code","worker_timeout"); entry.insert("phase","child_tree_exited"); entry.insert("message","Worker wall-time bound exceeded; owned process tree exit confirmed."); }
    else
    {
        const auto reconciled = RMeshJobService::queryReconcile(entry.value("frozenRequest").toObject());
        entry.insert("status",reconciled.valid ? "candidate_ready" : "error");
        entry.insert("phase","child_tree_exited"); entry.insert("descriptor",reconciled.descriptor);
        entry.insert("message",reconciled.message); entry.insert("code",reconciled.code);
    }
    entry.insert("pid",0);
    if (!persist(&error))
    {
        entry.insert("persistenceError",error); entry.insert("stopPersistenceUnknown",true);
        if (entry.value("terminalDecision") == QJsonValue("cancel")) entry.insert("status","cancelled_unknown");
        else if (entry.value("terminalDecision") == QJsonValue("timeout")) entry.insert("status","timed_out_unknown");
        else entry.insert("status","interrupted_unknown");
    }
    emit jobsChanged(); emit eventPublished(view(id)); startNext();
}

QJsonObject MeshJobController::view(const QString &id) const
{
    const auto entry = state->entries.value(id);
    if (entry.isEmpty()) return {};
    auto result = entry;
    result.remove("frozenRequest");
    result.insert("jobId",id); result.insert("requiresRebinding",true);
    result.insert("bindingState","REQUIRES_REBINDING");
    result.insert("units",QJsonObject{{"length","m"},{"volume","m^3"}});
    result.insert("options",entry.value("frozenRequest").toObject().value("options"));
    result.insert("sourceRuntimeStamp",entry.value("capture").toObject().value("sourceRuntimeStamp"));
    return result;
}

QJsonObject MeshJobController::query(const QString &attemptId)
{
    QString error;
    if (!ensureWorkspace(&error)) return reply(false,"workspace_unavailable",error);
    if (!state->entries.contains(attemptId)) return reply(false,"job_not_found","No owned mesh attempt has this ID.");
    auto &entry = state->entries[attemptId];
    const QString status = entry.value("status").toString();
    if (!stopSuppressed(entry) && (status == "candidate_ready" || status == "interrupted" || status == "error"))
    {
        const auto verified = RMeshJobService::queryReconcile(entry.value("frozenRequest").toObject());
        if (verified.valid)
        { entry.insert("status","candidate_ready"); entry.insert("descriptor",verified.descriptor); entry.insert("message",verified.message); }
        else if (status == "candidate_ready")
        { entry.insert("status","error"); entry.insert("code",verified.code); entry.insert("message",verified.message); }
        if (!persist(&error))
        { entry.insert("persistenceError",error); return reply(false,"registry_write_failed",error,view(attemptId)); }
    }
    auto result = view(attemptId);
    const bool current = sourceIsCurrent(entry.value("capture").toObject(),false,&error);
    result.insert("sourceCurrent",current); result.insert("sourceStatus",current ? "current" : "stale");
    result.insert("sourceMessage",error);
    if (entry.value("status") == QJsonValue("candidate_ready"))
        result.insert("quality",readObject(entry.value("descriptor").toObject().value("reportPath").toString()).value("meshReport"));
    return reply(true,"job_state","Owned mesh job state.",result);
}

QJsonArray MeshJobController::jobs()
{
    QString error;
    if (!ensureWorkspace(&error)) return {};
    QJsonArray result;
    // Listing is bounded metadata; query/acceptance independently revalidate
    // full candidate bytes. Rechecking 64 native models on every UI tick would
    // block the document thread without improving the acceptance boundary.
    for (const auto &id : state->entries.keys())
    {
        auto job = view(id);
        error.clear();
        const bool current = sourceIsCurrent(state->entries.value(id).value("capture").toObject(),false,&error);
        job.insert("sourceCurrent",current); job.insert("sourceStatus",current ? "current" : "stale");
        job.insert("sourceMessage",error); result.append(job);
    }
    return result;
}

QJsonObject MeshJobController::cancel(const QString &attemptId)
{
    QString error;
    if (!ensureWorkspace(&error)) return reply(false,"workspace_unavailable",error);
    if (!state->entries.contains(attemptId)) return reply(false,"job_not_found","No owned mesh attempt has this ID.");
    auto &entry = state->entries[attemptId];
    const QString status = entry.value("status").toString();
    if (status != "queued" && (status != "running" || state->activeId != attemptId || !state->worker))
        return reply(false,"cancel_unavailable","This job is not queued or running.",view(attemptId));
    if (!persistStopDecision(attemptId,"cancel",&error))
        return reply(false,"cancel_persistence_failed","Cancellation was not issued: "+error,view(attemptId));
    entry.insert("terminalDecision","cancel");
    if (status == "queued")
    { entry.insert("status","cancelled"); entry.insert("phase","never_started"); entry.insert("message","Queued job cancelled before worker launch."); }
    else
    {
        entry.insert("status","cancelling"); entry.insert("phase","waiting_for_child_tree_exit");
        entry.insert("message","Cancellation requested; waiting for all owned processes to exit.");
    }
    const bool durableRegistry = persist(&error);
    if (!durableRegistry) { entry.insert("persistenceError",error); entry.insert("stopPersistenceUnknown",true); }
    const bool terminated = status == "queued" || state->worker->terminate();
    state->acceptancePreview = {}; emit jobsChanged();
    if (!durableRegistry) return reply(false,"cancel_registry_unknown","Stop decision is durable, but registry persistence failed: "+error,view(attemptId));
    if (!terminated) return reply(false,"cancel_failed","Stop intent is durable; Job termination failed and owner retains containment until tree exit.",view(attemptId));
    return reply(true,entry.value("status").toString(),entry.value("message").toString(),view(attemptId));
}

QJsonObject MeshJobController::previewAccept(const QString &attemptId)
{
    state->acceptancePreview = {};
    const auto queried = query(attemptId);
    if (!queried.value("valid").toBool()) return queried;
    const auto &entry = state->entries[attemptId];
    if (entry.value("status") != QJsonValue("candidate_ready")) return reply(false,"candidate_unavailable","Require a fully verified candidate_ready receipt.");
    QString error;
    if (!sourceIsCurrent(entry.value("capture").toObject(),true,&error)) return reply(false,"source_stale",error);
    const auto verified = RMeshJobService::verifyCandidate(entry.value("frozenRequest").toObject());
    if (!verified.valid) return verified.toJson();
    state->acceptancePreview = {{"previewId",uuid()},{"attemptId",attemptId},{"capture",entry.value("capture")},
        {"descriptor",verified.descriptor},{"acceptanceMode","new_document_from_current_source"},
        {"requiresRebinding",true},{"bindingState","REQUIRES_REBINDING"},
        {"sourcePhysicalIds",entry.value("capture").toObject().value("physicalIds")},
        {"candidatePhysicalIds",physicalIds(verified.candidate)},
        {"quality",queried.value("result").toObject().value("quality")}};
    return reply(true,"acceptance_preview_ready","Review this verified candidate. Approval creates a new unbound document and preserves the source.",state->acceptancePreview);
}

QJsonObject MeshJobController::approveAccept(const QString &previewId)
{
    QString error;
    if (!ensureWorkspace(&error)) return reply(false,"workspace_unavailable",error);
    const auto preview = state->acceptancePreview; state->acceptancePreview = {};
    if (preview.isEmpty() || preview.value("previewId") != QJsonValue(previewId))
        return reply(false,"preview_unavailable","Preview acceptance again before approving.");
    const QString id = preview.value("attemptId").toString();
    auto found = state->entries.find(id);
    if (found == state->entries.end() || found->value("status") != QJsonValue("candidate_ready"))
        return reply(false,"candidate_unavailable","Candidate is no longer ready for acceptance.");
    if (!sourceIsCurrent(preview.value("capture").toObject(),true,&error)) return reply(false,"source_stale",error);
    const auto verified = RMeshJobService::verifyCandidate(found->value("frozenRequest").toObject());
    if (!verified.valid) return verified.toJson();
    if (verified.descriptor != preview.value("descriptor").toObject())
        return reply(false,"candidate_changed","Candidate/report/source/worker identity changed after acceptance preview.");
    // Persist the intent before Session publication. A crash in this interval
    // leaves an unknown outcome; restarting cannot repeat acceptance.
    found->insert("status","accepting_unknown"); found->insert("message","Explicit new-document acceptance started; never automatically replayed.");
    if (!persist(&error)) { found->insert("status","candidate_ready"); return reply(false,"registry_write_failed",error); }
    const auto capture = preview.value("capture").toObject();
    const auto expected = capture.value("sourceRuntimeStamp").toObject();
    const QString originalWorkspace = state->workspace;
    const QJsonObject originalEntry = found.value();
    const QString requestHash = originalEntry.value("frozenRequest").toObject().value("requestSha256").toString();
    const auto sameAttempt = [this,&id,&originalWorkspace,&requestHash] {
        const auto entry = state->entries.constFind(id);
        return state->workspace == originalWorkspace && state->owner && entry != state->entries.constEnd()
            && entry->value("frozenRequest").toObject().value("requestSha256").toString() == requestHash;
    };
    const auto unknown = [this,&id,&sameAttempt,&originalEntry](const QString &reason,const QString &documentId) {
        auto result = originalEntry; result.remove("frozenRequest");
        result.insert("status","acceptance_unknown"); result.insert("createdDocumentId",documentId);
        result.insert("message",reason);
        // Never insert an old attempt into a new workspace after a callback.
        // Its original durable accepting_unknown intent already forbids replay.
        if (sameAttempt())
        {
            auto entry = state->entries.find(id);
            entry->insert("status","acceptance_unknown"); entry->insert("createdDocumentId",documentId);
            entry->insert("message",reason); QString persistenceError;
            if (!persist(&persistenceError)) result.insert("persistenceError",persistenceError);
        }
        return reply(false,"acceptance_unknown",reason,result);
    };
    uint newModelId = 0;
    QString createdDocumentId;
    bool documentMayExist = false;
    if (!session->appendMeshCandidate(expected.value("documentId").toString(),expected.value("sessionEpoch").toString(),
        quint64(expected.value("documentRevision").toDouble()),capture.value("supportedSourceStateSha256").toString(),
        verified.candidate,&newModelId,&error,&createdDocumentId,&documentMayExist))
    {
        const QString appendError = error;
        if (documentMayExist || !sameAttempt() || !session || session->projectWorkspace() != originalWorkspace)
            return unknown("Publication may have occurred; do not replay acceptance: "+appendError,createdDocumentId);
        auto entry = state->entries.find(id);
        entry->insert("status","candidate_ready");
        entry->insert("message","New-document append refused: "+appendError);
        if (!persist(&error)) return reply(false,"registry_write_failed",error,view(id));
        return reply(false,"acceptance_refused",appendError,view(id));
    }
    if (!sameAttempt() || !session || session->projectWorkspace() != originalWorkspace || createdDocumentId.isEmpty())
        return unknown("Publication callbacks changed the workspace or owned attempt; the original acceptance intent is retained.",createdDocumentId);
    // Resolve the returned immutable UUID after modelAdded callbacks, rather
    // than trusting an index that can now identify a different document.
    newModelId = session->findModelByCommandIdentity(createdDocumentId);
    if (newModelId >= session->getNModels()) return unknown("Publication callbacks removed the new candidate document.",createdDocumentId);
    const auto acceptedStamp = stamp(session,newModelId);
    if (acceptedStamp.value("documentId") != QJsonValue(createdDocumentId)
        || acceptedStamp.value("sessionEpoch") != expected.value("sessionEpoch")
        || acceptedStamp.value("documentRevision") != QJsonValue(0)
        || session->hasDocumentDraft(newModelId) || session->getModel(newModelId).getLoadedSourceBinding()
        || !sourceIsCurrent(capture,true,&error))
        return unknown("Post-publication document identity, draft, provenance or source proof changed: "+error,createdDocumentId);
    const auto acceptedSnapshot = session->createDocumentSnapshot(newModelId,&error);
    if (acceptedSnapshot.isEmpty() || acceptedSnapshot.value("instanceRef") != QJsonValue(acceptedStamp)
        || acceptedSnapshot.value("modelSha256") != verified.descriptor.value("candidateFileSha256")
        || !sameAttempt() || session->projectWorkspace() != originalWorkspace
        || !sourceIsCurrent(capture,true,&error))
        return unknown("Full accepted native bytes or source proof changed during publication: "+error,createdDocumentId);
    auto entry = state->entries.find(id);
    entry->insert("status","accepted"); entry->insert("acceptedRuntimeStamp",acceptedStamp);
    entry->insert("createdDocumentId",createdDocumentId);
    entry->insert("acceptedModelSha256",acceptedSnapshot.value("modelSha256"));
    entry->insert("message","New unbound document created; explicit static rebinding is required before FE.");
    if (!persist(&error))
    { emit jobsChanged(); return reply(false,"accepted_registry_unknown","New document exists but acceptance receipt persistence failed: "+error,view(id)); }
    invalidatePreviews(); emit jobsChanged(); emit candidateAccepted(id,acceptedStamp.value("documentId").toString());
    return reply(true,"accepted","New unbound document created; source and results preserved.",view(id));
}

void MeshJobController::shutdown()
{
    invalidatePreviews();
    state->stopped = true;
    if (state->timer) state->timer->stop();
    QString error;
    if (state->worker)
    {
        const bool exited = state->worker->stopAndWait();
        auto &entry = state->entries[state->activeId];
        const QString decision = entry.value("terminalDecision").toString();
        entry.insert("status",decision == "cancel" ? (exited ? "cancelled" : "cancelled_unknown")
            : decision == "timeout" ? (exited ? "timed_out" : "timed_out_unknown") : (exited ? "interrupted" : "interrupted_unknown"));
        entry.insert("phase",exited ? "child_tree_exited" : "child_tree_exit_unconfirmed");
        entry.insert("message","GUI owner closed. Artifacts retained; this attempt will never be replayed.");
        if (exited) entry.insert("pid",0);
        state->worker.reset(); state->activeId.clear();
    }
    for (auto it=state->entries.begin();it!=state->entries.end();++it)
        if (it->value("status") == QJsonValue("queued"))
        { it->insert("status","interrupted"); it->insert("message","GUI owner closed before launch; no automatic replay."); }
    if (state->owner) persist(&error);
    state->owner.reset();
}
