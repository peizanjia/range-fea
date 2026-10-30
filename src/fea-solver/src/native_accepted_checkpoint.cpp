#include "native_accepted_checkpoint.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <rml_model.h>
#include <rml_structural_dynamics_restart_data.h>
#include <rml_structural_dynamics_setup.h>
#include <rml_time_solver.h>
#include <rml_file_io.h>

#ifdef RANGE_NATIVE_CHECKPOINT_TEST_HOOKS
namespace NativeAcceptedCheckpointTestHooks
{
using RenameFailureHook = void (*)(const QString &, QFileDevice::FileError);
namespace { RenameFailureHook renameFailureHook = nullptr; }
void setRenameFailureHook(RenameFailureHook hook) { renameFailureHook = hook; }
void notifyRenameFailure(const QString &path, QFileDevice::FileError error)
{
    if (renameFailureHook) renameFailureHook(path,error);
}
}
#endif

namespace
{
constexpr auto contextSchema = "range.native-checkpoint-context/v1";
constexpr auto envelopeSchema = "range.native-checkpoint/v1";
constexpr auto pointerSchema = "range.native-checkpoint-pointer/v1";
constexpr auto traceEnvelopeSchema = "range.native-checkpoint/v2";
constexpr auto tracePointerSchema = "range.native-checkpoint-pointer/v2";
constexpr auto traceFileName = "accepted-trace.rpt";
constexpr auto pendingMarkerName = "pending-publication.json";
constexpr auto policyId = "accepted-steps-or-sim-time-v1";
constexpr uint frozenMaxAcceptedSteps = 10;
constexpr double frozenMaxSimulationSeconds = 1.0;
constexpr qsizetype maxContextBytes = 16384;
constexpr qint64 maxModelBytes = 1024ll*1024ll*1024ll;

void requireNoSymlinkComponents(const QString &absolutePath);
struct ExactModelWriter : RModel {
    explicit ExactModelWriter(const RModel &model):RModel(model) {}
    using RModel::writeBinary;
};

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toUtf8().constData());
}
#ifdef Q_OS_WIN
QString nativeExtendedPath(const QString &value)
{
    auto native=QDir::toNativeSeparators(value);
    if(native.startsWith(QStringLiteral("\\\\?\\")))return native;
    native=QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(value).absoluteFilePath()));
    if(native.startsWith(QStringLiteral("\\\\?\\")))return native;
    if(native.startsWith(QStringLiteral("\\\\")))return QStringLiteral("\\\\?\\UNC\\")+native.mid(2);
    if(native.size()>=3 && native[0].isLetter() && native[1]==QLatin1Char(':') && native[2]==QLatin1Char('\\'))
        return QStringLiteral("\\\\?\\")+native;
    fail(QStringLiteral("Checkpoint native path must be absolute."));
}
#endif

bool isLowerHexSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value)
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
              (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
            return false;
    return true;
}

QString sha256File(const QString &path)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() > maxModelBytes)
        fail(QStringLiteral("Checkpoint artifact must be a regular file of at most 1 GiB: '%1'.").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Cannot read checkpoint file '%1': %2")
                 .arg(path,file.errorString()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot hash checkpoint file '%1': %2")
                 .arg(path,file.errorString()));
    return QString::fromLatin1(hash.result().toHex());
}

QByteArray readFile(const QString &path, qsizetype maximumBytes = -1)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink())
        fail(QStringLiteral("Checkpoint artifact is not a regular file: '%1'.").arg(path));
    if (maximumBytes >= 0 && info.size() > maximumBytes)
        fail(QStringLiteral("Checkpoint metadata exceeds the %1-byte limit: '%2'.")
                 .arg(maximumBytes).arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Cannot open checkpoint metadata '%1': %2")
                 .arg(path,file.errorString()));
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot read checkpoint metadata '%1': %2")
                 .arg(path,file.errorString()));
    return bytes;
}

void writeAtomic(const QString &path, const QByteArray &bytes)
{
    constexpr int maxAttempts =
#ifdef Q_OS_WIN
        6;
#else
        1;
#endif
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        // Recheck the directory and destination before every fresh QSaveFile
        // so a retry cannot bypass the same path-safety checks as the first try.
        requireNoSymlinkComponents(QFileInfo(path).absolutePath());
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile()))
            fail(QStringLiteral("Checkpoint publication refuses a non-regular target: '%1'.")
                     .arg(path));

        QString commitError;
        bool retryableRename = false;
        {
            // A failed Windows replacement can be caused by a reader that
            // opened the old pointer without FILE_SHARE_DELETE. Every retry
            // gets a new temporary file; direct-write fallback stays disabled.
            QSaveFile file(path);
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly))
                fail(QStringLiteral("Cannot stage checkpoint publication '%1': %2")
                         .arg(path,file.errorString()));
            if (file.write(bytes) != bytes.size())
            {
                const QString error = file.errorString();
                file.cancelWriting();
                fail(QStringLiteral("Cannot write checkpoint publication '%1': %2")
                         .arg(path,error));
            }
            if (file.commit()) return;
            commitError = file.errorString();
#ifdef Q_OS_WIN
            retryableRename = file.error() == QFileDevice::RenameError;
#endif
#ifdef RANGE_NATIVE_CHECKPOINT_TEST_HOOKS
            NativeAcceptedCheckpointTestHooks::notifyRenameFailure(path,file.error());
#endif
            file.cancelWriting();
        }

#ifdef Q_OS_WIN
        if (retryableRename && attempt + 1 < maxAttempts)
        {
            // Five bounded exponential waits (5..80 ms) tolerate short-lived
            // sharing violations while keeping persistent errors fail-closed.
            const auto delay = std::chrono::milliseconds(5 * (1 << attempt));
            std::this_thread::sleep_for(delay);
            continue;
        }
#endif
        fail(QStringLiteral("Cannot atomically publish checkpoint '%1': %2")
                 .arg(path,commitError));
    }
}

QJsonObject parseObject(const QByteArray &bytes, const QString &what,
                        qsizetype maximumBytes)
{
    if (bytes.isEmpty() || bytes.size() > maximumBytes)
        fail(QStringLiteral("%1 must contain 1..%2 bytes.").arg(what).arg(maximumBytes));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes,&error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("%1 is not a valid JSON object: %2.")
                 .arg(what,error.errorString()));
    return document.object();
}

void requireOnlyKeys(const QJsonObject &object, QStringList expected,
                     const QString &what)
{
    QStringList actual = object.keys();
    std::sort(actual.begin(),actual.end());
    std::sort(expected.begin(),expected.end());
    if (actual != expected)
        fail(QStringLiteral("%1 has missing or unknown fields.").arg(what));
}

void requireHashField(const QJsonObject &object, const QString &key)
{
    if (!object.value(key).isString() ||
        !isLowerHexSha256(object.value(key).toString()))
        fail(QStringLiteral("Checkpoint context field '%1' must be a lowercase SHA-256.")
                 .arg(key));
}

bool closeTime(double actual, double expected)
{
    const double scale = std::max({1.0,std::abs(actual),std::abs(expected)});
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual-expected) <= 1.0e-12*scale;
}

NativeAcceptedCheckpointStateView legacyStateView(const RModel &model)
{
    const auto &setup = model.getProblemSetup().getStructuralDynamicsSetup();
    const auto &state = model.getProblemSetup().getStructuralDynamicsRestartData();
    state.validate();
    if (!setup.getEnabled() || !std::isfinite(setup.getTimeStep()) ||
        setup.getTimeStep() <= 0.0 ||
        (state.available && state.fixedTimeStep != setup.getTimeStep()))
        fail(QStringLiteral("Native checkpoint requires enabled structural dynamics."));
    QByteArray bytes;
    QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    const auto vector = [&stream](const std::vector<double> &values)
    {
        stream << quint64(values.size());
        for (double value : values)
        {
            if (!std::isfinite(value))
                fail(QStringLiteral("Checkpoint state contains a non-finite binary64 value."));
            stream << value;
        }
    };
    stream << QByteArrayLiteral("range.native-checkpoint.legacy-state/v1")
           << state.available << state.completedSteps << state.time << state.fixedTimeStep
           << state.meshNodeCount << state.meshElementCount << state.physicalDofCount
           << state.meshFingerprint << state.dofFingerprint << state.systemFingerprint;
    vector(state.q); vector(state.v); vector(state.a); vector(state.externalForce);
    stream << state.cumulativeExternalWork << state.cumulativeDampingDissipation
           << state.initialMechanicalEnergy << state.supportResponseAvailable
           << state.supportFingerprint;
    vector(state.supportReactions); vector(state.supportPowers); vector(state.cumulativeSupportWork);
    stream << state.controlAvailable << state.controlProtocolVersion << state.controlFingerprint
           << quint64(state.controlChannelIds.size());
    for (const auto &id : state.controlChannelIds) stream << id;
    vector(state.controlLastCommands); vector(state.controlPowers); vector(state.cumulativeControlWork);
    stream << state.controlControllerState << state.controlDeterministicSeed;
    if (stream.status() != QDataStream::Ok ||
        !std::isfinite(state.time) || !std::isfinite(state.fixedTimeStep) ||
        !std::isfinite(state.cumulativeExternalWork) ||
        !std::isfinite(state.cumulativeDampingDissipation) ||
        !std::isfinite(state.initialMechanicalEnergy))
        fail(QStringLiteral("Checkpoint restart cannot be encoded as finite binary64 state."));
    return {state.available ? state.completedSteps : 0,
            state.available ? state.time : 0.0,
            state.available ? state.fixedTimeStep : setup.getTimeStep(),
            QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)};
}

void validateView(const NativeAcceptedCheckpointStateView &view)
{
    if (view.stateFingerprint32.size() != 32 || !std::isfinite(view.timeS) ||
        !std::isfinite(view.dtS) || view.dtS <= 0.0 ||
        view.sequence > std::numeric_limits<uint>::max())
        fail(QStringLiteral("Native checkpoint extractor returned an invalid state identity."));
}

bool sameView(const NativeAcceptedCheckpointStateView &a,
              const NativeAcceptedCheckpointStateView &b)
{
    return a.sequence == b.sequence && a.timeS == b.timeS && a.dtS == b.dtS &&
           a.stateFingerprint32 == b.stateFingerprint32;
}

bool safeBasename(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") &&
           name != QStringLiteral("..") &&
           QFileInfo(name).fileName() == name &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

void requireNoSymlinkComponents(const QString &absolutePath)
{
    const QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(absolutePath));
    QString root;
    QString remainder;
    if (normalized.startsWith(QStringLiteral("//")))
    {
        QStringList parts = normalized.mid(2).split(QLatin1Char('/'),Qt::SkipEmptyParts);
        if (parts.size() < 2)
            fail(QStringLiteral("Checkpoint UNC path has no server/share root."));
        const QString server = parts.takeFirst();
        const QString share = parts.takeFirst();
        root = QStringLiteral("//%1/%2/").arg(server,share);
        remainder = parts.join(QLatin1Char('/'));
    }
    else if (normalized.size() >= 3 && normalized[0].isLetter() &&
             normalized[1] == QLatin1Char(':') && normalized[2] == QLatin1Char('/'))
    {
        root = normalized.left(3);
        remainder = normalized.mid(3);
    }
    else if (normalized.startsWith(QLatin1Char('/')))
    {
        root = QStringLiteral("/");
        remainder = normalized.mid(1);
    }
    else
    {
        fail(QStringLiteral("Checkpoint directory has no absolute filesystem root."));
    }
    QString current = root;
    const QStringList components = remainder.split(QLatin1Char('/'),Qt::SkipEmptyParts);
    for (const QString &component : components)
    {
        current = QDir(current).filePath(component);
        if (QFileInfo(current).isSymLink())
            fail(QStringLiteral("Native checkpoint path contains a symbolic-link component: '%1'.")
                     .arg(current));
    }
}

QJsonObject pointerObject(const QString &envelopeFile, const QString &envelopeHash)
{
    return {{QStringLiteral("schema"),QString::fromLatin1(pointerSchema)},
            {QStringLiteral("envelopeFile"),envelopeFile},
            {QStringLiteral("envelopeSha256"),envelopeHash}};
}
}

