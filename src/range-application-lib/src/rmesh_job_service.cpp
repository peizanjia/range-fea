#include <rmesh_job_service.h>
#include <rmesh_candidate_service.h>
#include <rgeometry_candidate_service.h>
#include <rml_file_io.h>
#include <rml_file.h>
#include <rbl_error.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <stdexcept>

namespace
{
constexpr qint64 modelLimit=32*1024*1024;
constexpr qint64 jsonLimit=1024*1024;
QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
QByteArray jsonBytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
bool exact(const QJsonObject &object,const QStringList &keys)
{
    if (object.size()!=keys.size()) return false;
    for (const auto &key:keys) if (!object.contains(key)) return false;
    return true;
}
bool sha(const QJsonValue &value)
{
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
bool boundedId(const QJsonValue &value)
{
    static const QRegularExpression pattern("^[A-Za-z0-9_.:-]{1,128}$");
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
bool uuid(const QString &value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces)==value.toLower(); }
bool regular(const QString &path,qint64 limit)
{
    const QFileInfo file(path);
    return file.isAbsolute() && file.isFile() && !file.isSymLink() && file.size()>0 && file.size()<=limit
        && QDir::cleanPath(file.absoluteFilePath())==QDir::cleanPath(file.canonicalFilePath());
}
bool canonicalDirectory(const QString &path)
{
    const QFileInfo file(path);
    return file.isAbsolute() && file.isDir() && !file.isSymLink()
        && QDir::cleanPath(file.absoluteFilePath())==QDir::cleanPath(file.canonicalFilePath());
}
QByteArray readBytes(const QString &path,qint64 limit)
{
    if (!regular(path,limit)) throw std::runtime_error("Require bounded canonical regular file without links.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open frozen input.");
    const QByteArray bytes=file.read(limit+1);
    if (bytes.size()!=file.size() || bytes.size()>limit || file.error()!=QFileDevice::NoError)
        throw std::runtime_error("Input read failed or exceeded its budget.");
    return bytes;
}
QJsonObject readObject(const QString &path)
{
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(readBytes(path,jsonLimit),&error);
    if (error.error!=QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Require one JSON object.");
    return document.object();
}
void saveNew(const QString &path,const QByteArray &bytes)
{
    if (QFileInfo::exists(path)) throw std::runtime_error("Refuse to overwrite a mesh job artifact.");
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
        throw std::runtime_error("Atomic mesh job write failed.");
}
QString jobPath(const QJsonObject &request,const QString &name)
{
    return QDir(request.value("jobDirectory").toString()).filePath(name);
}
RMeshJobService::Result failure(const QString &code,const QString &message,const QJsonObject &request={})
{
    RMeshJobService::Result result; result.code=code; result.message=message;
    result.descriptor={{"schemaVersion",1},{"status",code},{"attemptId",request.value("attemptId")},
        {"requestSha256",request.value("requestSha256")},{"accepted",false}};
    return result;
}
RModel loadCapturedModel(const QByteArray &bytes,const QString &extension)
{
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("Cannot create private model reader staging.");
    const QString path=directory.filePath("captured."+extension);
    saveNew(path,bytes);
    if (extension!=RModel::getDefaultFileExtension(true) && extension!=RModel::getDefaultFileExtension(false))
        throw std::runtime_error("Source must be a native .rbm or .ram MODEL.");
    const bool binary=extension==RModel::getDefaultFileExtension(true);
    RFile input(path,binary ? RFile::BINARY : RFile::ASCII);
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read MODEL header.");
    RFileHeader header;
    if (binary) RFileIO::readBinary(input,header); else RFileIO::readAscii(input,header);
    if (header.getType()!=R_FILE_TYPE_MODEL) throw std::runtime_error("Only actual MODEL records are allowed; LINK records are forbidden.");
    input.close();
    RModel model; model.read(path); return model;
}
bool instance(const QJsonObject &object)
{
    const double revision=object.value("documentRevision").toDouble(-1);
    return exact(object,{"documentId","sessionEpoch","documentRevision"})
        && boundedId(object.value("documentId")) && boundedId(object.value("sessionEpoch"))
        && object.value("documentRevision").isDouble() && std::isfinite(revision)
        && revision>=0 && revision<=9007199254740991.0 && std::floor(revision)==revision;
}
bool contextValid(const QJsonObject &context,const QString &sourceSha)
{
    if (!exact(context,{"authority","persistentModelPath","persistentModelSha256","runtimeSnapshotPath",
                        "runtimeSnapshotSha256","sourceSnapshotRef","sourceRuntimeStamp"})) return false;
    if (!sha(context.value("persistentModelSha256")) || !sha(context.value("runtimeSnapshotSha256"))) return false;
    const auto ref=context.value("sourceSnapshotRef").toObject();
    if (context.value("authority")==QJsonValue("file-owner"))
        return exact(ref,{"sourceId","sourceRevision","modelSha256","instanceRef"})
            && uuid(ref.value("sourceId").toString()) && boundedId(ref.value("sourceRevision"))
            && ref.value("modelSha256")==QJsonValue(sourceSha) && ref.value("instanceRef").isNull()
            && context.value("sourceRuntimeStamp").isNull();
    if (context.value("authority")!=QJsonValue("gui-captured-document")) return false;
    const QStringList allowed{"id","projectId","persistentDocumentId","sourceRevision","modelSha256","relativePath","instanceRef"};
    for (const auto &key:ref.keys()) if (!allowed.contains(key)) return false;
    for (const auto &key:{"projectId","persistentDocumentId","sourceRevision"}) if (!boundedId(ref.value(key))) return false;
    if (ref.contains("id") && !boundedId(ref.value("id"))) return false;
    if (ref.contains("relativePath") && (!ref.value("relativePath").isString() || ref.value("relativePath").toString().size()>1024)) return false;
    return ref.value("modelSha256")==QJsonValue(sourceSha) && instance(ref.value("instanceRef").toObject())
        && instance(context.value("sourceRuntimeStamp").toObject())
        && ref.value("instanceRef")==context.value("sourceRuntimeStamp");
}
QString inputsUnchanged(const QJsonObject &request)
{
    if (RMeshJobService::fileSha256(request.value("sourceSnapshotPath").toString())!=request.value("sourceSnapshotSha256").toString()) return "source_changed";
    if (RMeshJobService::fileSha256(request.value("workerPath").toString())!=request.value("workerSha256").toString()) return "worker_changed";
    const auto context=request.value("runtimeContext").toObject();
    if (!regular(context.value("persistentModelPath").toString(),modelLimit)
        || RMeshJobService::fileSha256(context.value("persistentModelPath").toString())!=context.value("persistentModelSha256").toString()) return "persistent_source_changed";
    if (!regular(context.value("runtimeSnapshotPath").toString(),jsonLimit)
        || RMeshJobService::fileSha256(context.value("runtimeSnapshotPath").toString())!=context.value("runtimeSnapshotSha256").toString()) return "runtime_context_changed";
    return {};
}
QString frozenValid(const QJsonObject &request)
{
    if (!exact(request,{"schemaVersion","mode","attemptId","sourceSnapshotPath","sourceSnapshotSha256","options",
        "runtimeContext","workerPath","workerSha256","jobDirectory","optionsSha256","runtimeContextSha256",
        "sourceGeometrySha256","requestSha256"}) || request.value("schemaVersion")!=QJsonValue(1)
        || request.value("mode")!=QJsonValue("closed_surface_tetrahedra") || !uuid(request.value("attemptId").toString())
        || !request.value("options").isObject() || !request.value("runtimeContext").isObject()) return "invalid_request";
    for (const auto &key:{"sourceSnapshotSha256","workerSha256","sourceGeometrySha256","optionsSha256","runtimeContextSha256","requestSha256"})
        if (!sha(request.value(key))) return "invalid_request";
    QJsonObject forHash=request; forHash.remove("requestSha256");
    if (RMeshJobService::objectSha256(forHash)!=request.value("requestSha256").toString()
        || RMeshJobService::objectSha256(request.value("options").toObject())!=request.value("optionsSha256").toString()
        || RMeshJobService::objectSha256(request.value("runtimeContext").toObject())!=request.value("runtimeContextSha256").toString()) return "request_hash_mismatch";
    if (!contextValid(request.value("runtimeContext").toObject(),request.value("sourceSnapshotSha256").toString())) return "invalid_runtime_context";
    const QString directory=request.value("jobDirectory").toString();
    if (!canonicalDirectory(directory) || QFileInfo(directory).fileName()!=request.value("attemptId").toString()) return "invalid_job_directory";
    if (!regular(request.value("sourceSnapshotPath").toString(),modelLimit)
        || !regular(request.value("workerPath").toString(),256*1024*1024)) return "invalid_input_path";
    for (const auto &path:{request.value("sourceSnapshotPath").toString(),request.value("workerPath").toString(),
         request.value("runtimeContext").toObject().value("persistentModelPath").toString(),
         request.value("runtimeContext").toObject().value("runtimeSnapshotPath").toString()})
        if (path.startsWith(directory+"/",Qt::CaseInsensitive) || path.startsWith(directory+"\\",Qt::CaseInsensitive)) return "input_inside_job_directory";
    return inputsUnchanged(request);
}
RModel frozenSource(const QJsonObject &request)
{
    const QString path=request.value("sourceSnapshotPath").toString();
    const QByteArray bytes=readBytes(path,modelLimit);
    if (digest(bytes)!=request.value("sourceSnapshotSha256").toString()) throw std::runtime_error("Captured source bytes changed.");
    RModel model=loadCapturedModel(bytes,QFileInfo(path).suffix());
    if (RGeometryCandidateService::geometryFingerprint(model)!=request.value("sourceGeometrySha256").toString())
        throw std::runtime_error("Frozen source geometry changed.");
    const auto preview=RMeshCandidateService::preview(model,request.value("options").toObject());
    if (!preview.valid || preview.normalizedArguments!=request.value("options").toObject()) throw std::runtime_error("Frozen options are not valid normalized bounded options.");
    return model;
}
RMeshJobService::Result checkArtifacts(const QJsonObject &request,const QJsonObject &receipt)
{
    if (!exact(receipt,{"schemaVersion","attemptId","requestSha256","status","accepted","requiresRebinding",
        "candidatePath","candidateFileSha256","candidateGeometrySha256","reportPath","reportFileSha256","receiptSha256"})
        || receipt.value("schemaVersion")!=QJsonValue(1) || receipt.value("status")!=QJsonValue("candidate_ready")
        || receipt.value("accepted")!=QJsonValue(false) || receipt.value("requiresRebinding")!=QJsonValue(true)
        || receipt.value("attemptId")!=request.value("attemptId") || receipt.value("requestSha256")!=request.value("requestSha256")
        || receipt.value("candidatePath")!=QJsonValue(jobPath(request,"candidate.rbm"))
        || receipt.value("reportPath")!=QJsonValue(jobPath(request,"report.json"))) return failure("invalid_receipt","Receipt identity, state or output paths differ.",request);
    QJsonObject receiptHash=receipt; receiptHash.remove("receiptSha256");
    if (RMeshJobService::objectSha256(receiptHash)!=receipt.value("receiptSha256").toString()) return failure("receipt_hash_mismatch","Receipt contents changed.",request);
    const auto candidateBytes=readBytes(jobPath(request,"candidate.rbm"),modelLimit);
    const auto reportBytes=readBytes(jobPath(request,"report.json"),jsonLimit);
    if (digest(candidateBytes)!=receipt.value("candidateFileSha256").toString()
        || digest(reportBytes)!=receipt.value("reportFileSha256").toString()) return failure("artifact_hash_mismatch","Candidate or report bytes changed.",request);
    QJsonParseError parseError;
    const auto reportDocument=QJsonDocument::fromJson(reportBytes,&parseError);
    if (parseError.error!=QJsonParseError::NoError || !reportDocument.isObject()) return failure("invalid_report","Malformed report.",request);
    const auto report=reportDocument.object();
    if (!exact(report,{"schemaVersion","attemptId","requestSha256","candidateFileSha256","candidateGeometrySha256","requiresRebinding","bindingState","meshReport"})
        || report.value("schemaVersion")!=QJsonValue(1) || report.value("attemptId")!=request.value("attemptId")
        || report.value("requestSha256")!=request.value("requestSha256")
        || report.value("candidateFileSha256")!=receipt.value("candidateFileSha256")
        || report.value("candidateGeometrySha256")!=receipt.value("candidateGeometrySha256")
        || report.value("requiresRebinding")!=QJsonValue(true) || report.value("bindingState")!=QJsonValue("REQUIRES_REBINDING"))
        return failure("invalid_report","Report is not bound to this frozen job.",request);
    const RModel source=frozenSource(request);
    const RModel candidate=loadCapturedModel(candidateBytes,RModel::getDefaultFileExtension(true));
    const auto verified=RMeshCandidateService::verify(source,candidate,request.value("options").toObject());
    if (!verified.valid) return failure(verified.code,verified.message,request);
    if (verified.postGeometrySha256!=receipt.value("candidateGeometrySha256").toString()
        || verified.report!=report.value("meshReport").toObject()) return failure("quality_report_mismatch","Fresh verification disagrees with recorded geometry or quality report.",request);
    const QString changed=inputsUnchanged(request);
    if (!changed.isEmpty() || RMeshJobService::fileSha256(jobPath(request,"candidate.rbm"))!=digest(candidateBytes)
        || RMeshJobService::fileSha256(jobPath(request,"report.json"))!=digest(reportBytes)
        || readObject(jobPath(request,"request.json"))!=request)
        return failure(changed.isEmpty() ? "artifact_changed" : changed,"Files changed during verification.",request);
    RMeshJobService::Result result; result.valid=true; result.code="candidate_ready";
    result.message="Detached artifacts verified; acceptance and explicit rebinding remain required.";
    result.candidate=candidate; result.descriptor=receipt;
    result.descriptor.insert("qualityReportSha256",verified.report.value("reportSha256"));
    result.descriptor.insert("sourceGeometrySha256",request.value("sourceGeometrySha256"));
    return result;
}
}

QJsonObject RMeshJobService::Result::toJson() const
{
    return {{"valid",valid},{"code",code},{"message",message},{"descriptor",descriptor}};
}
QString RMeshJobService::objectSha256(const QJsonObject &object) { return digest(jsonBytes(object)); }
QString RMeshJobService::fileSha256(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) return {};
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray bytes=file.read(1024*1024);
        if (bytes.isEmpty() && file.error()!=QFileDevice::NoError) return {};
        hash.addData(bytes);
    }
    return QString::fromLatin1(hash.result().toHex());
}

