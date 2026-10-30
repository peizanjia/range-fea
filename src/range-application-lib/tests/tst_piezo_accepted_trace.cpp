#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <piezo_accepted_trace.h>
#include <rpiezo_experiment_service.h>
#include <rstructural_piezo_axial_native_run.h>
#include <rstructural_piezo_layered_native_run.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rbl_error.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

namespace {
using Trace=PiezoAcceptedTrace;
QByteArray hash(const QByteArray &b){return QCryptographicHash::hash(b,QCryptographicHash::Sha256);}
QByteArray bits(double value){return QByteArray(reinterpret_cast<const char*>(&value),8);}
void put(QByteArray &bytes,qsizetype offset,quint64 value){for(uint i=0;i<8;++i)bytes[offset+i]=char((value>>(8*i))&255);}
void putReal(QByteArray &bytes,qsizetype offset,double value){quint64 raw=0;std::memcpy(&raw,&value,8);put(bytes,offset,raw);}
void rehashRecord(QByteArray &bytes,const Trace::Cursor &c){const auto digest=hash(bytes.mid(c.lastRecordOffset+8,c.byteEnd-c.lastRecordOffset-40));bytes.replace(c.byteEnd-32,32,digest);}
template<class F> bool rejects(F f){try{f();}catch(const std::exception &){return true;}catch(const RError &){return true;}return false;}
Trace::Header header(quint64 target=4,quint64 base=0,double initial=-0.){
    Trace::Header h;h.family="layered";h.sourceFingerprint=QString(64,'a');h.configurationIdentity=QString(64,'b');h.backendIdentity="integrity-only-test";h.baseModelFile="source/base.rbm";
    h.baseModelSha256=QByteArray(32,'m');h.contextSha256=QByteArray(32,'c');h.baseSeq=base;h.initialTime=initial;h.dt=.125;h.targetSteps=target;h.targetTimeGridSha256=Trace::targetTimeGridSha256(h.initialTime,h.dt,h.targetSteps);return h;
}
struct Image{QByteArray bytes;Trace::Header header;QVector<Trace::Cursor> cursors;};
Image image(quint64 base=0){Image x;QBuffer file(&x.bytes);if(!file.open(QIODevice::ReadWrite))throw std::runtime_error("Buffer open");x.header=Trace::writeHeader(file,header(base+4,base));auto c=Trace::initialCursor(x.header);for(quint64 seq=base;seq<base+3;++seq){const double t=seq?x.header.initialTime+double(seq)*x.header.dt:x.header.initialTime;c=Trace::append(file,x.header,c,seq,t,QByteArray("opaque-storage-")+QByteArray::number(seq));x.cursors.append(c);}return x;}
Trace::Prefix verify(QByteArray &bytes,const Trace::Cursor &cursor,bool allowTail=false){QBuffer file(&bytes);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Buffer open");return Trace::verifyPrefix(file,cursor,allowTail);}
class OversizedBuffer:public QBuffer{public:using QBuffer::QBuffer;qint64 size()const override{return Trace::MaxFileBytes+1;}};
class WriteMutation:public QBuffer{
public:using QBuffer::QBuffer;bool armed=false;qsizetype mutationOffset=0;
protected:qint64 writeData(const char *data,qint64 count)override{const auto n=QBuffer::writeData(data,count);if(armed&&n>0){armed=false;buffer()[mutationOffset]=char(uchar(buffer()[mutationOffset])^1);}return n;}
};
class ReadMutation:public QBuffer{
public:using QBuffer::QBuffer;bool armed=true;qint64 afterOffset=0;qsizetype mutationOffset=0;
protected:qint64 readData(char *data,qint64 count)override{const auto begin=pos();const auto n=QBuffer::readData(data,count);if(armed&&n>0&&begin+n>=afterOffset){armed=false;buffer()[mutationOffset]=char(uchar(buffer()[mutationOffset])^1);}return n;}
};
RModel actualSource(bool axial){
    RModel empty;empty.getTimeSolver().setInputStartTime(.001);auto recipe=RPiezoExperimentService::exampleRecipe(empty,axial?"axialRod":"planarBeam");recipe["replaceLegacyConditions"]=true;recipe["steps"]=3;
    const auto result=RPiezoExperimentService::preview(empty,recipe);if(!result.valid)throw std::runtime_error(result.message.toStdString());auto m=result.candidate;
    if(axial){auto &s=m.getProblemSetup().getStructuralPiezoAxialSetup();s.recipeConfiguration=QJsonDocument(QJsonObject{{"family","axialRod"}}).toJson(QJsonDocument::Compact);s.bindToModel(m);}
    else{auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();s.controllerConfiguration.clear();s.designConfiguration.clear();s.excitationConfiguration.clear();s.bindToModel(m);}return m;
}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoAcceptedTrace:public QObject{
    Q_OBJECT
private slots:
    void signedZeroRestartBaseAndFileRoundtrip();void headerAndGridLimits();void payloadFileAndRecordLimits();
    void everyTruncation();void corruptRecordParts();void changedHeaderAndCursor();void explicitTailAndReadbackMutation();
    void actualNativeCodecRoundtrip_data();void actualNativeCodecRoundtrip();
};
void TestPiezoAcceptedTrace::signedZeroRestartBaseAndFileRoundtrip() try{
    auto x=image();const auto proof=verify(x.bytes,x.cursors.back());QCOMPARE(proof.records,quint64(3));QCOMPARE(proof.terminal.payload,QByteArray("opaque-storage-2"));QCOMPARE(proof.prefixSha32,hash(x.bytes));
    QBuffer file(&x.bytes);QVERIFY(file.open(QIODevice::ReadOnly));const auto h=Trace::readHeader(file);QCOMPARE(bits(h.initialTime),bits(-0.));const auto first=Trace::readRecord(file,h,Trace::initialCursor(h));QCOMPARE(bits(first.time),bits(-0.));
    auto signedZeroBytes=x.bytes.left(h.byteEnd);QBuffer writable(&signedZeroBytes);QVERIFY(writable.open(QIODevice::ReadWrite));QVERIFY(rejects([&]{Trace::append(writable,h,Trace::initialCursor(h),0,+0.,"signed-zero");}));
    auto restart=image(19);const auto restored=verify(restart.bytes,restart.cursors.back());QCOMPARE(restored.header.baseSeq,quint64(19));QCOMPARE(restored.cursor.sequence,quint64(21));
    QTemporaryDir dir;QVERIFY(dir.isValid());QFile disk(dir.filePath("accepted.trace"));QVERIFY(disk.open(QIODevice::ReadWrite));const auto dh=Trace::writeHeader(disk,header());const auto dc=Trace::append(disk,dh,Trace::initialCursor(dh),0,-0.,"native-payload");disk.close();QVERIFY(disk.open(QIODevice::ReadOnly));QCOMPARE(Trace::verifyPrefix(disk,dc).terminal.payload,QByteArray("native-payload"));
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::headerAndGridLimits() try{
    const auto good=header();for(uint which=0;which<12;++which){auto h=good;if(which==0)h.schema=2;if(which==1)h.family="solid";if(which==2)h.sourceFingerprint="short";if(which==3)h.configurationIdentity=QString(64,'z');if(which==4)h.baseModelSha256.resize(31);if(which==5)h.baseModelFile="../base.rbm";if(which==6)h.baseModelFile="C:/base.rbm";if(which==7)h.dt=0;if(which==8)h.baseSeq=h.targetSteps+1;if(which==9)h.backendIdentity=QString(int(Trace::MaxHeaderBytes),'x');if(which==10)h.targetTimeGridSha256=QByteArray(32,'x');if(which==11)h.initialTime=std::numeric_limits<double>::infinity();QByteArray bytes;QBuffer file(&bytes);QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(rejects([&]{Trace::writeHeader(file,h);}));QVERIFY(bytes.isEmpty());}
    QByteArray small;QBuffer baseline(&small);QVERIFY(baseline.open(QIODevice::ReadWrite));const auto ordinary=Trace::writeHeader(baseline,good);auto maximum=good;maximum.backendIdentity=QString(int(good.backendIdentity.size()+Trace::MaxHeaderBytes-ordinary.byteEnd),'x');QByteArray largest;QBuffer bounded(&largest);QVERIFY(bounded.open(QIODevice::ReadWrite));QCOMPARE(Trace::writeHeader(bounded,maximum).byteEnd,Trace::MaxHeaderBytes);
    QVERIFY(rejects([&]{Trace::targetTimeGridSha256(1e100,.001,4);}));QVERIFY(rejects([&]{Trace::targetTimeGridSha256(0,1,1000001);}));
    QByteArray target;for(quint64 i=1;i<=good.targetSteps;++i){quint64 raw=0;const double t=good.initialTime+double(i)*good.dt;std::memcpy(&raw,&t,8);const auto begin=target.size();target.resize(begin+8);put(target,begin,raw);}QCOMPARE(Trace::targetTimeGridSha256(good.initialTime,good.dt,good.targetSteps),hash(target));
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::payloadFileAndRecordLimits() try{
    QByteArray bytes;QBuffer file(&bytes);QVERIFY(file.open(QIODevice::ReadWrite));const auto h=Trace::writeHeader(file,header());const auto empty=Trace::initialCursor(h);QVERIFY(rejects([&]{Trace::append(file,h,empty,0,-0.,{});}));QVERIFY(rejects([&]{Trace::append(file,h,empty,0,-0.,QByteArray(int(Trace::MaxPayloadBytes+1),'p'));}));QCOMPARE(file.size(),h.byteEnd);
    const QByteArray maximum(int(Trace::MaxPayloadBytes),'p');const auto c=Trace::append(file,h,empty,0,-0.,maximum);QCOMPARE(Trace::verifyPrefix(file,c).terminal.payload,maximum);
    OversizedBuffer huge;QVERIFY(huge.open(QIODevice::ReadOnly));QVERIFY(rejects([&]{Trace::readHeader(huge);}));
    QByteArray many;QBuffer records(&many);QVERIFY(records.open(QIODevice::ReadWrite));const auto mh=Trace::writeHeader(records,header(25000));auto last=Trace::initialCursor(mh);for(quint64 seq=0;seq<Trace::MaxRecords;++seq)last=Trace::append(records,mh,last,seq,seq?double(seq)*mh.dt:mh.initialTime,"p");QCOMPARE(Trace::verifyPrefix(records,last).records,Trace::MaxRecords);QVERIFY(rejects([&]{Trace::append(records,mh,last,Trace::MaxRecords,double(Trace::MaxRecords)*mh.dt,"p");}));
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::everyTruncation() try{
    auto x=image();for(qsizetype cut=0;cut<x.bytes.size();++cut){auto bytes=x.bytes.left(cut);QVERIFY2(rejects([&]{verify(bytes,x.cursors.back());}),qPrintable(QString("Truncation %1 admitted").arg(cut)));}
    for(qsizetype cut=x.header.byteEnd;cut<x.cursors[0].byteEnd;++cut){auto bytes=x.bytes.left(cut);QBuffer file(&bytes);QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(rejects([&]{Trace::readRecord(file,x.header,Trace::initialCursor(x.header));}));}
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::corruptRecordParts() try{
    auto x=image();const auto c=x.cursors[0];for(uint which=0;which<6;++which){auto bytes=x.bytes;if(which==0)put(bytes,c.lastRecordOffset,quint64(Trace::MaxPayloadBytes)+97);if(which==1)put(bytes,c.lastRecordOffset+24,std::numeric_limits<quint64>::max());if(which==2)bytes[c.lastRecordOffset+32]=char(uchar(bytes[c.lastRecordOffset+32])^1);if(which==3){bytes[c.byteEnd-64]=char(uchar(bytes[c.byteEnd-64])^1);rehashRecord(bytes,c);}if(which==4)bytes[c.byteEnd-1]=char(uchar(bytes[c.byteEnd-1])^1);if(which==5){putReal(bytes,c.lastRecordOffset+16,+0.);rehashRecord(bytes,c);}QVERIFY(rejects([&]{verify(bytes,x.cursors.back());}));}
    // Rehash otherwise-valid successor frames to isolate sequence order guards.
    for(quint64 sequence:{quint64(0),quint64(2)}){auto bytes=x.bytes;const auto next=x.cursors[1];put(bytes,next.lastRecordOffset+8,sequence);putReal(bytes,next.lastRecordOffset+16,sequence?double(sequence)*x.header.dt:x.header.initialTime);rehashRecord(bytes,next);QBuffer file(&bytes);QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(rejects([&]{Trace::readRecord(file,x.header,c);}));}
    auto bytes=x.bytes;const auto first=x.bytes.mid(x.cursors[0].lastRecordOffset,x.cursors[0].byteEnd-x.cursors[0].lastRecordOffset),second=x.bytes.mid(x.cursors[1].lastRecordOffset,x.cursors[1].byteEnd-x.cursors[1].lastRecordOffset);bytes.replace(x.header.byteEnd,first.size()+second.size(),second+first);QVERIFY(rejects([&]{verify(bytes,x.cursors.back());}));
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::changedHeaderAndCursor() try{
    auto x=image();for(qsizetype offset:{qsizetype(0),qsizetype(8),qsizetype(16),qsizetype(x.header.byteEnd-1)}){auto bytes=x.bytes;bytes[offset]=char(uchar(bytes[offset])^1);QVERIFY(rejects([&]{verify(bytes,x.cursors.back());}));}
    for(uint which=0;which<6;++which){auto cursor=x.cursors.back();if(which==0)--cursor.byteEnd;if(which==1)++cursor.byteEnd;if(which==2)++cursor.lastRecordOffset;if(which==3)--cursor.sequence;if(which==4)cursor.recordSha32[0]=char(uchar(cursor.recordSha32[0])^1);if(which==5)cursor.stateSha32[0]=char(uchar(cursor.stateSha32[0])^1);auto bytes=x.bytes;QVERIFY(rejects([&]{verify(bytes,cursor);}));QBuffer append(&bytes);QVERIFY(append.open(QIODevice::ReadWrite));const auto next=cursor.sequence+1;QVERIFY(rejects([&]{Trace::append(append,x.header,cursor,next,double(next)*x.header.dt,"p");}));}
    auto bytes=x.bytes;QBuffer file(&bytes);QVERIFY(file.open(QIODevice::ReadWrite));auto changed=x.header;changed.backendIdentity="changed";QVERIFY(rejects([&]{Trace::append(file,changed,x.cursors.back(),3,.375,"p");}));
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::explicitTailAndReadbackMutation() try{
    auto x=image();const auto original=verify(x.bytes,x.cursors.back());x.bytes+="uncommitted malformed tail";QVERIFY(rejects([&]{verify(x.bytes,x.cursors.back());}));const auto diagnostic=verify(x.bytes,x.cursors.back(),true);QCOMPARE(diagnostic.prefixSha32,original.prefixSha32);QCOMPARE(diagnostic.terminal.payload,original.terminal.payload);QCOMPARE(diagnostic.uncommittedTailBytes,qint64(26));QBuffer tail(&x.bytes);QVERIFY(tail.open(QIODevice::ReadWrite));QVERIFY(rejects([&]{Trace::append(tail,x.header,x.cursors.back(),3,.375,"p");}));
    for(bool mutateHeader:{false,true}){QByteArray bytes;WriteMutation file(&bytes);QVERIFY(file.open(QIODevice::ReadWrite));const auto h=Trace::writeHeader(file,header());file.mutationOffset=mutateHeader?0:qsizetype(h.byteEnd+32);file.armed=true;QVERIFY(rejects([&]{Trace::append(file,h,Trace::initialCursor(h),0,-0.,"payload");}));}
    // Append checks its trusted old terminal, not every earlier byte. Full
    // admission must still reject a damaged earlier record after such append.
    auto behind=image();behind.bytes[behind.cursors[0].lastRecordOffset+32]=char(uchar(behind.bytes[behind.cursors[0].lastRecordOffset+32])^1);QBuffer terminalOnly(&behind.bytes);QVERIFY(terminalOnly.open(QIODevice::ReadWrite));const auto newlyAppended=Trace::append(terminalOnly,behind.header,behind.cursors.back(),3,.375,"p");QVERIFY(rejects([&]{Trace::verifyPrefix(terminalOnly,newlyAppended);}));
    auto y=image();ReadMutation duringRead(&y.bytes);duringRead.afterOffset=y.cursors[0].byteEnd;duringRead.mutationOffset=qsizetype(y.cursors[0].lastRecordOffset+32);QVERIFY(duringRead.open(QIODevice::ReadOnly));QVERIFY(rejects([&]{Trace::verifyPrefix(duringRead,y.cursors.back());}));
}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::actualNativeCodecRoundtrip_data() try{QTest::addColumn<bool>("axial");QTest::newRow("actual-TRUSS-codec")<<true;QTest::newRow("actual-BEAM-codec")<<false;}REPORT_EXCEPTIONS
void TestPiezoAcceptedTrace::actualNativeCodecRoundtrip() try{
    QFETCH(bool,axial);const auto source=actualSource(axial);QByteArray payload;Trace::Header h=header(3,0,0.);h.dt=.001;h.targetTimeGridSha256=Trace::targetTimeGridSha256(h.initialTime,h.dt,h.targetSteps);
    if(axial){RStructuralPiezoAxialNativeRun run(source);const auto &r=run.acceptedRuntime();payload=RStructuralPiezoAxialRestartCodec::encode(r);QCOMPARE(RStructuralPiezoAxialRestartCodec::encode(RStructuralPiezoAxialRestartCodec::decode(payload)),payload);h.family="axial";h.sourceFingerprint=r.sourceFingerprint;h.configurationIdentity=QString::fromLatin1(r.configurationIdentity.toHex());h.backendIdentity=r.backendIdentity;h.initialTime=r.initialTime;h.dt=r.dt;h.targetSteps=r.targetSteps;}
    else{RStructuralPiezoLayeredNativeRun run(source);const auto &r=run.acceptedRuntime();payload=RStructuralPiezoLayeredRestartCodec::encode(r);QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(RStructuralPiezoLayeredRestartCodec::decode(payload)),payload);h.sourceFingerprint=r.sourceFingerprint;h.configurationIdentity=QString::fromLatin1(r.configurationIdentity.toHex());h.backendIdentity=r.backendIdentity;h.initialTime=r.initialTime;h.dt=r.dt;h.targetSteps=r.targetSteps;}
    h.targetTimeGridSha256=Trace::targetTimeGridSha256(h.initialTime,h.dt,h.targetSteps);QByteArray bytes;QBuffer file(&bytes);QVERIFY(file.open(QIODevice::ReadWrite));h=Trace::writeHeader(file,h);const auto c=Trace::append(file,h,Trace::initialCursor(h),0,0.,payload);const auto verified=Trace::verifyPrefix(file,c);QCOMPARE(verified.terminal.payload,payload);QCOMPARE(verified.cursor.stateSha32,hash(payload));
}REPORT_EXCEPTIONS
QTEST_GUILESS_MAIN(TestPiezoAcceptedTrace)
#include "tst_piezo_accepted_trace.moc"