// Windows denies write/delete sharing for the lifetime of accepted base and
// current periodic MODEL authority. Other platforms retain an inode descriptor
// and check precise file identity/metadata at each transaction boundary. This
// avoids hashing the full MODEL once per FE while preserving cold hash proofs.
class NativeAcceptedCheckpointStore::ModelPin final
{
    QString path;
#ifdef Q_OS_WIN
    HANDLE handle=INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION original{};
    static bool equal(const BY_HANDLE_FILE_INFORMATION &a,const BY_HANDLE_FILE_INFORMATION &b) {
        return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow &&
            a.nFileSizeHigh==b.nFileSizeHigh && a.nFileSizeLow==b.nFileSizeLow && a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime &&
            a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime && a.dwFileAttributes==b.dwFileAttributes;
    }
    HANDLE open() const {const auto native=nativeExtendedPath(path);
        return CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);}
#else
    int handle=-1;
    struct stat original{};
    static bool equal(const struct stat &a,const struct stat &b) {
#ifdef Q_OS_DARWIN
        const bool times=a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec &&
            a.st_ctimespec.tv_sec==b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec==b.st_ctimespec.tv_nsec;
#else
        const bool times=a.st_mtim.tv_sec==b.st_mtim.tv_sec && a.st_mtim.tv_nsec==b.st_mtim.tv_nsec &&
            a.st_ctim.tv_sec==b.st_ctim.tv_sec && a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
#endif
        return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size && a.st_mode==b.st_mode && times;
    }
#endif
public:
    ModelPin(const ModelPin &)=delete;
    ModelPin &operator=(const ModelPin &)=delete;
    explicit ModelPin(QString modelPath):path(std::move(modelPath)) {
        requireNoSymlinkComponents(QFileInfo(path).absolutePath());const QFileInfo info(path);
        if(!info.isFile() || info.isSymLink() || info.size()>maxModelBytes)fail(QStringLiteral("Immutable MODEL pin requires a regular bounded file."));
#ifdef Q_OS_WIN
        handle=open();if(handle==INVALID_HANDLE_VALUE)fail(QStringLiteral("Cannot pin immutable MODEL against writers: '%1'.").arg(path));
        if(!GetFileInformationByHandle(handle,&original) || (original.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;fail(QStringLiteral("Immutable MODEL handle identity is invalid."));}
#else
        handle=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(handle<0)fail(QStringLiteral("Cannot pin immutable MODEL inode."));
        if(::fstat(handle,&original)!=0 || !S_ISREG(original.st_mode)) {::close(handle);handle=-1;fail(QStringLiteral("Immutable MODEL inode identity is invalid."));}
#endif
        try{verify();}catch(...){
#ifdef Q_OS_WIN
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
#else
            ::close(handle);handle=-1;
#endif
            throw;}
    }
    ~ModelPin(){
#ifdef Q_OS_WIN
        if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
#else
        if(handle>=0)::close(handle);
#endif
    }
    void verify() const {
        requireNoSymlinkComponents(QFileInfo(path).absolutePath());const QFileInfo info(path);
        if(!info.isFile() || info.isSymLink())fail(QStringLiteral("Pinned immutable MODEL path changed."));
#ifdef Q_OS_WIN
        BY_HANDLE_FILE_INFORMATION held{},current{};
        if(!GetFileInformationByHandle(handle,&held) || !equal(held,original))fail(QStringLiteral("Pinned immutable MODEL metadata changed."));
        const HANDLE currentHandle=open();if(currentHandle==INVALID_HANDLE_VALUE)fail(QStringLiteral("Pinned immutable MODEL path cannot be reopened."));
        const bool same=GetFileInformationByHandle(currentHandle,&current) && equal(current,original);CloseHandle(currentHandle);
        if(!same)fail(QStringLiteral("Pinned immutable MODEL file identity changed."));
#else
        struct stat held{},current{};
        if(::fstat(handle,&held)!=0 || ::lstat(QFile::encodeName(path).constData(),&current)!=0 || !equal(held,original) || !equal(current,original))
            fail(QStringLiteral("Pinned immutable MODEL identity or metadata changed."));
#endif
    }
};

NativeAcceptedCheckpointContext NativeAcceptedCheckpointContext::parse(
    const QByteArray &canonicalJson)
{
    if (canonicalJson.size() > maxContextBytes)
        fail(QStringLiteral("Native checkpoint context exceeds 16384 bytes."));
    const QJsonObject object = parseObject(canonicalJson,
        QStringLiteral("Native checkpoint context"),maxContextBytes);
    requireOnlyKeys(object,{
        QStringLiteral("schema"),QStringLiteral("sourceSha256"),
        QStringLiteral("modelSha256"),QStringLiteral("controlSha256"),
        QStringLiteral("runtimeSha256"),QStringLiteral("packageSha256"),
        QStringLiteral("policy"),QStringLiteral("maxAcceptedSteps"),
        QStringLiteral("maxSimulationSeconds"),QStringLiteral("dtS")
    },QStringLiteral("Native checkpoint context"));
    if (object.value(QStringLiteral("schema")).toString() != QString::fromLatin1(contextSchema))
        fail(QStringLiteral("Unsupported native checkpoint context schema."));

    NativeAcceptedCheckpointContext context;
    for (const QString &key : {QStringLiteral("sourceSha256"),QStringLiteral("modelSha256"),
                               QStringLiteral("controlSha256"),QStringLiteral("runtimeSha256"),
                               QStringLiteral("packageSha256")})
        requireHashField(object,key);
    context.sourceSha256 = object.value(QStringLiteral("sourceSha256")).toString();
    context.modelSha256 = object.value(QStringLiteral("modelSha256")).toString();
    context.controlSha256 = object.value(QStringLiteral("controlSha256")).toString();
    context.runtimeSha256 = object.value(QStringLiteral("runtimeSha256")).toString();
    context.packageSha256 = object.value(QStringLiteral("packageSha256")).toString();
    context.policy = object.value(QStringLiteral("policy")).toString();
    if (context.policy != QString::fromLatin1(policyId))
        fail(QStringLiteral("Native checkpoint policy is not the frozen v1 policy."));

    const QJsonValue stepsValue = object.value(QStringLiteral("maxAcceptedSteps"));
    const QJsonValue secondsValue = object.value(QStringLiteral("maxSimulationSeconds"));
    const QJsonValue dtValue = object.value(QStringLiteral("dtS"));
    if (!stepsValue.isDouble() || stepsValue.toDouble() != double(frozenMaxAcceptedSteps) ||
        !secondsValue.isDouble() || secondsValue.toDouble() != frozenMaxSimulationSeconds ||
        !dtValue.isDouble() || !std::isfinite(dtValue.toDouble()) ||
        dtValue.toDouble() <= 0.0 || dtValue.toDouble() > frozenMaxSimulationSeconds)
        fail(QStringLiteral("Native checkpoint interval or dtS differs from frozen v1 bounds."));
    context.maxAcceptedSteps = frozenMaxAcceptedSteps;
    context.maxSimulationSeconds = frozenMaxSimulationSeconds;
    context.dtS = dtValue.toDouble();
    return context;
}

QString NativeAcceptedCheckpointContext::noControlSha256()
{
    const QByteArray domain = QByteArrayLiteral("range.native-checkpoint.no-control.v1");
    return QString::fromLatin1(QCryptographicHash::hash(
        domain,QCryptographicHash::Sha256).toHex());
}

QJsonObject NativeAcceptedCheckpointContext::toJsonObject() const
{
    return {{QStringLiteral("schema"),QString::fromLatin1(contextSchema)},
            {QStringLiteral("sourceSha256"),this->sourceSha256},
            {QStringLiteral("modelSha256"),this->modelSha256},
            {QStringLiteral("controlSha256"),this->controlSha256},
            {QStringLiteral("runtimeSha256"),this->runtimeSha256},
            {QStringLiteral("packageSha256"),this->packageSha256},
            {QStringLiteral("policy"),this->policy},
            {QStringLiteral("maxAcceptedSteps"),double(this->maxAcceptedSteps)},
            {QStringLiteral("maxSimulationSeconds"),this->maxSimulationSeconds},
            {QStringLiteral("dtS"),this->dtS}};
}

NativeAcceptedCheckpointStore::NativeAcceptedCheckpointStore(
    const QString &directory,
    const QByteArray &contextJson,
    const QString &inputModelSha256,
    const QString &controlSha256,
    const QString &runtimeSha256,
    bool restart,
    const RModel &initialModel,
    StateViewExtractor extractor)
    : NativeAcceptedCheckpointStore(directory,contextJson,inputModelSha256,controlSha256,runtimeSha256,
                                    restart,initialModel,std::move(extractor),TraceOptions{}) {}

