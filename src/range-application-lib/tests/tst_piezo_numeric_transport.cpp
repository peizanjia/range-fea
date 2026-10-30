#include <QtTest>
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTemporaryDir>
#include <QtEndian>
#include <QThread>
#include <memory>
#include <map>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif
#include "piezo_numeric_transport.h"

#ifndef RANGE_PIEZO_BACKEND_PYTHON
#error "Transport qualification requires the private numerical runtime"
#endif

namespace {
QStringList *profileMessages=nullptr;
void captureProfile(QtMsgType,const QMessageLogContext &,const QString &message)
{
    if(profileMessages && message.startsWith("PIEZO_TRANSPORT_PROFILE "))profileMessages->append(message);
}
class ProfileCapture final {
    QByteArray previous=qgetenv("RANGE_PIEZO_TRANSPORT_PROFILE");
    bool wasSet=qEnvironmentVariableIsSet("RANGE_PIEZO_TRANSPORT_PROFILE");
    QtMessageHandler handler;
public:
    QStringList messages;
    ProfileCapture(){profileMessages=&messages;qputenv("RANGE_PIEZO_TRANSPORT_PROFILE","1");handler=qInstallMessageHandler(captureProfile);}
    ~ProfileCapture(){qInstallMessageHandler(handler);profileMessages=nullptr;
        if(wasSet)qputenv("RANGE_PIEZO_TRANSPORT_PROFILE",previous);else qunsetenv("RANGE_PIEZO_TRANSPORT_PROFILE");}
};
void writeFile(const QString &path,const QByteArray &bytes)
{
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size())
        throw std::runtime_error("Cannot prepare isolated protocol fixture");
}
void package(const QTemporaryDir &directory)
{
    const auto root=directory.filePath("range_workflow");
    if(!QDir().mkpath(root))throw std::runtime_error("Cannot prepare protocol package");
    for(const auto *name:{"__init__.py","piezo_design.py","piezo_control_design.py","piezo_excitation.py"})
        writeFile(QDir(root).filePath(name),{});
    writeFile(QDir(root).filePath("piezo_native_backend.py"),R"PY(import json,sys,pathlib
def main(argv):
    for line in sys.stdin:
        request=json.loads(line)
        kind=request.get('kind')
        if kind=='exit_early':
            print('fixture premature exit',file=sys.stderr,flush=True)
            return 17
        if kind=='partial_then_exit':
            sys.stdout.write('{"ok":true')
            sys.stdout.flush()
            return 19
        if kind=='stderr_then_valid':
            sys.stderr.write('fixture diagnostic\n'*4096)
            sys.stderr.flush()
        if kind=='stderr_beyond_bound':
            for _ in range(64):
                sys.stderr.write('x'*32768)
                sys.stderr.flush()
            continue
        if kind=='malformed': response='not-json'
        elif kind=='array': response='[]'
        elif kind=='status': response=json.dumps({'ok':1,'result':{}})
        elif kind=='success_shape': response=json.dumps({'ok':True,'result':[]})
        elif kind=='success_extra': response=json.dumps({'ok':True,'result':{},'extra':1})
        elif kind=='rejection_shape': response=json.dumps({'ok':False,'error':{}})
        elif kind=='rejection_extra': response=json.dumps({'ok':False,'error':'invalid','extra':1})
        elif kind=='duplicate': response='{"ok":true,"result":{}}\n{"ok":true,"result":{}}'
        elif kind=='reject': response=json.dumps({'ok':False,'error':'qualified business rejection'})
        elif kind=='large_reply': response=json.dumps({'ok':True,'result':{'payload':'x'*(2*1024*1024)}})
        elif kind=='mutate_before_reply':
            pathlib.Path(__file__).with_name('added_during_reply.py').write_text('# added during reply\n')
            response=json.dumps({'ok':True,'result':{'accepted':True}})
        else: response=json.dumps({'ok':True,'result':{'accepted':True}})
        print(response,flush=True)
        if kind in ('valid','reject'):
            pathlib.Path(__file__).resolve().parent.parent.joinpath('reply_published_'+kind).write_text('ready')
    return 0
)PY");
}
QString failure(PiezoNumericTransport &transport,const QString &kind)
{
    try { transport.request({{"kind",kind}}); }
    catch(const std::exception &e) { return QString::fromUtf8(e.what()); }
    return {};
}
#ifdef Q_OS_WIN
HANDLE attributeWriter(const QString &path,bool directory)
{
    const auto native=QDir::toNativeSeparators(path);
    return CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
}
bool mountPoint(const QString &path,const QString &target)
{
    const auto substitute=QStringLiteral("\\??\\")+QDir::toNativeSeparators(target);
    const auto print=QDir::toNativeSeparators(target);
    const QByteArray substituteBytes(reinterpret_cast<const char *>(substitute.utf16()),substitute.size()*2);
    const QByteArray printBytes(reinterpret_cast<const char *>(print.utf16()),print.size()*2);
    QByteArray data(16,char(0));
    qToLittleEndian<quint32>(IO_REPARSE_TAG_MOUNT_POINT,reinterpret_cast<uchar *>(data.data()));
    qToLittleEndian<quint16>(quint16(8+substituteBytes.size()+printBytes.size()+4),reinterpret_cast<uchar *>(data.data()+4));
    qToLittleEndian<quint16>(quint16(substituteBytes.size()),reinterpret_cast<uchar *>(data.data()+10));
    qToLittleEndian<quint16>(quint16(substituteBytes.size()+2),reinterpret_cast<uchar *>(data.data()+12));
    qToLittleEndian<quint16>(quint16(printBytes.size()),reinterpret_cast<uchar *>(data.data()+14));
    data+=substituteBytes;data+=QByteArray(2,char(0));data+=printBytes;data+=QByteArray(2,char(0));
    const auto handle=attributeWriter(path,true);if(handle==INVALID_HANDLE_VALUE)return false;
    DWORD returned=0;const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,data.data(),DWORD(data.size()),nullptr,0,&returned,nullptr);
    CloseHandle(handle);return ok;
}
#endif
}