RMeshJobService::Result RMeshJobService::prepare(const QJsonObject &request)
{
    if (!exact(request,{"schemaVersion","mode","attemptId","sourceSnapshotPath","sourceSnapshotSha256","options",
        "runtimeContext","workerPath","workerSha256","jobDirectory"}) || request.value("schemaVersion")!=QJsonValue(1)
        || request.value("mode")!=QJsonValue("closed_surface_tetrahedra") || !uuid(request.value("attemptId").toString())
        || !request.value("options").isObject() || !request.value("runtimeContext").isObject()
        || jsonBytes(request).size()>65536 || !sha(request.value("sourceSnapshotSha256")) || !sha(request.value("workerSha256")))
        return failure("invalid_request","Require the exact bounded mesh job request schema.",request);
    const QString directory=request.value("jobDirectory").toString();
    if (!QFileInfo(directory).isAbsolute() || QDir::cleanPath(directory)!=directory
        || QFileInfo(directory).fileName()!=request.value("attemptId").toString()
        || !canonicalDirectory(QFileInfo(directory).absolutePath()) || QFileInfo::exists(directory))
        return failure("invalid_job_directory","Job directory must be a new UUID directory under a canonical existing parent.",request);
    if (!contextValid(request.value("runtimeContext").toObject(),request.value("sourceSnapshotSha256").toString()))
        return failure("invalid_runtime_context","Runtime identity chain does not match the source capture.",request);
    if (!regular(request.value("sourceSnapshotPath").toString(),modelLimit)
        || !regular(request.value("workerPath").toString(),256*1024*1024)) return failure("invalid_input_path","Require bounded actual files.",request);
    const auto context=request.value("runtimeContext").toObject();
    if (context.value("authority")==QJsonValue("gui-captured-document")
        && request.value("sourceSnapshotPath")==context.value("persistentModelPath"))
        return failure("invalid_source_capture","GUI jobs require a separate frozen source capture.",request);
    const QString changed=inputsUnchanged(request);
    if (!changed.isEmpty()) return failure(changed,"Expected input hash differs.",request);
    try
    {
        readObject(context.value("runtimeSnapshotPath").toString());
        const QString sourcePath=request.value("sourceSnapshotPath").toString();
        const auto bytes=readBytes(sourcePath,modelLimit);
        if (digest(bytes)!=request.value("sourceSnapshotSha256").toString()) return failure("source_changed","Captured source bytes changed.",request);
        const auto source=loadCapturedModel(bytes,QFileInfo(sourcePath).suffix());
        const auto preview=RMeshCandidateService::preview(source,request.value("options").toObject());
        if (!preview.valid) return failure(preview.code,preview.message,request);
        QJsonObject frozen=request; frozen.insert("options",preview.normalizedArguments);
        frozen.insert("optionsSha256",objectSha256(preview.normalizedArguments));
        frozen.insert("runtimeContextSha256",objectSha256(context)); frozen.insert("sourceGeometrySha256",preview.sourceGeometrySha256);
        frozen.insert("requestSha256",objectSha256(frozen));
        const QString after=inputsUnchanged(frozen);
        if (!after.isEmpty()) return failure(after,"Inputs changed during preparation.",frozen);
        if (!QDir(QFileInfo(directory).absolutePath()).mkdir(QFileInfo(directory).fileName())) return failure("job_directory_conflict","Could not exclusively create job directory.",frozen);
        saveNew(jobPath(frozen,"request.json"),jsonBytes(frozen));
        Result result; result.valid=true; result.code="prepared"; result.message="Frozen request ready for the fixed worker.";
        result.descriptor={{"schemaVersion",1},{"status","prepared"},{"attemptId",frozen.value("attemptId")},
            {"requestSha256",frozen.value("requestSha256")},{"requestPath",jobPath(frozen,"request.json")},
            {"frozenRequest",frozen},{"requiresRebinding",true},{"accepted",false}};
        return result;
    }
    catch (const RError &error) { return failure("prepare_failed",error.getMessage(),request); }
    catch (const std::exception &error) { return failure("prepare_failed",QString::fromUtf8(error.what()),request); }
}