NativeAcceptedCheckpointStore::NativeAcceptedCheckpointStore(
    const QString &directory,const QByteArray &contextJson,const QString &inputModelSha256,
    const QString &controlSha256,const QString &runtimeSha256,bool restart,const RModel &initialModel,
    StateViewExtractor extractor,TraceOptions options)
    : checkpointContext(NativeAcceptedCheckpointContext::parse(contextJson)),
      stateViewExtractor(extractor ? std::move(extractor) : StateViewExtractor(legacyStateView)),
      traceOptions(std::move(options))
{
    if(this->traceOptions.initialHook)setPhaseHook(this->traceOptions.initialHookPhase,std::move(this->traceOptions.initialHook));
    const QFileInfo requestedDirectory(directory);
    if (!requestedDirectory.isAbsolute())
        fail(QStringLiteral("Native checkpoint directory must be absolute."));
    const QString requestedAbsoluteDirectory =
        QDir::cleanPath(requestedDirectory.absoluteFilePath());
    requireNoSymlinkComponents(requestedAbsoluteDirectory);
    if (!QDir().mkpath(requestedAbsoluteDirectory))
        fail(QStringLiteral("Cannot create native checkpoint directory '%1'.").arg(directory));
    requireNoSymlinkComponents(requestedAbsoluteDirectory);
    QFileInfo canonicalDirectory(requestedAbsoluteDirectory);
    if (!canonicalDirectory.isDir() || canonicalDirectory.isSymLink() ||
        canonicalDirectory.canonicalFilePath().isEmpty())
        fail(QStringLiteral("Native checkpoint directory is not a regular directory."));
    this->directory = QDir::cleanPath(canonicalDirectory.canonicalFilePath());

    if (controlSha256 != this->checkpointContext.controlSha256)
        fail(QStringLiteral("Checkpoint context controlSha256 does not match the active controller."));
    if (runtimeSha256 != this->checkpointContext.runtimeSha256)
        fail(QStringLiteral("Checkpoint context runtimeSha256 does not match this solver executable."));

    const auto initialView = this->stateViewExtractor(initialModel);
    validateView(initialView);
    if (initialView.dtS != this->checkpointContext.dtS)
        fail(QStringLiteral("Input structural checkpoint dt does not match context dtS."));

    // Acquire the shared lease before inspecting PENDING. Constructor recovery
    // is the only automatic mutator; query and ordinary readLatest remain
    // read-only/fail-closed while a marker is present.
    this->writerLock = std::make_unique<NativeCheckpointRecovery::WriterLease>(this->directory);
    const QByteArray recoveryContext = QJsonDocument(this->checkpointContext.toJsonObject())
        .toJson(QJsonDocument::Compact);
    QString recoveryError;
    auto recoveryPlan = NativeCheckpointRecovery::Plan::inspect(this->directory,recoveryContext,&recoveryError);
    if (!recoveryError.isEmpty())
        fail(QStringLiteral("Pending checkpoint recovery evidence is invalid: %1").arg(recoveryError));
    if (recoveryPlan)
    {
        if (recoveryPlan->traceMode() != this->traceOptions.enabled)
            fail(QStringLiteral("Pending checkpoint recovery mode differs from the active store."));
        if (recoveryPlan->traceMode() && !recoveryPlan->latestExists())
            fail(QStringLiteral("Interrupted trace baseline recovery is unavailable; retained evidence is unchanged."));
        if (!recoveryPlan->restore(*this->writerLock,&recoveryError))
            fail(QStringLiteral("Pending checkpoint recovery could not restore its old authority: %1").arg(recoveryError));
        this->recoveringMarkerBytes = recoveryPlan->markerBytes();
    }

    const QString latestPath = QDir(this->directory).filePath(QStringLiteral("latest.json"));
    const QString lastValidPath = QDir(this->directory).filePath(QStringLiteral("lastvalid.json"));
    if (QFileInfo(latestPath).isSymLink() || QFileInfo(lastValidPath).isSymLink())
        fail(QStringLiteral("Native checkpoint pointers cannot be symbolic links."));
    if(this->traceOptions.enabled) {
        if(!QFileInfo::exists(latestPath) && inputModelSha256!=this->checkpointContext.modelSha256)
            fail(QStringLiteral("Trace checkpoint context does not match the pinned input MODEL."));
        initializeTrace(initialModel,initialView,restart);
        if (recoveryPlan && !recoveryPlan->finish(*this->writerLock,&recoveryError))
            fail(QStringLiteral("Restored trace authority could not clear its pending marker: %1").arg(recoveryError));
        return;
    }
    bool existingCheckpoint = false;
    VerifiedCheckpoint latest;
    if (QFileInfo::exists(latestPath))
    {
        latest = this->readPointer(QStringLiteral("latest.json"),false);
        existingCheckpoint = true;
    }
    else if (QFileInfo::exists(lastValidPath))
    {
        fail(QStringLiteral("Committed latest native checkpoint is missing; lastvalid is diagnostic history only."));
    }

    if (!existingCheckpoint && inputModelSha256 != this->checkpointContext.modelSha256)
        fail(QStringLiteral("Checkpoint context modelSha256 does not match the input MODEL."));

    if (existingCheckpoint)
    {
        if (!restart || initialView.sequence == 0)
            fail(QStringLiteral("Existing checkpoint directories can only continue from their verified latest accepted MODEL."));
        RModel verifiedModel;
        verifiedModel.read(latest.modelPath);
        const auto verifiedView = this->stateViewExtractor(verifiedModel);
        validateView(verifiedView);
        if (!sameView(initialView,verifiedView))
            fail(QStringLiteral("Restart MODEL does not match the existing verified native checkpoint."));
        this->receipt = std::make_unique<VerifiedCheckpoint>(std::move(latest));
    }
    this->lastPublishedSequence = initialView.sequence;
    this->lastPublishedTimeS = initialView.timeS;
    this->lastObservedSequence = initialView.sequence;
    this->started = true;
    if (recoveryPlan && !recoveryPlan->finish(*this->writerLock,&recoveryError))
        fail(QStringLiteral("Restored checkpoint authority could not clear its pending marker: %1").arg(recoveryError));
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readPointer(const QString &pointerName,
                                           bool usedLastValid) const
{
    requireNoSymlinkComponents(this->directory);
    const QString pointerPath = QDir(this->directory).filePath(pointerName);
    if (!QFileInfo::exists(pointerPath))
        fail(QStringLiteral("Checkpoint pointer does not exist: '%1'.").arg(pointerName));
    if (QFileInfo(pointerPath).isSymLink())
        fail(QStringLiteral("Checkpoint pointer cannot be a symbolic link."));
    const QByteArray pointerBytes=readFile(pointerPath,64*1024);
    const QJsonObject pointer = parseObject(pointerBytes,
        QStringLiteral("Native checkpoint pointer"),64*1024);
    requireOnlyKeys(pointer,{QStringLiteral("schema"),QStringLiteral("envelopeFile"),
                             QStringLiteral("envelopeSha256")},
                    QStringLiteral("Native checkpoint pointer"));
    if (pointer.value(QStringLiteral("schema")).toString() != QString::fromLatin1(pointerSchema) &&
        !(this->traceOptions.enabled && pointer.value(QStringLiteral("schema")).toString()==QString::fromLatin1(tracePointerSchema)))
        fail(QStringLiteral("Unsupported native checkpoint pointer schema."));
    const QString envelopeFile = pointer.value(QStringLiteral("envelopeFile")).toString();
    const QString envelopeHash = pointer.value(QStringLiteral("envelopeSha256")).toString();
    if (!safeBasename(envelopeFile) || !isLowerHexSha256(envelopeHash))
        fail(QStringLiteral("Native checkpoint pointer contains an invalid envelope reference."));
    auto result=this->readEnvelope(envelopeFile,envelopeHash,usedLastValid);
    if(this->traceOptions.enabled && (QFileInfo(pointerPath).isSymLink() || readFile(pointerPath,64*1024)!=pointerBytes))
        fail(QStringLiteral("Trace checkpoint pointer changed during complete certification."));
    return result;
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readEnvelope(const QString &envelopeFile,
                                            const QString &envelopeHash,
                                            bool usedLastValid) const
{
    requireNoSymlinkComponents(this->directory);
    if (!safeBasename(envelopeFile) || !isLowerHexSha256(envelopeHash))
        fail(QStringLiteral("Native checkpoint envelope reference is invalid."));
    const QString envelopePath = QDir(this->directory).filePath(envelopeFile);
    const QByteArray envelopeBytes = readFile(envelopePath,64*1024);
    if (QString::fromLatin1(QCryptographicHash::hash(
            envelopeBytes,QCryptographicHash::Sha256).toHex()) != envelopeHash)
        fail(QStringLiteral("Native checkpoint envelope SHA-256 does not match its pointer."));
    const QJsonObject envelope = parseObject(envelopeBytes,
        QStringLiteral("Native checkpoint envelope"),64*1024);
    const QByteArray canonicalEnvelope = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (canonicalEnvelope != envelopeBytes)
        fail(QStringLiteral("Native checkpoint envelope is not canonical JSON."));
    if(this->traceOptions.enabled && envelope.value(QStringLiteral("schema")).toString()==QString::fromLatin1(traceEnvelopeSchema)) {
        auto result=readTraceEnvelope(envelope,usedLastValid);
        if(QFileInfo(envelopePath).isSymLink() || readFile(envelopePath,64*1024)!=envelopeBytes)
            fail(QStringLiteral("Immutable trace envelope changed during complete certification."));
        return result;
    }
    requireOnlyKeys(envelope,{
        QStringLiteral("schema"),QStringLiteral("sourceSha256"),
        QStringLiteral("inputModelSha256"),QStringLiteral("controlSha256"),
        QStringLiteral("runtimeSha256"),QStringLiteral("packageSha256"),
        QStringLiteral("policy"),QStringLiteral("maxAcceptedSteps"),
        QStringLiteral("maxSimulationSeconds"),QStringLiteral("acceptedSeq"),
        QStringLiteral("timeS"),QStringLiteral("dtS"),
        QStringLiteral("modelFile"),QStringLiteral("checkpointModelSha256")
    },QStringLiteral("Native checkpoint envelope"));
    if (envelope.value(QStringLiteral("schema")).toString() != QString::fromLatin1(envelopeSchema) ||
        envelope.value(QStringLiteral("sourceSha256")).toString() != this->checkpointContext.sourceSha256 ||
        envelope.value(QStringLiteral("inputModelSha256")).toString() != this->checkpointContext.modelSha256 ||
        envelope.value(QStringLiteral("controlSha256")).toString() != this->checkpointContext.controlSha256 ||
        envelope.value(QStringLiteral("runtimeSha256")).toString() != this->checkpointContext.runtimeSha256 ||
        envelope.value(QStringLiteral("packageSha256")).toString() != this->checkpointContext.packageSha256 ||
        envelope.value(QStringLiteral("policy")).toString() != this->checkpointContext.policy ||
        envelope.value(QStringLiteral("maxAcceptedSteps")).toDouble(-1.0) != double(this->checkpointContext.maxAcceptedSteps) ||
        envelope.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) != this->checkpointContext.maxSimulationSeconds)
        fail(QStringLiteral("Native checkpoint envelope does not match the supplied recovery context."));
    const QJsonValue sequenceValue = envelope.value(QStringLiteral("acceptedSeq"));
    const QJsonValue timeValue = envelope.value(QStringLiteral("timeS"));
    const QJsonValue dtValue = envelope.value(QStringLiteral("dtS"));
    const QString modelFile = envelope.value(QStringLiteral("modelFile")).toString();
    const QString modelHash = envelope.value(QStringLiteral("checkpointModelSha256")).toString();
    if (!sequenceValue.isDouble() || sequenceValue.toDouble() < 1.0 ||
        std::floor(sequenceValue.toDouble()) != sequenceValue.toDouble() ||
        sequenceValue.toDouble() > double(std::numeric_limits<uint>::max()) ||
        !timeValue.isDouble() || !std::isfinite(timeValue.toDouble()) ||
        !dtValue.isDouble() || dtValue.toDouble() != this->checkpointContext.dtS ||
        !safeBasename(modelFile) || !modelFile.endsWith(QStringLiteral(".rbm")) ||
        !isLowerHexSha256(modelHash))
        fail(QStringLiteral("Native checkpoint envelope has invalid accepted-state fields."));
    const QString modelPath = QDir(this->directory).filePath(modelFile);
    if (QFileInfo(modelPath).isSymLink() || sha256File(modelPath) != modelHash)
        fail(QStringLiteral("Immutable checkpoint MODEL is missing or its SHA-256 does not match."));
    RModel checkpointModel;
    checkpointModel.read(modelPath);
    if (sha256File(modelPath) != modelHash)
        fail(QStringLiteral("Immutable checkpoint MODEL changed during verification."));
    const auto state = this->stateViewExtractor(checkpointModel);
    validateView(state);
    if (state.sequence != quint64(sequenceValue.toDouble()) ||
        state.timeS != timeValue.toDouble() || state.dtS != dtValue.toDouble() ||
        state.sequence != quint64(checkpointModel.getTimeSolver().getCurrentTimeStep())+1 ||
        !closeTime(state.timeS,checkpointModel.getTimeSolver().getComputedTime()))
        fail(QStringLiteral("Checkpoint MODEL does not match its accepted sequence, time or dt."));
    return {modelPath,envelope,usedLastValid,state.sequence};
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readLatest() const
{
    if (this->poisoned)
        fail(QStringLiteral("Native checkpoint rollback is incomplete; latest is not accepted recovery authority for this store. Inspect the retained rollback evidence."));
    if (this->activePublication)
        fail(QStringLiteral("Native checkpoint publication has not committed."));
    requireNoSymlinkComponents(this->directory);
    const QFileInfo marker(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)));
    if (marker.exists() || marker.isSymLink())
        fail(QStringLiteral("Native checkpoint has an unresolved publication marker; explicit repair is required before recovery."));
    auto result=this->readPointer(QStringLiteral("latest.json"),false);
    if(this->traceOptions.enabled && (QFileInfo(marker.absoluteFilePath()).exists() || QFileInfo(marker.absoluteFilePath()).isSymLink()))
        fail(QStringLiteral("Trace publication started during complete recovery certification."));
    return result;
}

struct NativeAcceptedCheckpointStore::PublicationState
{
    struct PointerSnapshot
    {
        bool exists = false;
        QByteArray bytes;
    };
    RAcceptedStepIdentity identity;
    NativeAcceptedCheckpointStateView expected;
    PointerSnapshot latest;
    PointerSnapshot lastValid;
    QByteArray newPointerBytes;
    std::unique_ptr<VerifiedCheckpoint> verifiedReceipt;
    QStringList evidencePaths;
    QString markerPath;
    QByteArray markerBytes;
    bool due = false;
    bool markerCreated = false;
    bool pointersTouched = false;
    bool published = false;
    bool trace = false, traceTouched = false, fullModelDue = false;
    PiezoAcceptedTrace::Cursor oldTraceCursor, newTraceCursor;
    QByteArray oldLatestBytes, oldLastValidBytes;
    std::shared_ptr<const RModel> nextFullModel;
    std::shared_ptr<const ModelPin> nextFullModelPin;
};

class NativeAcceptedCheckpointStore::Publication final : public RAcceptedStepPublication
{
public:
    NativeAcceptedCheckpointStore *owner;
    std::unique_ptr<PublicationState> state;
    bool committed = false;
    bool rolledBack = false;

    explicit Publication(NativeAcceptedCheckpointStore *owner,
                         std::unique_ptr<PublicationState> state)
        : owner(owner), state(std::move(state)) {}

    ~Publication() override
    {
        if (!this->committed && !this->rolledBack) (void)this->rollback();
    }