class TestPiezoNumericTransport:public QObject
{
    Q_OBJECT
private slots:
    void prematureExitPoisonsPipe_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("eof-without-response")<<QString("exit_early");
        QTest::newRow("eof-with-partial-response")<<QString("partial_then_exit");
    }
    void prematureExitPoisonsPipe()
    {
        QFETCH(QString,kind);
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        QElapsedTimer timer;timer.start();
        const auto diagnostic=failure(transport,kind);
        QVERIFY2(!diagnostic.isEmpty(),"An exited worker must not produce a successful response");
        if(kind=="exit_early")QVERIFY2(diagnostic.contains("fixture premature exit"),qPrintable(diagnostic));
        QVERIFY2(timer.elapsed()<10000,qPrintable(diagnostic));
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void stderrDoesNotCorruptNdjsonOrRetries()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        QElapsedTimer timer;timer.start();
        QVERIFY(transport.request({{"kind","stderr_then_valid"}})["accepted"].toBool());
        QVERIFY2(timer.elapsed()<10000,"Bounded stderr must drain without stalling the response");
        QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
        QVERIFY(failure(transport,"reject").contains("qualified business rejection"));
        QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
    }
    void stderrBeyondBoundPoisonsTransport()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        QElapsedTimer timer;timer.start();
        const auto diagnostic=failure(transport,"stderr_beyond_bound");
        QVERIFY2(diagnostic.contains("Backend output exceeded bounded size"),qPrintable(diagnostic));
        QVERIFY2(timer.elapsed()<10000,"Oversized stderr must fail without a pipe deadlock");
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void unavailableExecutableFailsBeforeRequests()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        QString diagnostic;
        try{PiezoNumericTransport transport(directory.filePath("missing-private-python.exe"),directory.path());}
        catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
        QVERIFY2(!diagnostic.isEmpty(),"An unavailable executable must fail construction");
    }
    void invalidProtocolDisablesRetry_data()
    {
        QTest::addColumn<QString>("kind");
        for(const auto *kind:{"malformed","array","status","success_shape","success_extra","rejection_shape","rejection_extra","duplicate"})
            QTest::newRow(kind)<<QString::fromLatin1(kind);
    }
    void invalidProtocolDisablesRetry()
    {
        QFETCH(QString,kind);
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        QVERIFY(!failure(transport,kind).isEmpty());
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void validBusinessRejectionAllowsRetry()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        QVERIFY(failure(transport,"reject").contains("qualified business rejection"));
        QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
    }
    void splitBusinessRejectionRetryAndBalancedProfile()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);ProfileCapture capture;
        {
            PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
            transport.beginRequest({{"kind","reject"}});QString diagnostic;
            try{transport.finishRequest();}catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
            QVERIFY(diagnostic.contains("qualified business rejection"));
            transport.beginRequest({{"kind","valid"}});QVERIFY(transport.finishRequest()["accepted"].toBool());
            QVERIFY(transport.request({{"op","certificationBatch"},{"records",QJsonArray{}},{"times",QJsonArray{}}})["accepted"].toBool());
        }
        QCOMPARE(capture.messages.size(),1);const auto text=capture.messages.first();
        const auto operations=QJsonDocument::fromJson(text.mid(text.indexOf('{')).toUtf8()).object()["operations"].toObject();
        const auto ordinary=operations["other"].toObject();QCOMPARE(ordinary["calls"].toInteger(),qint64(2));QCOMPARE(ordinary["failures"].toInteger(),qint64(1));
        const auto empty=operations["certificationBatchEmpty"].toObject();QCOMPARE(empty["calls"].toInteger(),qint64(1));QCOMPARE(empty["failures"].toInteger(),qint64(0));
        for(const auto &record:{ordinary,empty}){
            QCOMPARE(record["totalNs"].toInteger(),record["preGuardNs"].toInteger()+record["serializationNs"].toInteger()+record["pipeNs"].toInteger()+record["postGuardNs"].toInteger());
            QVERIFY(record["preGuardNs"].toInteger()>0);QVERIFY(record["postGuardNs"].toInteger()>0);
        }
    }
    void splitSourceChangeBetweenBeginAndFinish()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        transport.beginRequest({{"kind","valid"}});writeFile(directory.filePath("range_workflow/added_while_pending.py"),"# changed while pending\n");
        QString diagnostic;try{transport.finishRequest();}catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
        QVERIFY2(diagnostic.contains("inventory changed"),qPrintable(diagnostic));QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void splitMalformedReplyPoisonsTransport_data(){invalidProtocolDisablesRetry_data();}
    void splitMalformedReplyPoisonsTransport()
    {
        QFETCH(QString,kind);QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());transport.beginRequest({{"kind",kind}});
        QString diagnostic;try{transport.finishRequest();}catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
        QVERIFY(!diagnostic.isEmpty());QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void splitLateBufferedReplyDeadlinePoisonsTransport_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("valid-reply")<<QString("valid");
        QTest::newRow("business-rejection")<<QString("reject");
    }
    void splitLateBufferedReplyDeadlinePoisonsTransport()
    {
        QFETCH(QString,kind);QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto replyPublished=directory.filePath("reply_published_"+kind);
        auto transport=std::make_unique<PiezoNumericTransport>(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        transport->beginRequest({{"kind",kind}});
        QElapsedTimer publishWait;publishWait.start();
        while(!QFileInfo::exists(replyPublished) && publishWait.elapsed()<10000)QTest::qWait(10);
        QVERIFY2(QFileInfo::exists(replyPublished),"Private Python fixture did not publish the complete reply");
        QTest::qWait(61000);
        QString diagnostic;try{transport->finishRequest();}catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
        QVERIFY2(diagnostic.contains("deadline"),qPrintable(diagnostic));
        QCOMPARE(failure(*transport,"valid"),QString("Backend transport is no longer retryable"));
#ifdef Q_OS_WIN
        QFile pinned(directory.filePath("range_workflow/piezo_native_backend.py"));
        QVERIFY(!pinned.open(QIODevice::Append));
#endif
        transport.reset();
        QFile released(directory.filePath("range_workflow/piezo_native_backend.py"));
        QVERIFY(released.open(QIODevice::Append));released.close();
    }
    void splitBackpressureAndOwnedAbort()
    {
        QTemporaryDir first,second;QVERIFY(first.isValid());QVERIFY(second.isValid());package(first);package(second);
        auto transport=std::make_unique<PiezoNumericTransport>(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),first.path());
        PiezoNumericTransport independent(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),second.path());
        transport->beginRequest({{"kind","large_reply"}});QThread::msleep(50);
        QCOMPARE(transport->finishRequest()["payload"].toString().size(),qsizetype(2*1024*1024));
        transport->beginRequest({{"kind","large_reply"}});QThread::msleep(50);transport->abortPendingRequest();
        QCOMPARE(failure(*transport,"valid"),QString("Backend transport is no longer retryable"));
        QVERIFY(independent.request({{"kind","valid"}})["accepted"].toBool());transport.reset();
        QFile released(first.filePath("range_workflow/piezo_native_backend.py"));QVERIFY(released.open(QIODevice::Append));released.close();
    }
    void identityUsesActualSourceBytes()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        std::map<QString,QByteArray> hashes;
        const QDir sources(directory.filePath("range_workflow"));
        QStringList paths{QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON)};
        for(const auto &name:sources.entryList(QDir::Files))paths.append(sources.filePath(name));
        for(const auto &path:paths){
            QFile source(path);QVERIFY(source.open(QIODevice::ReadOnly));
            QCryptographicHash hash(QCryptographicHash::Sha256);QVERIFY(hash.addData(&source));
            hashes.emplace(QFileInfo(path).canonicalFilePath(),hash.result());
        }
        QByteArray witness;for(const auto &entry:hashes){witness.append(entry.first.toUtf8());witness.append(char(0));witness.append(entry.second);}
        QCOMPARE(transport.runtimeMetadata()["packageAndExecutableSha256"].toString(),
                 QString::fromLatin1(QCryptographicHash::hash(witness,QCryptographicHash::Sha256).toHex()));
        QCOMPARE(transport.runtimeIdentity(),QString::fromLatin1(QCryptographicHash::hash(
            witness+QJsonDocument(transport.runtimeMetadata()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()));
    }
    void extendedDrivePaths()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto python=QStringLiteral("\\\\?\\")+QDir::toNativeSeparators(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON));
        const auto modules=QStringLiteral("\\\\?\\")+QDir::toNativeSeparators(directory.path());
        PiezoNumericTransport transport(python,modules);
        QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