RMeshJobService::Result RMeshJobService::runWorker(const QJsonObject &request,const QString &actualWorkerPath,const std::atomic_bool *cancel)
{
    const QString invalid=frozenValid(request);
    if (!invalid.isEmpty()) return failure(invalid,"Frozen request or inputs failed validation.",request);
    if (QFileInfo(actualWorkerPath).canonicalFilePath()!=request.value("workerPath").toString()
        || fileSha256(actualWorkerPath)!=request.value("workerSha256").toString())
        return failure("worker_identity_mismatch","The running worker differs from the trusted frozen binary.",request);
    try
    {
        if (readObject(jobPath(request,"request.json"))!=request) return failure("request_file_mismatch","Job request file differs.",request);
        if (QFileInfo::exists(jobPath(request,"intent.json"))) return queryReconcile(request);
        QLockFile lock(jobPath(request,"worker.lock")); lock.setStaleLockTime(0);
        if (!lock.tryLock()) return failure("job_running_or_unknown","Another worker holds this attempt.",request);
        const QStringList allowed{"request.json","worker.lock"};
        for (const auto &name:QDir(request.value("jobDirectory").toString()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System))
            if (!allowed.contains(name)) return failure("output_conflict","Job directory has unexpected files; refuse to overwrite or regenerate.",request);
        saveNew(jobPath(request,"intent.json"),jsonBytes(QJsonObject{{"schemaVersion",1},{"attemptId",request.value("attemptId")},
            {"requestSha256",request.value("requestSha256")},{"status","running"}}));
        const auto terminate=[&](const QString &code,const QString &message) {
            QJsonObject receipt{{"schemaVersion",1},{"attemptId",request.value("attemptId")},{"requestSha256",request.value("requestSha256")},
                {"status",code=="cancelled" ? "cancelled" : "failed"},{"code",code},{"message",message},{"accepted",false}};
            receipt.insert("receiptSha256",objectSha256(receipt));
            saveNew(jobPath(request,"receipt.json"),jsonBytes(receipt));
            auto result=failure(code,message,request); result.descriptor=receipt; return result;
        };
        if (cancel && cancel->load()) return terminate("cancelled","Mesh job cancelled before generation.");
        const auto source=frozenSource(request);
        const auto generated=RMeshCandidateService::generate(source,request.value("options").toObject(),cancel);
        if (!generated.valid) return terminate(generated.code,generated.message);
        QTemporaryDir stage(QDir(request.value("jobDirectory").toString()).filePath(".stage-XXXXXX"));
        if (!stage.isValid()) return terminate("staging_failed","Cannot stage owned candidate output.");
        const QString candidateStage=stage.filePath("candidate.rbm");
        if (generated.candidate.write(candidateStage,false)!=candidateStage) return terminate("staging_failed","Native writer changed the staged output path.");
        const auto candidateBytes=readBytes(candidateStage,modelLimit);
        const auto actualCandidate=loadCapturedModel(candidateBytes,RModel::getDefaultFileExtension(true));
        const auto verified=RMeshCandidateService::verify(source,actualCandidate,request.value("options").toObject(),cancel);
        if (!verified.valid) return terminate(verified.code,verified.message);
        const QString after=inputsUnchanged(request);
        if (!after.isEmpty()) return terminate(after,"Frozen inputs changed during generation.");
        if (cancel && cancel->load()) return terminate("cancelled","Mesh job cancelled before artifact publication.");
        const QString candidateHash=digest(candidateBytes);
        const QJsonObject report{{"schemaVersion",1},{"attemptId",request.value("attemptId")},{"requestSha256",request.value("requestSha256")},
            {"candidateFileSha256",candidateHash},{"candidateGeometrySha256",verified.postGeometrySha256},
            {"requiresRebinding",true},{"bindingState","REQUIRES_REBINDING"},{"meshReport",verified.report}};
        const auto reportBytes=jsonBytes(report);
        // Each artifact is atomic; only the final receipt makes a complete attempt visible.
        saveNew(jobPath(request,"candidate.rbm"),candidateBytes);
        saveNew(jobPath(request,"report.json"),reportBytes);
        QJsonObject receipt{{"schemaVersion",1},{"attemptId",request.value("attemptId")},{"requestSha256",request.value("requestSha256")},
            {"status","candidate_ready"},{"accepted",false},{"requiresRebinding",true},
            {"candidatePath",jobPath(request,"candidate.rbm")},{"candidateFileSha256",candidateHash},
            {"candidateGeometrySha256",verified.postGeometrySha256},{"reportPath",jobPath(request,"report.json")},
            {"reportFileSha256",digest(reportBytes)}};
        receipt.insert("receiptSha256",objectSha256(receipt));
        const auto finalCheck=checkArtifacts(request,receipt);
        if (!finalCheck.valid) return terminate(finalCheck.code,finalCheck.message);
        if (cancel && cancel->load()) return terminate("cancelled","Mesh job cancelled before durable receipt.");
        saveNew(jobPath(request,"receipt.json"),jsonBytes(receipt));
        return verifyCandidate(request);
    }
    catch (const RError &error) { return failure("worker_failed",error.getMessage(),request); }
    catch (const std::exception &error) { return failure("worker_failed",QString::fromUtf8(error.what()),request); }
    catch (...) { return failure("worker_failed","Worker failed without a diagnostic.",request); }
}

