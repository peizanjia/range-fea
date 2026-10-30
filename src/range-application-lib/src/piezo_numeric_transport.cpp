#include "piezo_numeric_transport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <limits>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
QByteArray canonical(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject object(const QByteArray &bytes,const char *label){QJsonParseError e;const auto d=QJsonDocument::fromJson(bytes,&e);if(e.error!=QJsonParseError::NoError || !d.isObject())throw std::invalid_argument(label);return d.object();}
#ifndef Q_OS_WIN
QByteArray hashFile(const QString &path){
    QFile f(path);require(f.open(QIODevice::ReadOnly),"Frozen backend file unavailable");QCryptographicHash h(QCryptographicHash::Sha256);
    require(h.addData(&f) && f.error()==QFileDevice::NoError,"Frozen backend file hash failed");return h.result();
}
#endif
QString file(const QString &path){const QFileInfo f(path);require(f.isFile(),"Backend executable/module path is not a file");const auto p=f.canonicalFilePath();require(!p.isEmpty(),"Backend file has no canonical path");return p;}
QString identity(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
#ifdef Q_OS_WIN
QString nativeExtendedPath(const QString &path){
    auto native=QDir::toNativeSeparators(path);
    if(native.startsWith(QStringLiteral("\\\\?\\")))return native;
    native=QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if(native.startsWith(QStringLiteral("\\\\")))return QStringLiteral("\\\\?\\UNC\\")+native.mid(2);
    require(native.size()>=3 && native[0].isLetter() && native[1]==QLatin1Char(':') && native[2]==QLatin1Char('\\'),"Backend pin path must be absolute");
    return QStringLiteral("\\\\?\\")+native;
}
#endif
void requireNoReparseComponents(const QString &path){
    QString current=QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    auto normalized=QDir::fromNativeSeparators(current);
    if(normalized.startsWith("//?/UNC/",Qt::CaseInsensitive))normalized="//"+normalized.mid(8);
    else if(normalized.startsWith("//?/"))normalized=normalized.mid(4);
    QString root;
    if(normalized.startsWith("//")){
        const auto parts=normalized.mid(2).split('/',Qt::SkipEmptyParts);
        require(parts.size()>=2,"Backend UNC path must contain a server/share root");
        root="//"+parts[0]+"/"+parts[1];
    }else if(normalized.size()>=3 && normalized[1]==QLatin1Char(':'))root=normalized.left(3);
    else root="/";
    current=normalized;
    for(;;){
        const QFileInfo info(current);
        require(info.exists() && !info.isSymLink(),"Backend path contains a missing or symbolic-link component");
#ifdef Q_OS_WIN
        const auto native=nativeExtendedPath(current);const auto attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Backend path contains a reparse component");
#endif
        if(QDir::cleanPath(current)==QDir::cleanPath(root))break;
        const auto parent=QDir::cleanPath(info.absolutePath());require(parent!=current,"Backend path has an invalid root");current=parent;
    }
}
// This is a Windows share-mode pin, not a QFile ReadOnly promise. It blocks
// source updates until the provider is destroyed. Hashes remain compatibility
// identities; user excitation sources/dependencies are deliberately not pinned.
class SourcePin final {
    QString path;
#ifdef Q_OS_WIN
    HANDLE handle=INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION original{};
    HANDLE open() const {
        const auto native=nativeExtendedPath(path);
        return CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    }
    static bool equal(const BY_HANDLE_FILE_INFORMATION &a,const BY_HANDLE_FILE_INFORMATION &b){
        return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow &&
            a.nFileSizeHigh==b.nFileSizeHigh && a.nFileSizeLow==b.nFileSizeLow && a.dwFileAttributes==b.dwFileAttributes &&
            a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime && a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
    }
#endif
public:
    explicit SourcePin(QString value):path(std::move(value)){
#ifdef Q_OS_WIN
        handle=open();require(handle!=INVALID_HANDLE_VALUE,"Cannot pin backend source against existing writers");
        if(!GetFileInformationByHandle(handle,&original) || (original.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))){
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;throw std::invalid_argument("Backend source pin requires a regular file");
        }
#endif
    }
    SourcePin(const SourcePin &)=delete;
    SourcePin &operator=(const SourcePin &)=delete;
    ~SourcePin(){
#ifdef Q_OS_WIN
        if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
#endif
    }
    void verify(bool checkPath=false) const {
#ifdef Q_OS_WIN
        BY_HANDLE_FILE_INFORMATION held{},current{};
        require(GetFileInformationByHandle(handle,&held) && equal(held,original),"Pinned backend source metadata changed");
        if(!checkPath)return;
        const HANDLE reopened=open();require(reopened!=INVALID_HANDLE_VALUE,"Pinned backend source path cannot be reopened");
        const bool same=GetFileInformationByHandle(reopened,&current) && equal(current,original);CloseHandle(reopened);
        require(same,"Pinned backend source file identity changed");
#else
        Q_UNUSED(checkPath);
#endif
    }
    QByteArray sha256() const {
#ifdef Q_OS_WIN
        verify(true);LARGE_INTEGER beginning{};require(SetFilePointerEx(handle,beginning,nullptr,FILE_BEGIN),"Cannot seek pinned backend source");
        QCryptographicHash hash(QCryptographicHash::Sha256);char buffer[64*1024];DWORD bytes=0;
        for(;;){require(ReadFile(handle,buffer,sizeof(buffer),&bytes,nullptr),"Cannot read pinned backend source");if(!bytes)break;hash.addData(buffer,int(bytes));}
        verify(true);return hash.result();
#else
        return hashFile(path);
#endif
    }
};
#ifdef Q_OS_WIN
// Share-read handles keep every admitted file's directory chain in place.
// Attribute-only writes are still possible, so reparse attributes are checked
// on the held handles each time. Directory timestamps are never a cache key.
class DirectoryPin final {
    HANDLE handle=INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION original{};
public:
    explicit DirectoryPin(const QString &path){
        const auto native=nativeExtendedPath(path);
        handle=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(handle!=INVALID_HANDLE_VALUE,"Cannot pin backend directory against existing writers");
        if(!GetFileInformationByHandle(handle,&original) || !(original.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
           (original.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;throw std::invalid_argument("Backend directory pin requires a non-reparse directory");
        }
    }
    DirectoryPin(const DirectoryPin &)=delete;
    DirectoryPin &operator=(const DirectoryPin &)=delete;
    ~DirectoryPin(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    void verify() const {
        BY_HANDLE_FILE_INFORMATION held{};
        require(GetFileInformationByHandle(handle,&held) && held.dwVolumeSerialNumber==original.dwVolumeSerialNumber &&
            held.nFileIndexHigh==original.nFileIndexHigh && held.nFileIndexLow==original.nFileIndexLow &&
            (held.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(held.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),
            "Pinned backend directory identity/reparse attributes changed");
    }
};
#endif
class BusinessRejection final:public std::runtime_error {public:using std::runtime_error::runtime_error;};
struct OperationProfile {
    qint64 calls=0,failures=0,total=0,pre=0,serialization=0,pipe=0,post=0;
};
class RequestTiming final {
public:
    enum Phase {Pre,Serialization,Pipe,Post};
private:
    OperationProfile *profile;
    QElapsedTimer timer;
    qint64 previous=0;
    Phase phase=Pre;
    int exceptions=std::uncaught_exceptions();
    bool failed=false;
    void finishPhase(qint64 now){
        const auto duration=now-previous;
        switch(phase){case Pre:profile->pre+=duration;break;case Serialization:profile->serialization+=duration;break;
            case Pipe:profile->pipe+=duration;break;case Post:profile->post+=duration;break;}
        previous=now;
    }
public:
    explicit RequestTiming(OperationProfile *value):profile(value){if(profile)timer.start();}
    void fail() noexcept {failed=true;}
    void begin(Phase value){if(profile){finishPhase(timer.nsecsElapsed());phase=value;}}
    ~RequestTiming(){if(profile){const auto now=timer.nsecsElapsed();finishPhase(now);profile->total+=now;++profile->calls;
        if(failed || std::uncaught_exceptions()>exceptions)++profile->failures;}}
};
}
struct PiezoNumericTransport::Impl {
    QProcess process;
    Qt::HANDLE owner=QThread::currentThreadId();
    std::map<QString,QByteArray> frozen;
    std::map<QString,std::unique_ptr<SourcePin>> pins;
#ifdef Q_OS_WIN
    std::map<QString,std::unique_ptr<DirectoryPin>> directoryPins;
    void pinDirectory(const QString &path){
        if(directoryPins.find(path)==directoryPins.end())directoryPins.emplace(path,std::make_unique<DirectoryPin>(path));
    }
    void pinDirectoryChain(const QString &path){
        auto current=QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        if(current.startsWith("//?/UNC/",Qt::CaseInsensitive))current="//"+current.mid(8);
        else if(current.startsWith("//?/"))current=current.mid(4);
        QString root;
        if(current.startsWith("//")){const auto parts=current.mid(2).split('/',Qt::SkipEmptyParts);
            require(parts.size()>=2,"Backend UNC path must contain a server/share root");root="//"+parts[0]+"/"+parts[1];}
        else {require(current.size()>=3 && current[1]==QLatin1Char(':'),"Backend directory pin requires an absolute path");root=current.left(3);}
        QStringList chain;
        for(;;){chain.prepend(current);if(QDir::cleanPath(current)==QDir::cleanPath(root))break;
            const auto parent=QDir::cleanPath(QFileInfo(current).absolutePath());require(parent!=current,"Backend directory has an invalid root");current=parent;}
        for(const auto &directory:chain)pinDirectory(directory);
    }
#endif
    bool profiling=qEnvironmentVariableIntValue("RANGE_PIEZO_TRANSPORT_PROFILE")==1;
    qint64 startupNanos=0;
    std::map<QString,OperationProfile> profiles;
    QByteArray pending,stderrBytes;
    QString moduleRoot,packageRoot;
    std::set<QString> packageNames;
    QString python;
    bool healthy=true;
    bool requestPending=false,pendingColdRecheck=false;
    QElapsedTimer requestDeadline;
    std::unique_ptr<RequestTiming> requestTiming;
    static constexpr qint64 maxBytes=16*1024*1024;
    void stop() noexcept {process.kill();process.waitForFinished(5000);healthy=false;}
    std::map<QString,QString> inventory(bool freezeDirectories=false) {
#ifdef Q_OS_WIN
        std::map<QString,QString> result;
        const auto visit=[&](auto &&self,const QString &directory,const QString &prefix)->void {
            if(freezeDirectories)pinDirectory(directory);
            else if(directoryPins.find(directory)==directoryPins.end())requireNoReparseComponents(directory);
            const auto pattern=nativeExtendedPath(QDir(directory).filePath("*"));WIN32_FIND_DATAW entry{};
            const HANDLE handle=FindFirstFileExW(reinterpret_cast<LPCWSTR>(pattern.utf16()),FindExInfoBasic,&entry,FindExSearchNameMatch,nullptr,0);
            if(handle==INVALID_HANDLE_VALUE){require(GetLastError()==ERROR_FILE_NOT_FOUND,"Backend package directory cannot be enumerated");return;}
            struct Enumeration {HANDLE handle;~Enumeration(){FindClose(handle);}} enumeration{handle};
            for(;;){
                const auto name=QString::fromWCharArray(entry.cFileName);
                if(name!="." && name!=".."){
                    require(!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Backend package contains a reparse entry");
                    const auto relative=prefix+name;const auto candidate=QDir(directory).filePath(name);
                    if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)self(self,candidate,relative+'/');
                    else if(!relative.split('/').contains("__pycache__") && !relative.endsWith(".pyc")){
                        require(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DEVICE),"Backend package entry is not a regular file");
                        require(result.emplace(relative,candidate).second,"Backend package has duplicate relative names");
                    }
                }
                if(!FindNextFileW(handle,&entry)){require(GetLastError()==ERROR_NO_MORE_FILES,"Backend package enumeration failed");break;}
            }
        };
        visit(visit,packageRoot,QString{});return result;
#else
        Q_UNUSED(freezeDirectories);
        requireNoReparseComponents(packageRoot);
        std::map<QString,QString> result;
        QDirIterator entries(packageRoot,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        while(entries.hasNext()){
            const auto candidate=entries.next();const auto info=entries.fileInfo();
            require(!info.isSymLink(),"Backend package contains a symbolic link");
#ifdef Q_OS_WIN
            const auto native=nativeExtendedPath(candidate);const auto attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
            require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Backend package contains a reparse entry");
#endif
            if(info.isDir())continue;
            const auto relative=QDir(packageRoot).relativeFilePath(candidate);
            require(!relative.startsWith("../") && !QDir::isAbsolutePath(relative),"Backend module escaped explicit module root");
            if(relative.split('/').contains("__pycache__") || relative.endsWith(".pyc"))continue;
            require(info.isFile(),"Backend package entry is not a regular file");
            require(result.emplace(relative,candidate).second,"Backend package has duplicate relative names");
        }
        return result;
#endif
    }
    void verifySources(bool fullHash=false){
        try{
#ifdef Q_OS_WIN
            for(const auto &entry:directoryPins)entry.second->verify();
#endif
            const auto current=inventory();std::set<QString> names;for(const auto &entry:current)names.insert(entry.first);
            require(names==packageNames,"Backend package inventory changed since freeze");
#ifndef Q_OS_WIN
            requireNoReparseComponents(python);
#endif
            for(const auto &entry:pins){
                entry.second->verify(fullHash);
#ifdef Q_OS_WIN
                if(fullHash)
#else
                Q_UNUSED(fullHash);
#endif
                    require(entry.second->sha256()==frozen.at(entry.first),"Backend executable/module source changed since freeze");
            }
        }catch(...){stop();throw;}
    }
public:
    QString runtimeIdentity;
    QJsonObject metadata;
    Impl(const QString &executable,const QString &root){
        const bool startupTracing=qEnvironmentVariableIntValue("RANGE_PIEZO_TRANSPORT_STARTUP_TRACE")==1;
        QElapsedTimer startup;if(profiling || startupTracing)startup.start();
        const auto trace=[&](const char *phase){if(startupTracing)qInfo().noquote()<<"PIEZO_TRANSPORT_STARTUP"<<phase<<"elapsedMs"<<startup.elapsed();};
        trace("constructor_begin");trace("before_path_validation");
        requireNoReparseComponents(executable);requireNoReparseComponents(root);
        python=file(executable);const QFileInfo rootInfo(root);
        require(rootInfo.isDir() && !rootInfo.canonicalFilePath().isEmpty(),"Backend module root must be an explicit existing directory");
        moduleRoot=rootInfo.canonicalFilePath();
        const QDir package(QDir(moduleRoot).filePath("range_workflow"));
        packageRoot=package.path();
        const auto modules=package.entryList({"*.py"},QDir::Files,QDir::Name);
        require(modules.contains("__init__.py") && modules.contains("piezo_native_backend.py") && modules.contains("piezo_design.py") && modules.contains("piezo_control_design.py") && modules.contains("piezo_excitation.py"),"Backend package is incomplete");
        trace("after_path_validation");
#ifdef Q_OS_WIN
        trace("before_directory_pins");
        pinDirectoryChain(QFileInfo(python).absolutePath());pinDirectoryChain(packageRoot);
        trace("after_directory_pins");
#endif
        trace("before_inventory");const auto entries=inventory(true);trace("after_inventory");
        trace("before_source_pins");pins.emplace(python,std::make_unique<SourcePin>(python));
        for(const auto &entry:entries) {
            const auto path=file(entry.second);
            require(QDir(moduleRoot).relativeFilePath(path).startsWith("range_workflow/"),"Backend module escaped explicit module root");
            packageNames.insert(entry.first);pins.emplace(path,std::make_unique<SourcePin>(path));
        }
        trace("after_source_pins");
        // All authority handles precede the first byte read and runtime launch.
        trace("before_freeze_hashes");
        for(const auto &entry:pins)frozen.emplace(entry.first,entry.second->sha256());
        trace("after_freeze_hashes");trace("before_source_verify");verifySources();trace("after_source_verify");
#ifdef Q_OS_WIN
        // GetBinaryType may classify arbitrary short .exe bytes as DOS. Accept
        // native Windows runtimes only, before entering synchronous OS launch.
        // This is a format filter, not a guarantee that a runtime can execute.
        trace("before_executable_preflight");
        DWORD binaryType=0;const auto nativePython=nativeExtendedPath(python);
        require(GetBinaryTypeW(reinterpret_cast<LPCWSTR>(nativePython.utf16()),&binaryType) &&
            (binaryType==SCS_32BIT_BINARY || binaryType==SCS_64BIT_BINARY),
            "Private numerical backend requires a native Windows executable");
        trace("after_executable_preflight");
#endif
        QByteArray witness;for(const auto &entry:frozen){witness.append(entry.first.toUtf8());witness.append(char(0));witness.append(entry.second);}
        runtimeIdentity=identity(witness);
        auto env=QProcessEnvironment::systemEnvironment();for(const auto *key:{"PYTHONPATH","PYTHONHOME","PYTHONSTARTUP"})env.remove(key);
        for(const auto *key:{"OMP_NUM_THREADS","OPENBLAS_NUM_THREADS","MKL_NUM_THREADS","NUMEXPR_NUM_THREADS"})env.insert(key,"1");
        process.setProcessEnvironment(env);process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
        // The numerical worker uses inherited NDJSON pipes without attaching
        // a console. Keep QProcess ownership and its standard-stream handles.
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments){
            arguments->flags=(arguments->flags & ~DWORD(CREATE_NO_WINDOW)) | DETACHED_PROCESS;
        });
#endif
        trace("before_process_start");
        process.start(python,{"-I","-B","-c","import sys;sys.path.insert(0,sys.argv[1]);import json,platform,numpy,scipy;print(json.dumps({'ok':True,'result':{'python':platform.python_version(),'pythonBuild':sys.version,'pythonExecutable':sys.executable,'numpy':numpy.__version__,'scipy':scipy.__version__}}),flush=True);from range_workflow.piezo_native_backend import main;raise SystemExit(main(['--stdio']))",moduleRoot});
        trace("after_process_start");trace("before_wait_started");
        require(process.waitForStarted(10000),"Private numerical backend failed to start");
        trace("after_wait_started");trace("before_handshake_receive");metadata=receive();trace("after_handshake_receive");
        trace("before_handshake_source_verify");verifySources();trace("after_handshake_source_verify");
        for(const auto *key:{"python","pythonBuild","numpy","scipy"})require(metadata.value(key).isString() && !metadata.value(key).toString().isEmpty(),"Backend version handshake missing");
        metadata["executable"]=python; metadata["moduleRoot"]=moduleRoot;
        metadata["packageAndExecutableSha256"]=runtimeIdentity;
        runtimeIdentity=identity(witness+canonical(metadata));
        if(profiling)startupNanos=startup.nsecsElapsed();
        trace("constructor_complete");

    }
    ~Impl(){if(requestPending){if(requestTiming)requestTiming->fail();requestTiming.reset();requestPending=false;}if(process.state()!=QProcess::NotRunning)stop();
        if(profiling){QJsonObject operations;for(const auto &entry:profiles){const auto &p=entry.second;
            operations[entry.first]=QJsonObject{{"calls",p.calls},{"failures",p.failures},{"totalNs",p.total},{"preGuardNs",p.pre},
                {"serializationNs",p.serialization},{"pipeNs",p.pipe},{"postGuardNs",p.post}};}
            qInfo().noquote()<<"PIEZO_TRANSPORT_PROFILE"<<QJsonDocument(QJsonObject{{"startupNs",startupNanos},{"operations",operations}}).toJson(QJsonDocument::Compact);}}
    void abortPendingRequest(){
        require(QThread::currentThreadId()==owner,"Backend abort must execute on its owner thread");
        if(!requestPending)return;
        stop();requestPending=false;
        if(requestTiming)requestTiming->fail();requestTiming.reset();
    }
    void beginRequest(QJsonObject message){
        require(QThread::currentThreadId()==owner,"Backend callbacks must execute on their owner thread");
        require(!requestPending,"Backend already has a pending request");
        auto operation=message.value("op").toString();
        if(operation!="initialize" && operation!="force" && operation!="propose" && operation!="postFE" && operation!="validate" && operation!="certificationBatch")operation="other";
        const bool emptyBatch=message.size()==3 && operation=="certificationBatch" && message.value("records").isArray() &&
            message.value("records").toArray().isEmpty() && message.value("times").isArray() && message.value("times").toArray().isEmpty();
        if(emptyBatch)operation="certificationBatchEmpty";
        requestPending=true;pendingColdRecheck=emptyBatch;
        requestTiming=std::make_unique<RequestTiming>(profiling?&profiles[operation]:nullptr);
        auto &timing=*requestTiming;
        try{
        require(healthy,"Backend transport is no longer retryable");
        const bool coldRecheck=emptyBatch;
        verifySources(coldRecheck);
        timing.begin(RequestTiming::Serialization);
        const auto bytes=canonical(message);require(bytes.size()<=maxBytes,"Backend request exceeds bounded line size");
        if(!pending.isEmpty() || process.bytesAvailable()!=0){stop();throw std::runtime_error("Backend emitted unsolicited output");}
        timing.begin(RequestTiming::Pipe);
        requestDeadline.start();auto &timer=requestDeadline;const QByteArray line=bytes+'\n';
        if(process.write(line)!=line.size()){stop();throw std::runtime_error("Backend request write failed");}
        while(process.bytesToWrite()){
            const auto remaining=60000-timer.elapsed();
            if(remaining<=0){stop();throw std::runtime_error("Backend request write timed out");}
            if(!process.waitForBytesWritten(int(std::min<qint64>(1000,remaining)))){
                if(process.state()==QProcess::NotRunning || timer.elapsed()>=60000){stop();throw std::runtime_error("Backend request write timed out");}
            }
        }
        require(timer.elapsed()<60000,"Backend request write timed out");
        }catch(...){abortPendingRequest();throw;}
    }
    QJsonObject finishRequest(){
        require(QThread::currentThreadId()==owner,"Backend callbacks must execute on their owner thread");
        require(requestPending && bool(requestTiming),"Backend has no pending request");
        auto &timing=*requestTiming;const bool coldRecheck=pendingColdRecheck;
        try{
            require(requestDeadline.elapsed()<60000,"Backend pending request deadline expired");
            QJsonObject result;
            try{result=receive(60000-requestDeadline.elapsed());}
            catch(const BusinessRejection &){
                timing.begin(RequestTiming::Post);verifySources(coldRecheck);
                require(requestDeadline.elapsed()<60000,"Backend pending request deadline expired");
                timing.fail();requestPending=false;requestTiming.reset();throw;
            }
            catch(...){timing.begin(RequestTiming::Post);if(healthy)verifySources(coldRecheck);throw;}
            timing.begin(RequestTiming::Post);verifySources(coldRecheck);
            require(requestDeadline.elapsed()<60000,"Backend pending request deadline expired");
            requestPending=false;requestTiming.reset();return result;
        }catch(...){abortPendingRequest();throw;}
    }
    QJsonObject request(QJsonObject message){beginRequest(std::move(message));return finishRequest();}
    QJsonObject receive(qint64 deadline=60000){
        QElapsedTimer timer;timer.start();
        const auto remaining=[&]{const auto local=deadline-timer.elapsed();
            return requestPending?std::min<qint64>(local,60000-requestDeadline.elapsed()):local;};
        const auto checkDeadline=[&]{if(remaining()<=0){stop();throw std::runtime_error("Backend response unavailable or timed out");}};
        for(;;){
            checkDeadline();
            pending+=process.readAllStandardOutput();stderrBytes+=process.readAllStandardError();
            if(pending.size()>maxBytes || stderrBytes.size()>1024*1024){stop();throw std::runtime_error("Backend output exceeded bounded size");}
            const auto newline=pending.indexOf('\n');
            if(newline>=0){const auto answer=pending.left(newline);pending.remove(0,newline+1);
                if(!pending.isEmpty()){stop();throw std::runtime_error("Backend emitted more than one response");}
                QJsonObject response;
                try {response=object(answer,"Backend response is not a JSON object");}
                catch(...) {stop();throw;}
                checkDeadline();
                if(!response.value("ok").isBool()){stop();throw std::runtime_error("Backend response has no boolean status");}
                if(!response.value("ok").toBool()) {
                    if(response.size()!=2 || !response.value("error").isString()){stop();throw std::runtime_error("Backend rejection response shape invalid");}
                    checkDeadline();throw BusinessRejection(qPrintable(QStringLiteral("Numerical backend rejected request: ")+response.value("error").toString()));
                }
                if(response.size()!=2 || !response.value("result").isObject()){stop();throw std::runtime_error("Backend successful response shape invalid");}
                checkDeadline();return response.value("result").toObject();
            }
            if(process.state()==QProcess::NotRunning || remaining()<=0){const auto error=QString::fromUtf8(stderrBytes.left(2048));stop();throw std::runtime_error(qPrintable(QStringLiteral("Backend response unavailable or timed out: ")+error));}
            const auto waitMs=remaining();if(waitMs<=0)checkDeadline();
            process.waitForReadyRead(int(std::min<qint64>(1000,waitMs)));
        }
    }
};

PiezoNumericTransport::PiezoNumericTransport(const QString &python,const QString &root):impl(std::make_unique<Impl>(python,root)){}
PiezoNumericTransport::~PiezoNumericTransport()=default;
QJsonObject PiezoNumericTransport::request(QJsonObject value){return impl->request(std::move(value));}
void PiezoNumericTransport::beginRequest(QJsonObject value){impl->beginRequest(std::move(value));}
QJsonObject PiezoNumericTransport::finishRequest(){return impl->finishRequest();}
void PiezoNumericTransport::abortPendingRequest(){impl->abortPendingRequest();}
const QString &PiezoNumericTransport::runtimeIdentity() const noexcept{return impl->runtimeIdentity;}
const QJsonObject &PiezoNumericTransport::runtimeMetadata() const noexcept{return impl->metadata;}
std::pair<QString,QString> PiezoNumericTransport::defaultRuntime(){
    const QDir installed(QCoreApplication::applicationDirPath()+QStringLiteral("/.."));
    QString python=installed.filePath(QStringLiteral("runtime/python/python.exe"));
    QString modules=installed.filePath(QStringLiteral("share/range-workflow"));
    if(!QFileInfo(python).isFile() || !QFileInfo(QDir(modules).filePath("range_workflow/piezo_native_backend.py")).isFile()){
#if defined(RANGE_PIEZO_BACKEND_PYTHON) && defined(RANGE_PIEZO_BACKEND_MODULE_ROOT)
        python=QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON); modules=QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT);
#else
        throw std::invalid_argument("Installed private numerical backend unavailable and no compiled runtime configured");
#endif
    }
    return {python,modules};
}