#else
        QSKIP("Windows extended drive path qualification");
#endif
    }
    void packageWriteDeleteRenameAndRelease()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto path=directory.filePath("range_workflow/piezo_native_backend.py");
        auto transport=std::make_unique<PiezoNumericTransport>(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto identity=transport->runtimeIdentity();
        const auto metadata=transport->runtimeMetadata();
        QFile source(path);
#ifdef Q_OS_WIN
        QVERIFY(!source.open(QIODevice::Append));
        QVERIFY(!QFile::remove(path));
        QVERIFY(!QFile::rename(path,path+".renamed"));
        QVERIFY(transport->request({{"kind","valid"}})["accepted"].toBool());
        QCOMPARE(transport->runtimeIdentity(),identity);QCOMPARE(transport->runtimeMetadata(),metadata);
        QVERIFY(transport->request({{"op","certificationBatch"},{"records",QJsonArray{}},{"times",QJsonArray{}}})["accepted"].toBool());
        transport.reset();
        QVERIFY(source.open(QIODevice::Append));source.close();
        QVERIFY(QFile::rename(path,path+".renamed"));QVERIFY(QFile::remove(path+".renamed"));
#else
        QVERIFY(source.open(QIODevice::Append));source.write("\n# changed source\n");source.close();
        QVERIFY(!failure(*transport,"valid").isEmpty());
        QCOMPARE(failure(*transport,"valid"),QString("Backend transport is no longer retryable"));
#endif
    }
    void existingWriterPreventsInitialization_data()
    {
        QTest::addColumn<bool>("executable");QTest::newRow("package-writer")<<false;QTest::newRow("executable-writer")<<true;
    }
    void existingWriterPreventsInitialization()
    {
#ifdef Q_OS_WIN
        QFETCH(bool,executable);QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        auto python=QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON);
        if(executable){const auto copy=directory.filePath("copied-python.exe");QVERIFY(QFile::copy(python,copy));python=copy;}
        QFile writer(executable?python:directory.filePath("range_workflow/piezo_native_backend.py"));
        QVERIFY(writer.open(QIODevice::WriteOnly|QIODevice::Append));
        QString diagnostic;try{PiezoNumericTransport transport(python,directory.path());}catch(const std::exception &e){diagnostic=QString::fromUtf8(e.what());}
        QVERIFY2(diagnostic.contains("Cannot pin backend source against existing writers"),qPrintable(diagnostic));