RMeshJobService::Result RMeshJobService::verifyCandidate(const QJsonObject &request)
{
    const QString invalid=frozenValid(request);
    if (!invalid.isEmpty()) return failure(invalid,"Frozen request or current inputs failed validation.",request);
    try
    {
        if (readObject(jobPath(request,"request.json"))!=request) return failure("request_file_mismatch","Job request file differs.",request);
        const auto receiptBytes=readBytes(jobPath(request,"receipt.json"),jsonLimit);
        QJsonParseError parseError;
        const auto receiptDocument=QJsonDocument::fromJson(receiptBytes,&parseError);
        if (parseError.error!=QJsonParseError::NoError || !receiptDocument.isObject()) return failure("invalid_receipt","Malformed receipt.",request);
        auto result=checkArtifacts(request,receiptDocument.object());
        if (result.valid && fileSha256(jobPath(request,"receipt.json"))!=digest(receiptBytes))
            return failure("receipt_changed","Receipt changed during verification.",request);
        return result;
    }
    catch (const RError &error) { return failure("candidate_verification_failed",error.getMessage(),request); }
    catch (const std::exception &error) { return failure("candidate_verification_failed",QString::fromUtf8(error.what()),request); }
    catch (...) { return failure("candidate_verification_failed","Verification failed without a diagnostic.",request); }
}