    void ensureMarker()
    {
        auto &store = *this->owner;
        requireNoSymlinkComponents(store.directory);
        const QFileInfo marker(this->state->markerPath);
        if (marker.exists() || marker.isSymLink())
        {
            if (readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
                fail(QStringLiteral("Pending publication marker differs from the staged barrier."));
            this->state->markerCreated = true;
            return;
        }
        this->state->markerCreated = true;
        store.invokePhase(Phase::BeforeMarkerWrite);
        if (QFileInfo(this->state->markerPath).exists() || QFileInfo(this->state->markerPath).isSymLink())
            fail(QStringLiteral("Pending publication marker appeared during staging."));
        writeAtomic(this->state->markerPath,this->state->markerBytes);
        store.invokePhase(Phase::AfterMarkerWrite);
        if (readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
            fail(QStringLiteral("Pending publication marker failed read-back verification."));
    }

    //! Its successful removal is the final potentially failing operation in
    //! publish. No I/O, callbacks or allocation follow it before commit.
    void removeMarker(bool allowAbsent)
    {
        auto &store = *this->owner;
        requireNoSymlinkComponents(store.directory);
        const QFileInfo marker(this->state->markerPath);
        if (!marker.exists() && !marker.isSymLink() && allowAbsent)
        {
            this->state->markerCreated = false;
            return;
        }
        if (readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
            fail(QStringLiteral("Refusing to remove a different pending publication marker."));
        if (!QFile::remove(this->state->markerPath))
            fail(QStringLiteral("Cannot remove the pending publication marker."));
        this->state->markerCreated = false;
    }

    void publish() override
    {
        if (!this->owner || !this->state || this->committed || this->rolledBack ||
            this->state->published || this->owner->poisoned)
            fail(QStringLiteral("Native checkpoint publication is not publishable."));
        if (!this->state->due)
        {
            this->state->published = true;
            return;
        }
        auto &store = *this->owner;
        requireNoSymlinkComponents(store.directory);
        if (!this->state->markerCreated ||
            readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
            fail(QStringLiteral("Native checkpoint publication has no verified pending barrier."));
        const QString latestPath = QDir(store.directory).filePath(QStringLiteral("latest.json"));
        const QString lastValidPath = QDir(store.directory).filePath(QStringLiteral("lastvalid.json"));
        const auto unchanged = [](const QString &path, const PublicationState::PointerSnapshot &snapshot)
        {
            const QFileInfo info(path);
            if (info.isSymLink() || info.exists() != snapshot.exists ||
                (snapshot.exists && readFile(path,64*1024) != snapshot.bytes))
                fail(QStringLiteral("Checkpoint pointer changed after staging: '%1'.").arg(path));
        };
        unchanged(latestPath,this->state->latest);
        unchanged(lastValidPath,this->state->lastValid);
        if (this->state->latest.exists)
        {
            this->state->pointersTouched = true;
            store.invokePhase(Phase::BeforeLastValid);
            requireNoSymlinkComponents(store.directory);
            writeAtomic(lastValidPath,this->state->latest.bytes);
            store.invokePhase(Phase::AfterLastValid);
            if (readFile(lastValidPath,64*1024) != this->state->latest.bytes)
                fail(QStringLiteral("Previous valid checkpoint pointer failed read-back verification."));
        }
        this->state->pointersTouched = true;
        store.invokePhase(Phase::BeforeLatest);
        requireNoSymlinkComponents(store.directory);
        writeAtomic(latestPath,this->state->newPointerBytes);
        store.invokePhase(Phase::AfterLatest);
        if (readFile(latestPath,64*1024) != this->state->newPointerBytes)
            fail(QStringLiteral("Latest checkpoint pointer failed read-back verification."));
        // Trace frame and model pins are revalidated after all phase hooks,
        // immediately before the pending marker is removed.
        if(!this->state->trace) {
            const auto verified = store.readPointer(QStringLiteral("latest.json"),false);
            if (verified.modelPath != this->state->verifiedReceipt->modelPath || verified.envelope != this->state->verifiedReceipt->envelope)
                fail(QStringLiteral("Latest checkpoint pointer resolved to a different staged checkpoint."));
        }
        store.invokePhase(Phase::AfterLatestReadBack);
        store.invokePhase(Phase::BeforeMarkerRemove);
        if (readFile(latestPath,64*1024) != this->state->newPointerBytes)
            fail(QStringLiteral("Latest checkpoint pointer changed after read-back verification."));
        if(this->state->trace)store.verifyStagedTrace(*this->state);
        else {
            const auto finalVerified = store.readPointer(QStringLiteral("latest.json"),false);
            if (finalVerified.modelPath != this->state->verifiedReceipt->modelPath || finalVerified.envelope != this->state->verifiedReceipt->envelope)
                fail(QStringLiteral("Checkpoint state changed after the latest read-back hook."));
        }
        if (this->state->latest.exists)
        {
            if (readFile(lastValidPath,64*1024) != this->state->latest.bytes)
                fail(QStringLiteral("Last-valid checkpoint pointer changed during publication."));
        }
        else
            unchanged(lastValidPath,this->state->lastValid);
        // The host must publish this participant last, then commit without
        // further throwing operations. This barrier is not a crash journal.
        this->removeMarker(false);
        this->state->published = true;
    }

    bool rollback() noexcept override
    {
        if (this->committed || this->rolledBack) return true;
        if (!this->owner || !this->state) return false;
        auto &store = *this->owner;
        bool restored = true;
        const auto restore = [&store,&restored](const QString &name,
            const PublicationState::PointerSnapshot &snapshot, Phase before, Phase after)
        {
            try
            {
                store.invokePhase(before);
                requireNoSymlinkComponents(store.directory);
                const QString path = QDir(store.directory).filePath(name);
                const QFileInfo info(path);
                if (info.isSymLink() || (info.exists() && !info.isFile()))
                    fail(QStringLiteral("Rollback refuses a non-regular pointer target: '%1'.").arg(path));
                if (snapshot.exists)
                {
                    writeAtomic(path,snapshot.bytes);
                    if (readFile(path,64*1024) != snapshot.bytes)
                        fail(QStringLiteral("Rollback pointer read-back differs from its snapshot."));
                }
                else
                {
                    if (info.exists() && !QFile::remove(path))
                        fail(QStringLiteral("Rollback cannot remove the newly published pointer."));
                    if (QFileInfo(path).exists() || QFileInfo(path).isSymLink())
                        fail(QStringLiteral("Rollback did not restore pointer absence."));
                }
                store.invokePhase(after);
                requireNoSymlinkComponents(store.directory);
                if (snapshot.exists)
                {
                    if (readFile(path,64*1024) != snapshot.bytes)
                        fail(QStringLiteral("Rollback pointer changed after restoration."));
                }
                else if (QFileInfo(path).exists() || QFileInfo(path).isSymLink())
                    fail(QStringLiteral("Rollback pointer absence changed after restoration."));
            }
            catch (...)
            {
                restored = false;
                if (!store.rollbackFailure) store.rollbackFailure = std::current_exception();
            }
        };
        if (this->state->pointersTouched)
        {
            try
            {
                // Also guards an explicit rollback after successful publish,
                // which has already removed the original pending barrier.
                this->ensureMarker();
            }
            catch (...)
            {
                restored = false;
                if (!store.rollbackFailure) store.rollbackFailure = std::current_exception();
            }
            if (restored)
            {
                restore(QStringLiteral("latest.json"),this->state->latest,
                        Phase::BeforeRollbackLatest,Phase::AfterRollbackLatest);
                restore(QStringLiteral("lastvalid.json"),this->state->lastValid,
                        Phase::BeforeRollbackLastValid,Phase::AfterRollbackLastValid);
            }
        }
        if(this->state->traceTouched && restored) {
            try {
                this->ensureMarker();store.invokePhase(Phase::BeforeTraceTruncate);requireNoSymlinkComponents(store.directory);
                QFile trace(QDir(store.directory).filePath(QString::fromLatin1(traceFileName)));
                if(QFileInfo(trace.fileName()).isSymLink() || !trace.open(QIODevice::ReadWrite) ||
                   !trace.resize(this->state->oldTraceCursor.byteEnd) || !trace.flush())
                    fail(QStringLiteral("Trace rollback could not restore its committed byte boundary."));
                store.invokePhase(Phase::AfterTraceTruncate);
                const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,this->state->oldTraceCursor);
                if(prefix.header.headerSha32!=store.traceHeader.headerSha32)
                    fail(QStringLiteral("Trace rollback immutable header changed."));
                store.invokePhase(Phase::AfterTraceRollbackReadBack);
                if(PiezoAcceptedTrace::verifyPrefix(trace,this->state->oldTraceCursor).prefixSha32!=prefix.prefixSha32)
                    fail(QStringLiteral("Trace rollback prefix changed after readback."));
            }catch(...){restored=false;if(!store.rollbackFailure)store.rollbackFailure=std::current_exception();}
        }
        if (restored && this->state->markerCreated)
        {
            try
            {
                store.invokePhase(Phase::BeforeMarkerRemove);
                const auto verifySnapshot = [&store](const QString &name,
                    const PublicationState::PointerSnapshot &snapshot)
                {
                    const QString path = QDir(store.directory).filePath(name);
                    const QFileInfo info(path);
                    if (info.isSymLink() || info.exists() != snapshot.exists ||
                        (snapshot.exists && readFile(path,64*1024) != snapshot.bytes))
                        fail(QStringLiteral("Rollback pointer pair differs from the original snapshots."));
                };
                requireNoSymlinkComponents(store.directory);
                verifySnapshot(QStringLiteral("latest.json"),this->state->latest);
                verifySnapshot(QStringLiteral("lastvalid.json"),this->state->lastValid);
                if(this->state->trace)store.verifyModelPins();
                if(this->state->traceTouched){QFile trace(QDir(store.directory).filePath(QString::fromLatin1(traceFileName)));
                    if(QFileInfo(trace.fileName()).isSymLink() || !trace.open(QIODevice::ReadOnly) ||
                       PiezoAcceptedTrace::verifyPrefix(trace,this->state->oldTraceCursor).header.headerSha32!=store.traceHeader.headerSha32)
                        fail(QStringLiteral("Trace rollback boundary changed before marker removal."));}
                this->removeMarker(!this->state->pointersTouched);
            }
            catch (...)
            {
                restored = false;
                if (!store.rollbackFailure) store.rollbackFailure = std::current_exception();
            }
        }
        store.activePublication = nullptr;
        if (!restored)
        {
            store.poisoned = true;
            store.failedRollbackEvidencePaths.swap(this->state->evidencePaths);
            store.failedRollbackState = std::move(this->state);
            return false;
        }
        this->rolledBack = true;
        return true;
    }

    void commit() noexcept override
    {
        if (!this->owner || !this->state || this->committed || this->rolledBack ||
            !this->state->published || this->owner->poisoned) return;
        auto &store = *this->owner;
        store.hasObservedStep = true;
        store.lastObservedSequence = this->state->identity.sequence;
        if (this->state->due)
        {
            store.lastPublishedSequence = this->state->identity.sequence;
            store.lastPublishedTimeS = this->state->identity.timeS;
            store.receipt.swap(this->state->verifiedReceipt);
        }
        if(this->state->trace) {
            std::swap(store.traceCursor,this->state->newTraceCursor);
            store.cachedLatestBytes.swap(this->state->newPointerBytes);
            store.cachedLastValidBytes.swap(this->state->oldLatestBytes);
            if(this->state->fullModelDue) {
                store.cachedFullModel.swap(this->state->nextFullModel);
                store.currentModelPin.swap(this->state->nextFullModelPin);
                store.lastFullModelSequence=this->state->identity.sequence;
                store.lastFullModelTimeS=this->state->identity.timeS;
            }
        }
        store.activePublication = nullptr;
        this->committed = true;
    }
};

NativeAcceptedCheckpointStore::~NativeAcceptedCheckpointStore()
{
    if (this->activePublication)
    {
        auto *publication = this->activePublication;
        (void)publication->rollback();
        publication->owner = nullptr;
    }
}

std::unique_ptr<RAcceptedStepPublication> NativeAcceptedCheckpointStore::stage(
    const RModel &candidateModel, const RAcceptedStepIdentity &identity)
{
    return this->stageImpl(candidateModel,identity,false);
}

std::unique_ptr<RAcceptedStepPublication> NativeAcceptedCheckpointStore::stageImpl(
    const RModel &candidateModel, const RAcceptedStepIdentity &identity,
    bool allowLegacyFirstCallback)
{
    if(this->traceOptions.enabled)return stageTrace(candidateModel,identity);
    if (!this->started || this->poisoned)
        fail(QStringLiteral("Native checkpoint store is not retryable or initialized."));
    if (this->activePublication)
        fail(QStringLiteral("Native checkpoint store already has a staged publication."));
    requireNoSymlinkComponents(this->directory);
    const QFileInfo pendingMarker(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)));
    if (pendingMarker.exists() || pendingMarker.isSymLink())
        fail(QStringLiteral("Native checkpoint has an unresolved publication marker; explicit repair is required before staging."));
    const auto expected = this->stateViewExtractor(candidateModel);
    validateView(expected);
    const auto &clock = candidateModel.getTimeSolver();
    if (identity.sequence == 0 || identity.sequence != expected.sequence ||
        identity.timeS != expected.timeS || identity.dtS != expected.dtS ||
        identity.dtS != this->checkpointContext.dtS ||
        identity.sequence != quint64(clock.getCurrentTimeStep())+1 ||
        !closeTime(identity.timeS,clock.getComputedTime()) ||
        clock.getNTimeSteps() == 0 || identity.sequence > quint64(clock.getNTimeSteps()) ||
        (identity.sequence == quint64(clock.getNTimeSteps()) && !identity.finalStep))
        fail(QStringLiteral("Accepted structural checkpoint does not match the candidate model clock."));
    if ((!allowLegacyFirstCallback || this->hasObservedStep) &&
        identity.sequence != this->lastObservedSequence+1)
        fail(QStringLiteral("Accepted structural checkpoint sequence is not contiguous."));
    if (identity.sequence <= this->lastObservedSequence ||
        identity.timeS < this->lastPublishedTimeS)
        fail(QStringLiteral("Accepted structural checkpoint sequence or time moved backwards."));

    auto state = std::make_unique<PublicationState>();
    state->identity = identity;
    state->expected = expected;
    const double elapsed = identity.timeS-this->lastPublishedTimeS;
    state->due = identity.finalStep ||
        (this->requiredPublicationPredicate && this->requiredPublicationPredicate(identity.sequence)) ||
        identity.sequence-this->lastPublishedSequence >= this->checkpointContext.maxAcceptedSteps ||
        elapsed >= this->checkpointContext.maxSimulationSeconds ||
        elapsed+identity.dtS > this->checkpointContext.maxSimulationSeconds;
    requireNoSymlinkComponents(this->directory);
    const auto snapshot = [this](const QString &name)
    {
        PublicationState::PointerSnapshot saved;
        const QString path = QDir(this->directory).filePath(name);
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile()))
            fail(QStringLiteral("Checkpoint pointer is not a regular file: '%1'.").arg(path));
        saved.exists = info.exists();
        if (saved.exists) saved.bytes = readFile(path,64*1024);
        return saved;
    };
    state->latest = snapshot(QStringLiteral("latest.json"));
    state->lastValid = snapshot(QStringLiteral("lastvalid.json"));
    if (state->latest.exists)
        (void)this->readPointer(QStringLiteral("latest.json"),false);
    else if (state->lastValid.exists)
        fail(QStringLiteral("Committed latest checkpoint is missing; lastvalid is diagnostic history only."));
    auto publication = std::make_unique<Publication>(this,std::move(state));
    this->activePublication = publication.get();
    if (!publication->state->due) return publication;

    // All immutable writes and mode-specific validation finish before any
    // recovery pointer is published. No mutable RModel is retained by the token.
    const quint64 acceptedSequence = identity.sequence;
    const double acceptedTimeS = identity.timeS;
    const double dtS = identity.dtS;
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Durable evidence precedes pointer publication and is never a recovery
    // authority. Small successful/abandoned preparations may remain unreferenced.
    this->invokePhase(Phase::BeforeEvidenceWrite);
    const auto evidenceFile = [this,&publication](const QString &name, const QByteArray &bytes)
    {
        if (bytes.size() > 64*1024)
            fail(QStringLiteral("Native checkpoint rollback evidence exceeds 64 KiB."));
        const QString path = QDir(this->directory).filePath(name);
        if (QFileInfo(path).exists() || QFileInfo(path).isSymLink())
            fail(QStringLiteral("Refusing to replace immutable rollback evidence: '%1'.").arg(path));
        writeAtomic(path,bytes);
        if (readFile(path,64*1024) != bytes)
            fail(QStringLiteral("Native checkpoint rollback evidence failed read-back verification."));
        publication->state->evidencePaths.append(path);
        return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    };
    const auto backup = [&token,&evidenceFile](const QString &pointerName,
        const PublicationState::PointerSnapshot &snapshot)
    {
        QJsonObject descriptor{{QStringLiteral("exists"),snapshot.exists}};
        if (snapshot.exists)
        {
            const QString backupName = QStringLiteral("rollback-%1-%2.pointer").arg(token,pointerName);
            const QString hash = evidenceFile(backupName,snapshot.bytes);
            descriptor.insert(QStringLiteral("file"),backupName);
            descriptor.insert(QStringLiteral("sha256"),hash);
        }
        return descriptor;
    };
    const QJsonObject latestBackup = backup(QStringLiteral("latest"),publication->state->latest);
    const QJsonObject lastValidBackup = backup(QStringLiteral("lastvalid"),publication->state->lastValid);
    const QByteArray contextBytes = QJsonDocument(this->checkpointContext.toJsonObject())
        .toJson(QJsonDocument::Compact);
    const QJsonObject evidenceManifest{
        {QStringLiteral("schema"),QStringLiteral("range.native-checkpoint-rollback-evidence/v1")},
        {QStringLiteral("candidateSeq"),double(identity.sequence)},
        {QStringLiteral("sourceSha256"),this->checkpointContext.sourceSha256},
        {QStringLiteral("contextSha256"),QString::fromLatin1(QCryptographicHash::hash(
            contextBytes,QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("latest"),latestBackup},
        {QStringLiteral("lastvalid"),lastValidBackup}
    };
    const QString evidenceManifestName = QStringLiteral("rollback-evidence-%1.json").arg(token);
    const QString evidenceManifestHash = evidenceFile(evidenceManifestName,
        QJsonDocument(evidenceManifest).toJson(QJsonDocument::Compact));
    this->invokePhase(Phase::AfterEvidenceReadBack);

    const QString modelFile = QStringLiteral("checkpoint-%1-%2.rbm")
        .arg(acceptedSequence,10,10,QChar('0')).arg(token);
    const QString modelPath = QDir(this->directory).filePath(modelFile);
    if (QFileInfo::exists(modelPath) || QFileInfo(modelPath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint MODEL."));

    this->invokePhase(Phase::BeforeModelWrite);
    requireNoSymlinkComponents(this->directory);
    if (QFileInfo::exists(modelPath) || QFileInfo(modelPath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint MODEL."));
    const QString writtenPath = candidateModel.write(modelPath,false);
    this->invokePhase(Phase::AfterModelWrite);
    if (QFileInfo(writtenPath).absoluteFilePath() != QFileInfo(modelPath).absoluteFilePath())
        fail(QStringLiteral("Native checkpoint MODEL was written to an unexpected path."));
    const QString modelHash = sha256File(modelPath);
    RModel readBack;
    readBack.read(modelPath);
    const auto actual = this->stateViewExtractor(readBack);
    validateView(actual);
    if (sha256File(modelPath) != modelHash || !sameView(expected,actual))
        fail(QStringLiteral("Binary checkpoint MODEL read-back differs from the accepted structural state."));
    this->invokePhase(Phase::AfterModelReadBack);

    QJsonObject envelope = this->checkpointContext.toJsonObject();
    envelope.insert(QStringLiteral("schema"),QString::fromLatin1(envelopeSchema));
    envelope.remove(QStringLiteral("modelSha256"));
    envelope.insert(QStringLiteral("inputModelSha256"),this->checkpointContext.modelSha256);
    envelope.insert(QStringLiteral("acceptedSeq"),double(acceptedSequence));
    envelope.insert(QStringLiteral("timeS"),acceptedTimeS);
    envelope.insert(QStringLiteral("dtS"),dtS);
    envelope.insert(QStringLiteral("modelFile"),modelFile);
    envelope.insert(QStringLiteral("checkpointModelSha256"),modelHash);
    const QString envelopeFile = QStringLiteral("envelope-%1-%2.json")
        .arg(acceptedSequence,10,10,QChar('0')).arg(token);
    const QString envelopePath = QDir(this->directory).filePath(envelopeFile);
    if (QFileInfo::exists(envelopePath) || QFileInfo(envelopePath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint envelope."));
    const QByteArray envelopeBytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    this->invokePhase(Phase::BeforeEnvelopeWrite);
    writeAtomic(envelopePath,envelopeBytes);
    this->invokePhase(Phase::AfterEnvelopeWrite);
    const QString envelopeHash = QString::fromLatin1(
        QCryptographicHash::hash(envelopeBytes,QCryptographicHash::Sha256).toHex());
    if (sha256File(envelopePath) != envelopeHash)
        fail(QStringLiteral("Native checkpoint envelope failed post-write hash verification."));

    const VerifiedCheckpoint verifiedEnvelope =
        this->readEnvelope(envelopeFile,envelopeHash,false);
    if (verifiedEnvelope.envelope.value(QStringLiteral("acceptedSeq")).toDouble(-1.0) !=
        double(acceptedSequence))
        fail(QStringLiteral("Native checkpoint envelope read-back has a different accepted sequence."));
    this->invokePhase(Phase::AfterEnvelopeReadBack);

    const QJsonObject newPointer = pointerObject(envelopeFile,envelopeHash);
    publication->state->newPointerBytes = QJsonDocument(newPointer).toJson(QJsonDocument::Compact);
    publication->state->verifiedReceipt = std::make_unique<VerifiedCheckpoint>(verifiedEnvelope);
    const QJsonObject marker{
        {QStringLiteral("schema"),QStringLiteral("range.native-checkpoint-pending-publication/v1")},
        {QStringLiteral("candidateSeq"),double(identity.sequence)},
        {QStringLiteral("contextSha256"),evidenceManifest.value(QStringLiteral("contextSha256"))},
        {QStringLiteral("rollbackManifestFile"),evidenceManifestName},
        {QStringLiteral("rollbackManifestSha256"),evidenceManifestHash}
    };
    publication->state->markerPath = QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName));
    publication->state->markerBytes = QJsonDocument(marker).toJson(QJsonDocument::Compact);
    if (publication->state->markerBytes.size() > 64*1024)
        fail(QStringLiteral("Native checkpoint pending publication marker exceeds 64 KiB."));
    publication->state->evidencePaths.append(publication->state->markerPath);
    publication->ensureMarker();
    return publication;
}

namespace {
QByteArray traceDigest(const QByteArray &bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
bool sameCursor(const PiezoAcceptedTrace::Cursor &a,const PiezoAcceptedTrace::Cursor &b){
    return a.byteEnd==b.byteEnd && a.lastRecordOffset==b.lastRecordOffset && a.sequence==b.sequence && a.recordSha32==b.recordSha32 && a.stateSha32==b.stateSha32;
}
void rawBinaryModel(const QString &path){
    RFile file(path,RFile::BINARY);if(!file.open(QIODevice::ReadOnly))fail(QStringLiteral("Trace source MODEL cannot be read."));
    RFileHeader header;RFileIO::readBinary(file,header);if(header.getType()!=R_FILE_TYPE_MODEL)fail(QStringLiteral("Trace source authority must be a raw MODEL, not a LINK."));
}
QJsonObject cursorFields(const PiezoAcceptedTrace::Cursor &c){return {{"byteEnd",double(c.byteEnd)},{"lastRecordOffset",double(c.lastRecordOffset)},
    {"sequence",double(c.sequence)},{"recordSha256",QString::fromLatin1(c.recordSha32.toHex())},{"stateSha256",QString::fromLatin1(c.stateSha32.toHex())}};}
}

void NativeAcceptedCheckpointStore::initializeTrace(const RModel &initial,const NativeAcceptedCheckpointStateView &view,bool restart)
{
    auto &h=this->traceOptions.header;
    const QString latestPath=QDir(this->directory).filePath(QStringLiteral("latest.json"));
    const QString lastValidPath=QDir(this->directory).filePath(QStringLiteral("lastvalid.json"));
    const bool latestExists=QFileInfo::exists(latestPath);
    if(!this->traceOptions.extractPayload || !this->traceOptions.certifyPayload || !this->traceOptions.rebuildAcceptedModel ||
       (h.family!=QStringLiteral("axial") && h.family!=QStringLiteral("layered")) || !isLowerHexSha256(h.sourceFingerprint) ||
       !isLowerHexSha256(h.configurationIdentity) || h.backendIdentity.isEmpty() || (!latestExists && h.baseSeq!=view.sequence) ||
       h.targetSteps<h.baseSeq || h.targetSteps-h.baseSeq>=PiezoAcceptedTrace::MaxRecords ||
       h.targetSteps!=initial.getTimeSolver().getNTimeSteps() || h.dt!=view.dtS ||
       (view.sequence?h.initialTime+double(view.sequence)*h.dt:h.initialTime)!=view.timeS)
        fail(QStringLiteral("Trace options, original clock or complete record budget are invalid."));
    const auto contextHash=traceDigest(QJsonDocument(this->checkpointContext.toJsonObject()).toJson(QJsonDocument::Compact));
    const auto gridHash=PiezoAcceptedTrace::targetTimeGridSha256(h.initialTime,h.dt,h.targetSteps);
    if((!h.contextSha256.isEmpty() && h.contextSha256!=contextHash) || (!h.targetTimeGridSha256.isEmpty() && h.targetTimeGridSha256!=gridHash))
        fail(QStringLiteral("Trace source context or original target grid differs from admission."));
    h.contextSha256=contextHash;h.targetTimeGridSha256=gridHash;
    if(!latestExists && QFileInfo::exists(lastValidPath))fail(QStringLiteral("Trace latest is missing; lastvalid is diagnostic only."));
    this->cachedLatestBytes=latestExists?readFile(latestPath,64*1024):QByteArray{};
    this->cachedLastValidBytes=QFileInfo::exists(lastValidPath)?readFile(lastValidPath,64*1024):QByteArray{};
    if(latestExists){
        if(!restart)fail(QStringLiteral("Existing trace checkpoint requires an explicit restart."));
        const auto pointer=parseObject(this->cachedLatestBytes,QStringLiteral("Trace admission pointer"),64*1024);
        if(pointer.value(QStringLiteral("schema")).toString()==QString::fromLatin1(tracePointerSchema)) {
            // Retain pins across strict admission and cached full MODEL loading;
            // a verifier's temporary handles must not leave a writable gap.
            const QString envelopeName=pointer.value(QStringLiteral("envelopeFile")).toString();
            if(!safeBasename(envelopeName))fail(QStringLiteral("Trace admission envelope path is invalid."));
            const auto envelope=parseObject(readFile(QDir(this->directory).filePath(envelopeName),64*1024),QStringLiteral("Trace admission envelope"),64*1024);
            const QString fullName=envelope.value(QStringLiteral("modelFile")).toString();
            if(!safeBasename(fullName) || !fullName.endsWith(QStringLiteral(".rbm")))fail(QStringLiteral("Trace admission full MODEL path is invalid."));
            QFile trace(QDir(this->directory).filePath(QString::fromLatin1(traceFileName)));
            if(QFileInfo(trace.fileName()).isSymLink() || !trace.open(QIODevice::ReadOnly))fail(QStringLiteral("Trace admission header is not readable."));
            const auto persisted=PiezoAcceptedTrace::readHeader(trace);
            if(!safeBasename(persisted.baseModelFile) || !persisted.baseModelFile.endsWith(QStringLiteral(".rbm")))fail(QStringLiteral("Trace admission base MODEL path is invalid."));
            this->baseModelPin=std::make_shared<const ModelPin>(QDir(this->directory).filePath(persisted.baseModelFile));
            this->currentModelPin=fullName==persisted.baseModelFile?this->baseModelPin:
                std::make_shared<const ModelPin>(QDir(this->directory).filePath(fullName));
        }
        auto verified=this->readPointer(QStringLiteral("latest.json"),false);
        if(verified.envelope.value(QStringLiteral("schema")).toString()==QString::fromLatin1(traceEnvelopeSchema)){
            RModel full;full.read(verified.modelPath);const auto fullView=this->stateViewExtractor(full);
            if(!verified.acceptedModel || (!sameView(view,this->stateViewExtractor(*verified.acceptedModel)) && !sameView(view,fullView)))
                fail(QStringLiteral("Restart MODEL differs from the verified full source and latest trace accepted state."));
            QFile trace(QDir(this->directory).filePath(QString::fromLatin1(traceFileName)));
            if(!trace.open(QIODevice::ReadOnly))fail(QStringLiteral("Trace cannot be reopened."));
            this->traceHeader=PiezoAcceptedTrace::readHeader(trace);this->traceCursor=verified.traceCursor;
            this->verifyModelPins();
            this->cachedFullModel=std::make_shared<const RModel>(std::move(full));
            this->lastFullModelSequence=verified.modelSequence;this->lastFullModelTimeS=this->stateViewExtractor(*this->cachedFullModel).timeS;
            const auto acceptedView=this->traceOptions.certifyPayload(verified.acceptedRuntime);
            this->verifyModelPins();
            QFile finalTrace(QDir(this->directory).filePath(QString::fromLatin1(traceFileName)));
            if(QFileInfo(finalTrace.fileName()).isSymLink() || !finalTrace.open(QIODevice::ReadOnly) ||
               PiezoAcceptedTrace::verifyPrefix(finalTrace,verified.traceCursor).header.headerSha32!=this->traceHeader.headerSha32 ||
               readFile(latestPath,64*1024)!=this->cachedLatestBytes ||
               QFileInfo(lastValidPath).exists()!=!this->cachedLastValidBytes.isEmpty() || QFileInfo(lastValidPath).isSymLink() ||
               (!this->cachedLastValidBytes.isEmpty() && readFile(lastValidPath,64*1024)!=this->cachedLastValidBytes) ||
               (QFileInfo(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName))).exists() &&
                (this->recoveringMarkerBytes.isEmpty() ||
                 readFile(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)),64*1024)!=this->recoveringMarkerBytes)) ||
               QFileInfo(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName))).isSymLink())
                fail(QStringLiteral("Trace recovery authority changed during cache admission."));
            this->lastObservedSequence=this->lastPublishedSequence=acceptedView.sequence;this->lastPublishedTimeS=acceptedView.timeS;
            this->receipt=std::make_unique<VerifiedCheckpoint>(std::move(verified));this->started=true;return;
        }
        // Explicit migration of a fully verified v1 seed into its own v2 baseline.
        if(h.baseSeq!=view.sequence)fail(QStringLiteral("Imported full MODEL is not the requested trace baseline sequence."));
        RModel seed;seed.read(verified.modelPath);if(!sameView(view,this->stateViewExtractor(seed)))
            fail(QStringLiteral("Imported full MODEL differs from the admitted trace baseline."));
        this->receipt=std::make_unique<VerifiedCheckpoint>(std::move(verified));
    }
    const auto payload=this->traceOptions.extractPayload(initial);const auto certified=this->traceOptions.certifyPayload(payload);
    validateView(certified);if(!sameView(view,certified) || certified.stateFingerprint32!=traceDigest(payload))
        fail(QStringLiteral("Trace baseline codec and actual source certificate differ."));
    auto baseline=std::make_shared<const RModel>(this->traceOptions.rebuildAcceptedModel(initial,payload));
    if(!sameView(view,this->stateViewExtractor(*baseline)))fail(QStringLiteral("Trace baseline rebuilder changed the accepted state."));
    h.baseModelFile=QStringLiteral("trace-base-%1.rbm").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString modelPath=QDir(this->directory).filePath(h.baseModelFile);
    this->invokePhase(Phase::BeforeModelWrite);if(QFileInfo::exists(modelPath) || QFileInfo(modelPath).isSymLink())fail(QStringLiteral("Trace base MODEL already exists."));
    ExactModelWriter(*baseline).writeBinary(modelPath);this->baseModelPin=std::make_shared<const ModelPin>(modelPath);
    this->currentModelPin=this->baseModelPin;this->invokePhase(Phase::AfterModelWrite);this->verifyModelPins();
    h.baseModelSha256=QByteArray::fromHex(sha256File(modelPath).toLatin1());
    rawBinaryModel(modelPath);RModel readback;readback.read(modelPath);
    if(!sameView(view,this->stateViewExtractor(readback)) || QByteArray::fromHex(sha256File(modelPath).toLatin1())!=h.baseModelSha256)
        fail(QStringLiteral("Trace base MODEL changed during readback."));
    this->invokePhase(Phase::AfterModelReadBack);
    this->verifyModelPins();
    const QString tracePath=QDir(this->directory).filePath(QString::fromLatin1(traceFileName));
    this->invokePhase(Phase::BeforeTraceHeaderWrite);requireNoSymlinkComponents(this->directory);
    QFile file(tracePath);if(QFileInfo::exists(tracePath) || QFileInfo(tracePath).isSymLink() || !file.open(QIODevice::ReadWrite|QIODevice::NewOnly))
        fail(QStringLiteral("Trace initialization refuses an existing or unowned file."));
    this->traceHeader=PiezoAcceptedTrace::writeHeader(file,h);file.close();this->invokePhase(Phase::AfterTraceHeaderReadBack);
    this->traceCursor=PiezoAcceptedTrace::initialCursor(this->traceHeader);this->cachedFullModel=std::move(baseline);
    this->lastObservedSequence=this->lastPublishedSequence=this->lastFullModelSequence=view.sequence;
    this->lastPublishedTimeS=this->lastFullModelTimeS=view.timeS;this->started=true;
    auto publication=this->stageTrace(*this->cachedFullModel,{view.sequence,view.timeS,view.dtS,false},true);
    try{publication->publish();publication->commit();}catch(...){(void)publication->rollback();throw;}
}