#else
        QSKIP("Share-mode lifetime pins are specific to Windows");
#endif
    }
    void newRelativeNamePoisonsTransport()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto added=directory.filePath("range_workflow/added.py");writeFile(added,"# added source\n");
        QVERIFY(failure(transport,"valid").contains("inventory changed"));
        QVERIFY(QFile::remove(added));
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void sameFileAliasIsNotCollapsed()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto source=QDir::toNativeSeparators(directory.filePath("range_workflow/piezo_native_backend.py"));
        const auto alias=QDir::toNativeSeparators(directory.filePath("range_workflow/alias.py"));
        const bool linked=CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()),reinterpret_cast<LPCWSTR>(source.utf16()),nullptr);
        if(linked){QVERIFY(failure(transport,"valid").contains("inventory changed"));QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));}
        else{QCOMPARE(GetLastError(),DWORD(ERROR_SHARING_VIOLATION));QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());}
#else
        QSKIP("Windows hard-link alias/share-mode qualification");
#endif
    }
    void pathReplacementCannotRetainAdmission()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto old=directory.filePath("range_workflow");const auto moved=directory.filePath("moved_package");
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        if(QDir().rename(old,moved)){
            package(directory);const auto diagnostic=failure(transport,"valid");
            QVERIFY2(diagnostic.contains("file identity changed"),qPrintable(diagnostic));
            QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
        }else{
            QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
        }