RMeshJobService::Result RMeshJobService::queryReconcile(const QJsonObject &request)
{
    const QString invalid=frozenValid(request);
    if (!invalid.isEmpty()) return failure(invalid,"Frozen request or current inputs failed validation.",request);
    if (!QFileInfo::exists(jobPath(request,"receipt.json"))) return failure("unknown","No complete durable receipt; interruption or partial outputs cannot be promoted.",request);
    try
    {
        const auto receipt=readObject(jobPath(request,"receipt.json"));
        if (receipt.value("status")==QJsonValue("failed") || receipt.value("status")==QJsonValue("cancelled"))
        {
            QJsonObject forHash=receipt; forHash.remove("receiptSha256");
            if (!exact(receipt,{"schemaVersion","attemptId","requestSha256","status","code","message","accepted","receiptSha256"})
                || receipt.value("schemaVersion")!=QJsonValue(1) || receipt.value("attemptId")!=request.value("attemptId")
                || receipt.value("requestSha256")!=request.value("requestSha256") || receipt.value("accepted")!=QJsonValue(false)
                || objectSha256(forHash)!=receipt.value("receiptSha256").toString()) return failure("invalid_receipt","Failure receipt changed.",request);
            auto result=failure(receipt.value("code").toString(),receipt.value("message").toString(),request); result.descriptor=receipt; return result;
        }
        return verifyCandidate(request);
    }
    catch (const std::exception &error) { return failure("unknown",QString::fromUtf8(error.what()),request); }
}