std::unique_ptr<RAcceptedStepPublication> NativeAcceptedCheckpointStore::stageTrace(
    const RModel &candidate,const RAcceptedStepIdentity &identity,bool baseline)
{
    if(!this->started || this->poisoned || this->activePublication)fail(QStringLiteral("Trace publication is not retryable or already active."));
    requireNoSymlinkComponents(this->directory);
    this->verifyModelPins();
    const QString markerPath=QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName));
    if(QFileInfo::exists(markerPath) || QFileInfo(markerPath).isSymLink())fail(QStringLiteral("Trace has unresolved PENDING authority."));
    const auto view=this->stateViewExtractor(candidate);validateView(view);
    const auto payload=this->traceOptions.extractPayload(candidate);const auto certified=this->traceOptions.certifyPayload(payload);validateView(certified);
    const auto &clock=candidate.getTimeSolver();
    if(!sameView(view,certified) || view.stateFingerprint32!=traceDigest(payload) || identity.sequence!=view.sequence ||
       identity.timeS!=view.timeS || identity.dtS!=view.dtS || identity.dtS!=this->checkpointContext.dtS ||
       identity.sequence>this->traceHeader.targetSteps || !clock.getEnabled() || clock.getNTimeSteps()!=this->traceHeader.targetSteps ||
       clock.getInputNTimeSteps()!=this->traceHeader.targetSteps || clock.getInputTimeStepSize()!=this->traceHeader.dt ||
       clock.getCurrentTimeStep()!=(identity.sequence?identity.sequence-1:0) || clock.getComputedTime()!=identity.timeS ||
       (!baseline && (identity.sequence!=this->lastObservedSequence+1 ||
         (identity.sequence==this->traceHeader.targetSteps && !identity.finalStep))) ||
       (baseline && identity.sequence!=this->traceHeader.baseSeq))fail(QStringLiteral("Trace candidate source, codec or original accepted clock differs."));
    this->verifyModelPins();
    auto state=std::make_unique<PublicationState>();state->identity=identity;state->expected=view;state->trace=state->due=true;
    state->oldTraceCursor=this->traceCursor;state->oldLatestBytes=this->cachedLatestBytes;state->oldLastValidBytes=this->cachedLastValidBytes;
    const auto snapshot=[this](const QString &name,const QByteArray &expected){PublicationState::PointerSnapshot result;
        const QString path=QDir(this->directory).filePath(name);const QFileInfo info(path);
        if(info.isSymLink() || (info.exists() && !info.isFile()) || info.exists()!=!expected.isEmpty() ||
           (info.exists() && readFile(path,64*1024)!=expected))fail(QStringLiteral("Trace committed pointer changed since its verified receipt."));
        result.exists=info.exists();result.bytes=expected;return result;};
    state->latest=snapshot(QStringLiteral("latest.json"),this->cachedLatestBytes);
    state->lastValid=snapshot(QStringLiteral("lastvalid.json"),this->cachedLastValidBytes);
    const double elapsed=identity.timeS-this->lastFullModelTimeS;
    state->fullModelDue=baseline || identity.finalStep ||
        (this->requiredPublicationPredicate && this->requiredPublicationPredicate(identity.sequence)) || identity.sequence-this->lastFullModelSequence>=this->checkpointContext.maxAcceptedSteps ||
        elapsed>=this->checkpointContext.maxSimulationSeconds || elapsed+identity.dtS>this->checkpointContext.maxSimulationSeconds;
    auto publication=std::make_unique<Publication>(this,std::move(state));this->activePublication=publication.get();auto &s=*publication->state;
    const QString token=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto evidence=[this,&s](const QString &name,const QByteArray &bytes){const QString path=QDir(this->directory).filePath(name);
        if(bytes.size()>64*1024 || QFileInfo::exists(path) || QFileInfo(path).isSymLink())fail(QStringLiteral("Trace rollback evidence cannot be replaced."));
        writeAtomic(path,bytes);if(readFile(path,64*1024)!=bytes)fail(QStringLiteral("Trace rollback evidence changed."));s.evidencePaths.append(path);return QString::fromLatin1(traceDigest(bytes).toHex());};
    this->invokePhase(Phase::BeforeEvidenceWrite);
    const auto backup=[&](const QString &name,const PublicationState::PointerSnapshot &p){QJsonObject b{{"exists",p.exists}};
        if(p.exists){const QString file=QStringLiteral("rollback-%1-%2.pointer").arg(token,name);b["file"]=file;b["sha256"]=evidence(file,p.bytes);}return b;};
    const QString evidenceName=QStringLiteral("rollback-evidence-%1.json").arg(token);
    const QJsonObject evidenceObject{{"schema","range.native-checkpoint-rollback-evidence/v2"},{"candidateSeq",double(identity.sequence)},
        {"sourceSha256",this->checkpointContext.sourceSha256},{"contextSha256",QString::fromLatin1(this->traceHeader.contextSha256.toHex())},
        {"latest",backup(QStringLiteral("latest"),s.latest)},{"lastvalid",backup(QStringLiteral("lastvalid"),s.lastValid)},
        {"traceFile",QString::fromLatin1(traceFileName)},{"traceHeaderSha256",QString::fromLatin1(this->traceHeader.headerSha32.toHex())},
        {"oldTraceCursor",cursorFields(s.oldTraceCursor)}};
    const QString evidenceSha=evidence(evidenceName,QJsonDocument(evidenceObject).toJson(QJsonDocument::Compact));this->invokePhase(Phase::AfterEvidenceReadBack);
    s.markerPath=markerPath;s.markerBytes=QJsonDocument(QJsonObject{{"schema","range.native-checkpoint-pending-publication/v2"},
        {"candidateSeq",double(identity.sequence)},{"contextSha256",QString::fromLatin1(this->traceHeader.contextSha256.toHex())},
        {"rollbackManifestFile",evidenceName},{"rollbackManifestSha256",evidenceSha}}).toJson(QJsonDocument::Compact);
    s.evidencePaths.append(markerPath);publication->ensureMarker();
    this->invokePhase(Phase::BeforeTraceAppend);
    const QString tracePath=QDir(this->directory).filePath(QString::fromLatin1(traceFileName));requireNoSymlinkComponents(this->directory);
    QFile trace(tracePath);if(QFileInfo(tracePath).isSymLink() || !trace.open(QIODevice::ReadWrite))fail(QStringLiteral("Owned trace cannot be appended."));
    if(trace.size()!=s.oldTraceCursor.byteEnd)fail(QStringLiteral("Trace changed before append; refusing to truncate an unowned tail."));
    s.traceTouched=true;
    s.newTraceCursor=PiezoAcceptedTrace::append(trace,this->traceHeader,s.oldTraceCursor,identity.sequence,identity.timeS,payload);
    this->invokePhase(Phase::AfterTraceAppend);const auto readback=PiezoAcceptedTrace::readRecord(trace,this->traceHeader,s.oldTraceCursor,s.newTraceCursor.byteEnd);
    if(readback.payload!=payload || !sameCursor(readback.cursor,s.newTraceCursor))fail(QStringLiteral("Staged trace successor changed after append."));
    trace.close();this->invokePhase(Phase::AfterTraceRecordReadBack);
    QString modelFile,modelHash;quint64 modelSequence=this->lastFullModelSequence;
    if(baseline){modelFile=this->traceHeader.baseModelFile;modelHash=QString::fromLatin1(this->traceHeader.baseModelSha256.toHex());s.nextFullModel=this->cachedFullModel;s.nextFullModelPin=this->baseModelPin;}
    else if(s.fullModelDue){modelFile=QStringLiteral("checkpoint-%1-%2.rbm").arg(identity.sequence,10,10,QChar('0')).arg(token);
        const QString path=QDir(this->directory).filePath(modelFile);this->invokePhase(Phase::BeforeModelWrite);
        if(QFileInfo::exists(path) || QFileInfo(path).isSymLink())fail(QStringLiteral("Immutable trace full MODEL already exists."));
        ExactModelWriter(candidate).writeBinary(path);s.nextFullModelPin=std::make_shared<const ModelPin>(path);
        this->invokePhase(Phase::AfterModelWrite);s.nextFullModelPin->verify();modelHash=sha256File(path);rawBinaryModel(path);
        RModel full;full.read(path);if(!sameView(view,this->stateViewExtractor(full)) || sha256File(path)!=modelHash)
            fail(QStringLiteral("Periodic trace full MODEL differs from its accepted source."));
        this->invokePhase(Phase::AfterModelReadBack);s.nextFullModelPin->verify();s.nextFullModel=std::make_shared<const RModel>(std::move(full));modelSequence=identity.sequence;
    }else {if(!this->receipt)fail(QStringLiteral("Trace periodic source receipt is missing."));
        modelFile=this->receipt->envelope.value(QStringLiteral("modelFile")).toString();modelHash=this->receipt->envelope.value(QStringLiteral("checkpointModelSha256")).toString();}
    QJsonObject envelope=this->checkpointContext.toJsonObject();envelope["schema"]=QString::fromLatin1(traceEnvelopeSchema);
    envelope.remove(QStringLiteral("modelSha256"));envelope["inputModelSha256"]=this->checkpointContext.modelSha256;
    envelope["acceptedSeq"]=double(identity.sequence);envelope["timeS"]=identity.timeS;envelope["dtS"]=identity.dtS;
    envelope["modelFile"]=modelFile;envelope["checkpointModelSha256"]=modelHash;envelope["modelAcceptedSeq"]=double(modelSequence);
    envelope["stateFingerprintSha256"]=QString::fromLatin1(view.stateFingerprint32.toHex());envelope["traceFile"]=QString::fromLatin1(traceFileName);
    envelope["traceHeaderSha256"]=QString::fromLatin1(this->traceHeader.headerSha32.toHex());envelope["traceCursor"]=cursorFields(s.newTraceCursor);
    const QString envelopeName=QStringLiteral("envelope-%1-%2.json").arg(identity.sequence,10,10,QChar('0')).arg(token);
    const QByteArray envelopeBytes=QJsonDocument(envelope).toJson(QJsonDocument::Compact);this->invokePhase(Phase::BeforeEnvelopeWrite);
    const QString envelopePath=QDir(this->directory).filePath(envelopeName);if(QFileInfo::exists(envelopePath) || QFileInfo(envelopePath).isSymLink())fail(QStringLiteral("Trace envelope already exists."));
    writeAtomic(envelopePath,envelopeBytes);this->invokePhase(Phase::AfterEnvelopeWrite);
    if(readFile(envelopePath,64*1024)!=envelopeBytes)fail(QStringLiteral("Trace envelope readback differs."));this->invokePhase(Phase::AfterEnvelopeReadBack);
    s.verifiedReceipt=std::make_unique<VerifiedCheckpoint>();s.verifiedReceipt->modelPath=QDir(this->directory).filePath(modelFile);
    s.verifiedReceipt->envelope=envelope;s.verifiedReceipt->modelSequence=modelSequence;s.verifiedReceipt->acceptedRuntime=payload;
    s.verifiedReceipt->acceptedModel=s.nextFullModel;s.verifiedReceipt->traceCursor=s.newTraceCursor;
    s.newPointerBytes=QJsonDocument(QJsonObject{{"schema",QString::fromLatin1(tracePointerSchema)},
        {"envelopeFile",envelopeName},{"envelopeSha256",QString::fromLatin1(traceDigest(envelopeBytes).toHex())}}).toJson(QJsonDocument::Compact);
    verifyStagedTrace(s);return publication;
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint NativeAcceptedCheckpointStore::readTraceEnvelope(const QJsonObject &e,bool lastValid) const
{
    requireOnlyKeys(e,{"schema","sourceSha256","inputModelSha256","controlSha256","runtimeSha256","packageSha256",
        "policy","maxAcceptedSteps","maxSimulationSeconds","acceptedSeq","timeS","dtS","modelFile","checkpointModelSha256",
        "modelAcceptedSeq","stateFingerprintSha256","traceFile","traceHeaderSha256","traceCursor"},QStringLiteral("Trace checkpoint envelope"));
    auto expected=this->checkpointContext.toJsonObject();expected.remove(QStringLiteral("schema"));expected.remove(QStringLiteral("modelSha256"));
    expected["inputModelSha256"]=this->checkpointContext.modelSha256;
    for(auto it=expected.begin();it!=expected.end();++it)if(e.value(it.key())!=it.value())fail(QStringLiteral("Trace envelope differs from frozen context."));
    const auto integer=[](const QJsonValue &v,quint64 maximum){const double x=v.toDouble(-1);
        if(!v.isDouble() || !std::isfinite(x) || x<0 || std::floor(x)!=x || x>double(maximum))fail(QStringLiteral("Trace envelope integer is invalid."));return quint64(x);};
    const quint64 accepted=integer(e.value(QStringLiteral("acceptedSeq")),std::numeric_limits<uint>::max());
    const quint64 modelSequence=integer(e.value(QStringLiteral("modelAcceptedSeq")),accepted);
    const QString modelName=e.value(QStringLiteral("modelFile")).toString(),modelHash=e.value(QStringLiteral("checkpointModelSha256")).toString();
    if(!safeBasename(modelName) || !modelName.endsWith(QStringLiteral(".rbm")) || !isLowerHexSha256(modelHash) ||
       e.value(QStringLiteral("traceFile")).toString()!=QString::fromLatin1(traceFileName) ||
       !isLowerHexSha256(e.value(QStringLiteral("traceHeaderSha256")).toString()) || !isLowerHexSha256(e.value(QStringLiteral("stateFingerprintSha256")).toString()) ||
       !e.value(QStringLiteral("timeS")).isDouble() || !std::isfinite(e.value(QStringLiteral("timeS")).toDouble()))
        fail(QStringLiteral("Trace full MODEL, state or time proof is invalid."));
    const auto cursor=e.value(QStringLiteral("traceCursor")).toObject();requireOnlyKeys(cursor,{"byteEnd","lastRecordOffset","sequence","recordSha256","stateSha256"},QStringLiteral("Trace committed cursor"));
    PiezoAcceptedTrace::Cursor authority;authority.byteEnd=qint64(integer(cursor.value(QStringLiteral("byteEnd")),PiezoAcceptedTrace::MaxFileBytes));
    authority.lastRecordOffset=qint64(integer(cursor.value(QStringLiteral("lastRecordOffset")),quint64(authority.byteEnd)));
    authority.sequence=integer(cursor.value(QStringLiteral("sequence")),accepted);
    for(const auto &key:{QStringLiteral("recordSha256"),QStringLiteral("stateSha256")})if(!isLowerHexSha256(cursor.value(key).toString()))fail(QStringLiteral("Trace cursor hash is invalid."));
    authority.recordSha32=QByteArray::fromHex(cursor.value(QStringLiteral("recordSha256")).toString().toLatin1());
    authority.stateSha32=QByteArray::fromHex(cursor.value(QStringLiteral("stateSha256")).toString().toLatin1());
    if(authority.sequence!=accepted || authority.stateSha32!=QByteArray::fromHex(e.value(QStringLiteral("stateFingerprintSha256")).toString().toLatin1()))
        fail(QStringLiteral("Trace cursor is not the latest accepted state."));
    const QString modelPath=QDir(this->directory).filePath(modelName);const auto fullPin=std::make_shared<const ModelPin>(modelPath);
    if(sha256File(modelPath)!=modelHash)fail(QStringLiteral("Periodic trace MODEL hash changed."));
    rawBinaryModel(modelPath);RModel full;full.read(modelPath);const auto fullView=this->stateViewExtractor(full);validateView(fullView);
    if(fullView.sequence!=modelSequence || sha256File(modelPath)!=modelHash)fail(QStringLiteral("Periodic full MODEL sequence or bytes changed."));
    const QString tracePath=QDir(this->directory).filePath(QString::fromLatin1(traceFileName));requireNoSymlinkComponents(this->directory);
    QFile trace(tracePath);if(QFileInfo(tracePath).isSymLink() || !QFileInfo(tracePath).isFile() || !trace.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Trace committed file is missing or linked."));
    this->invokePhase(Phase::BeforeTracePrefixScan);const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,authority);
    const auto &h=prefix.header;const auto &admitted=this->traceOptions.header;
    if(prefix.records<1 || h.headerSha32!=QByteArray::fromHex(e.value(QStringLiteral("traceHeaderSha256")).toString().toLatin1()) ||
       h.family!=admitted.family || h.sourceFingerprint!=admitted.sourceFingerprint || h.configurationIdentity!=admitted.configurationIdentity ||
       h.backendIdentity!=admitted.backendIdentity || h.initialTime!=admitted.initialTime || h.dt!=admitted.dt || h.targetSteps!=admitted.targetSteps ||
       h.targetSteps-h.baseSeq>=PiezoAcceptedTrace::MaxRecords ||
       h.contextSha256!=traceDigest(QJsonDocument(this->checkpointContext.toJsonObject()).toJson(QJsonDocument::Compact)) ||
       !safeBasename(h.baseModelFile) || !h.baseModelFile.endsWith(QStringLiteral(".rbm")))
        fail(QStringLiteral("Trace prefix header differs from its actual source, backend, configuration or context."));
    const QString basePath=QDir(this->directory).filePath(h.baseModelFile);const QString baseHash=QString::fromLatin1(h.baseModelSha256.toHex());
    const auto basePin=modelPath==basePath?fullPin:std::make_shared<const ModelPin>(basePath);
    if(sha256File(basePath)!=baseHash)fail(QStringLiteral("Trace immutable base MODEL hash changed."));rawBinaryModel(basePath);
    RModel base;base.read(basePath);const auto baseView=this->stateViewExtractor(base);validateView(baseView);
    if(baseView.sequence!=h.baseSeq || sha256File(basePath)!=baseHash)fail(QStringLiteral("Trace base MODEL differs from its original authority."));
    auto current=PiezoAcceptedTrace::initialCursor(h);bool fullWitness=false;QByteArray terminal;
    while(current.byteEnd<authority.byteEnd){const auto record=PiezoAcceptedTrace::readRecord(trace,h,current,authority.byteEnd);
        const auto view=this->traceOptions.certifyPayload(record.payload);validateView(view);
        if(view.sequence!=record.sequence || view.timeS!=record.time || view.dtS!=h.dt || view.stateFingerprint32!=record.cursor.stateSha32)
            fail(QStringLiteral("Trace record is not its actual complete accepted runtime."));
        if(record.sequence==h.baseSeq && !sameView(view,baseView))fail(QStringLiteral("Trace baseline payload differs from its complete source MODEL."));
        if(record.sequence==modelSequence){if(!sameView(view,fullView))fail(QStringLiteral("Trace periodic MODEL is not the claimed accepted state."));fullWitness=true;}
        terminal=record.payload;current=record.cursor;
    }
    if(!fullWitness || !sameCursor(current,authority) || prefix.terminal.time!=e.value(QStringLiteral("timeS")).toDouble())
        fail(QStringLiteral("Trace complete prefix, full MODEL or latest clock differs from its envelope."));
    this->invokePhase(Phase::AfterTracePrefixScan);
    requireNoSymlinkComponents(this->directory);QFile repeatedFile(tracePath);
    if(QFileInfo(tracePath).isSymLink() || !QFileInfo(tracePath).isFile() || !repeatedFile.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Trace source path changed during physical certification."));
    const auto repeated=PiezoAcceptedTrace::verifyPrefix(repeatedFile,authority);
    if(repeated.prefixSha32!=prefix.prefixSha32 || sha256File(modelPath)!=modelHash || sha256File(basePath)!=baseHash)
        fail(QStringLiteral("Trace source bytes changed during complete physical certification."));
    auto acceptedModel=std::make_shared<const RModel>(this->traceOptions.rebuildAcceptedModel(full,terminal));
    const auto rebuilt=this->stateViewExtractor(*acceptedModel);const auto acceptedView=this->traceOptions.certifyPayload(terminal);
    if(!sameView(rebuilt,acceptedView) || rebuilt.sequence!=accepted || rebuilt.timeS!=e.value(QStringLiteral("timeS")).toDouble())
        fail(QStringLiteral("Trace terminal MODEL rebuilder changed its complete accepted state."));
    // These checks deliberately follow all caller-controlled certification and
    // reconstruction callbacks. A callback cannot return a valid DTO and then
    // leave its persisted source changed after our previous hash checks.
    fullPin->verify();basePin->verify();requireNoSymlinkComponents(this->directory);
    QFile finalTrace(tracePath);
    if(QFileInfo(tracePath).isSymLink() || !QFileInfo(tracePath).isFile() || !finalTrace.open(QIODevice::ReadOnly) ||
       PiezoAcceptedTrace::verifyPrefix(finalTrace,authority).prefixSha32!=prefix.prefixSha32 ||
       sha256File(modelPath)!=modelHash || sha256File(basePath)!=baseHash)
        fail(QStringLiteral("Trace base/full MODEL or committed prefix changed during terminal reconstruction."));
    VerifiedCheckpoint result;result.modelPath=modelPath;result.envelope=e;result.usedLastValid=lastValid;result.modelSequence=modelSequence;
    result.acceptedRuntime=terminal;result.acceptedModel=std::move(acceptedModel);result.traceCursor=authority;return result;
}

void NativeAcceptedCheckpointStore::verifyStagedTrace(const PublicationState &s) const
{
    requireNoSymlinkComponents(this->directory);
    this->verifyModelPins();if(s.fullModelDue){if(!s.nextFullModelPin)fail(QStringLiteral("Staged full MODEL pin is missing."));s.nextFullModelPin->verify();}
    const auto pointer=parseObject(s.newPointerBytes,QStringLiteral("Staged trace pointer"),64*1024);
    const QString envelopePath=QDir(this->directory).filePath(pointer.value(QStringLiteral("envelopeFile")).toString());
    const auto envelopeBytes=readFile(envelopePath,64*1024);
    if(QString::fromLatin1(traceDigest(envelopeBytes).toHex())!=pointer.value(QStringLiteral("envelopeSha256")).toString() ||
       envelopeBytes!=QJsonDocument(s.verifiedReceipt->envelope).toJson(QJsonDocument::Compact))fail(QStringLiteral("Staged immutable trace envelope changed."));
    const QFileInfo model(s.verifiedReceipt->modelPath);if(model.isSymLink() || !model.isFile() || model.canonicalFilePath()!=model.absoluteFilePath())
        fail(QStringLiteral("Trace full MODEL source path changed."));
    if(s.fullModelDue && sha256File(s.verifiedReceipt->modelPath)!=s.verifiedReceipt->envelope.value(QStringLiteral("checkpointModelSha256")).toString())
        fail(QStringLiteral("Staged periodic full MODEL bytes changed before the decision."));
    QFile trace(QDir(this->directory).filePath(QString::fromLatin1(traceFileName)));
    if(QFileInfo(trace.fileName()).isSymLink() || !trace.open(QIODevice::ReadOnly) || trace.size()!=s.newTraceCursor.byteEnd)
        fail(QStringLiteral("Staged trace has a changed path, partial successor or uncommitted tail."));
    const auto header=PiezoAcceptedTrace::readHeader(trace);if(header.headerSha32!=this->traceHeader.headerSha32)fail(QStringLiteral("Staged trace header changed."));
    const auto record=PiezoAcceptedTrace::readRecord(trace,header,s.oldTraceCursor,s.newTraceCursor.byteEnd);
    if(!sameCursor(record.cursor,s.newTraceCursor) || record.payload!=s.verifiedReceipt->acceptedRuntime ||
       !sameView(s.expected,this->traceOptions.certifyPayload(record.payload)))fail(QStringLiteral("Staged trace accepted payload or actual source changed."));
    this->verifyModelPins();if(s.nextFullModelPin)s.nextFullModelPin->verify();
}

void NativeAcceptedCheckpointStore::verifyModelPins() const
{
    if(!this->baseModelPin || !this->currentModelPin)fail(QStringLiteral("Trace immutable MODEL authority is not pinned."));
    this->baseModelPin->verify();if(this->currentModelPin!=this->baseModelPin)this->currentModelPin->verify();
}

void NativeAcceptedCheckpointStore::accept(const RModel &acceptedModel)
{
    const auto view = this->stateViewExtractor(acceptedModel);
    const auto &clock = acceptedModel.getTimeSolver();
    const RAcceptedStepIdentity identity{view.sequence,view.timeS,view.dtS,
        quint64(clock.getCurrentTimeStep())+1 >= clock.getNTimeSteps()};
    // The old postcommit observer may be attached after earlier accepted steps.
    // The precommit participant always requires initial sequence + 1.
    auto publication = this->stageImpl(acceptedModel,identity,true);
    try
    {
        publication->publish();
        publication->commit();
    }
    catch (...)
    {
        (void)publication->rollback();
        throw;
    }
}

bool NativeAcceptedCheckpointStore::isRetryable() const noexcept
{
    return this->started && !this->poisoned;
}

const NativeAcceptedCheckpointStore::VerifiedCheckpoint *
NativeAcceptedCheckpointStore::committedReceipt() const noexcept
{
    return this->receipt.get();
}

void NativeAcceptedCheckpointStore::setPhaseHook(Phase phase, PhaseHook hook)
{
    this->hookPhase = phase;
    this->phaseHook = std::move(hook);
}

void NativeAcceptedCheckpointStore::setRequiredPublicationPredicate(std::function<bool(quint64)> predicate)
{
    if(this->activePublication || this->poisoned)
        fail(QStringLiteral("Cannot change required publication policy during an active or failed transaction."));
    this->requiredPublicationPredicate=std::move(predicate);
}

void NativeAcceptedCheckpointStore::invokePhase(Phase phase) const
{
    if (this->phaseHook && this->hookPhase == phase)
    {
        PhaseHook hook;
        hook.swap(this->phaseHook);
        hook(phase);
    }
}

QString NativeAcceptedCheckpointStore::rollbackDiagnostic() const
{
    if (!this->rollbackFailure) return {};
    try { std::rethrow_exception(this->rollbackFailure); }
    catch (const std::exception &error) { return QString::fromUtf8(error.what()); }
    catch (...) { return QStringLiteral("Native checkpoint rollback failed with an unknown exception."); }
}

const QStringList &NativeAcceptedCheckpointStore::rollbackEvidencePaths() const noexcept
{
    return this->failedRollbackEvidencePaths;
}

const NativeAcceptedCheckpointContext &NativeAcceptedCheckpointStore::context() const
{
    return this->checkpointContext;
}