#else
        QSKIP("Windows path-to-file-ID pin qualification");
#endif
    }
    void changeBeforeReplyIsRejectedBeforeAdmission()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        QVERIFY(failure(transport,"mutate_before_reply").contains("inventory changed"));
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
    }
    void directoryAncestorsArePinnedAndReleased()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto nested=directory.filePath("range_workflow/nested");QVERIFY(QDir().mkpath(nested));writeFile(QDir(nested).filePath("source.txt"),"frozen");
        auto transport=std::make_unique<PiezoNumericTransport>(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto renamed=directory.path()+"-renamed";
        QVERIFY(!QDir().rename(directory.path(),renamed));
        QVERIFY(!QDir().rename(directory.filePath("range_workflow"),directory.filePath("moved_package")));
        QVERIFY(!QDir().rename(nested,nested+"-renamed"));
        QVERIFY(transport->request({{"kind","valid"}})["accepted"].toBool());
        transport.reset();
        QVERIFY(QDir().rename(directory.path(),renamed));QVERIFY(QDir().rename(renamed,directory.path()));
        QVERIFY(QDir().rename(nested,nested+"-renamed"));QVERIFY(QDir().rename(nested+"-renamed",nested));
#else
        QSKIP("Windows lifetime directory share-mode pins");
#endif
    }
    void directoryTimestampResetCannotHideInventoryChange()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto root=directory.filePath("range_workflow");
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto handle=attributeWriter(root,true);QVERIFY(handle!=INVALID_HANDLE_VALUE);
        FILETIME original{};const bool read=GetFileTime(handle,nullptr,nullptr,&original);
        writeFile(QDir(root).filePath("added.py"),"# changed inventory\n");
        const bool restored=SetFileTime(handle,nullptr,nullptr,&original);CloseHandle(handle);
        QVERIFY(read);QVERIFY(restored);
        QVERIFY(failure(transport,"valid").contains("inventory changed"));
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
#else
        QSKIP("Windows attribute-only writes bypass share-mode pins");
#endif
    }
    void heldFileTimestampChangePoisonsTransport()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto handle=attributeWriter(directory.filePath("range_workflow/piezo_native_backend.py"),false);QVERIFY(handle!=INVALID_HANDLE_VALUE);
        FILETIME original{};const bool read=GetFileTime(handle,nullptr,nullptr,&original);
        ULARGE_INTEGER time{};time.HighPart=original.dwHighDateTime;time.LowPart=original.dwLowDateTime;time.QuadPart+=10000000;
        const FILETIME changed{time.LowPart,time.HighPart};const bool updated=SetFileTime(handle,nullptr,nullptr,&changed);CloseHandle(handle);
        QVERIFY(read);QVERIFY(updated);
        QVERIFY(failure(transport,"valid").contains("metadata changed"));
        QCOMPARE(failure(transport,"valid"),QString("Backend transport is no longer retryable"));
#else
        QSKIP("Windows held-file attribute qualification");
#endif
    }
    void regularCacheAndEmptyDirectoryChangesRemainAllowed()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const auto cache=directory.filePath("range_workflow/__pycache__/nested");QVERIFY(QDir().mkpath(cache));
        const auto cached=QDir(cache).filePath("module.py");writeFile(cached,"# excluded regular cached file\n");
        const auto pyc=directory.filePath("range_workflow/ignored.pyc");writeFile(pyc,"compiled bytes");
        const auto empty=directory.filePath("range_workflow/new_empty");QVERIFY(QDir().mkpath(empty));
        QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
        QVERIFY(QFile::remove(cached));QVERIFY(QFile::remove(pyc));QVERIFY(QDir().rmdir(cache));QVERIFY(QDir().rmdir(empty));
        QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
        QVERIFY(transport.request({{"op","certificationBatch"},{"records",QJsonArray{}},{"times",QJsonArray{}}})["accepted"].toBool());
    }
    void attributeOnlyReparseConversionOfPinnedCacheIsRejected()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);
        const auto cache=directory.filePath("range_workflow/__pycache__"),target=directory.filePath("outside_target");
        QVERIFY(QDir().mkpath(cache));QVERIFY(QDir().mkpath(target));
        auto transport=std::make_unique<PiezoNumericTransport>(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
        const bool converted=mountPoint(cache,target);
        const auto diagnostic=converted?failure(*transport,"valid"):QString{};
        const auto retry=converted?failure(*transport,"valid"):QString{};
        transport.reset();
        if(converted){const auto native=QDir::toNativeSeparators(cache);QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(native.utf16())));}
        QVERIFY2(converted,"Attribute-only FSCTL_SET_REPARSE_POINT fixture failed");
        QVERIFY2(diagnostic.contains("reparse"),qPrintable(diagnostic));
        QCOMPARE(retry,QString("Backend transport is no longer retryable"));
#else
        QSKIP("Windows attribute-only reparse mutation qualification");
#endif
    }
    void aggregateProfileIncludesGuardsFailuresAndEmptyBatch()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());package(directory);ProfileCapture capture;
        {
            PiezoNumericTransport transport(QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),directory.path());
            QVERIFY(transport.request({{"kind","valid"}})["accepted"].toBool());
            QVERIFY(failure(transport,"reject").contains("qualified business rejection"));
            QVERIFY(transport.request({{"op","certificationBatch"},{"records",QJsonArray{}},{"times",QJsonArray{}}})["accepted"].toBool());
            writeFile(directory.filePath("range_workflow/profile_added.py"),"# changed inventory\n");
            QVERIFY(failure(transport,"valid").contains("inventory changed"));
        }
        QCOMPARE(capture.messages.size(),1);
        const auto text=capture.messages.first();QJsonParseError error;
        const auto report=QJsonDocument::fromJson(text.mid(text.indexOf('{')).toUtf8(),&error).object();QCOMPARE(error.error,QJsonParseError::NoError);
        QVERIFY(report["startupNs"].toInteger()>0);
        const auto operations=report["operations"].toObject();QCOMPARE(operations.size(),2);
        const auto ordinary=operations["other"].toObject(),empty=operations["certificationBatchEmpty"].toObject();
        QCOMPARE(ordinary["calls"].toInteger(),qint64(3));QCOMPARE(ordinary["failures"].toInteger(),qint64(2));
        QCOMPARE(empty["calls"].toInteger(),qint64(1));QCOMPARE(empty["failures"].toInteger(),qint64(0));
        for(const auto &record:{ordinary,empty}){
            const auto phases=record["preGuardNs"].toInteger()+record["serializationNs"].toInteger()+
                record["pipeNs"].toInteger()+record["postGuardNs"].toInteger();
            QCOMPARE(record["totalNs"].toInteger(),phases);QVERIFY(record["preGuardNs"].toInteger()>0);QVERIFY(record["postGuardNs"].toInteger()>0);
        }
        QVERIFY(!text.contains("profile_added.py"));QVERIFY(!text.contains("qualified business rejection"));
    }
};
QTEST_GUILESS_MAIN(TestPiezoNumericTransport)
#include "tst_piezo_numeric_transport.moc"
