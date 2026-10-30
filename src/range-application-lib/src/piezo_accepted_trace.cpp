#include "piezo_accepted_trace.h"
#include <rbl_hash_sha256.h>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using Trace=PiezoAcceptedTrace;
const QByteArray magic("RPACTR01",8);
constexpr quint64 maxTarget=1000000;
constexpr qint64 recordOverhead=96;
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
QByteArray hash(QByteArrayView b){return RHashSha256::hash(b);}
void integer(QByteArray &b,quint64 value){char bytes[8];for(uint i=0;i<8;++i)bytes[i]=char((value>>(8*i))&255);b.append(bytes,8);}
void real(QByteArray &b,double value){static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559,"IEEE binary64 required");require(std::isfinite(value),"Trace value is nonfinite");quint64 bits=0;std::memcpy(&bits,&value,8);integer(b,bits);}
bool same(double a,double b){return std::memcmp(&a,&b,8)==0;}
bool same(const Trace::Cursor &a,const Trace::Cursor &b){return a.byteEnd==b.byteEnd && a.sequence==b.sequence && a.lastRecordOffset==b.lastRecordOffset && a.recordSha32==b.recordSha32 && a.stateSha32==b.stateSha32;}
void text(QByteArray &b,const QString &value){require(value.size()<=Trace::MaxHeaderBytes,"Trace string exceeds header bound");const auto utf8=value.toUtf8();require(QString::fromUtf8(utf8)==value,"Trace string is not canonical UTF-8");integer(b,quint64(utf8.size()));b+=utf8;require(b.size()<=Trace::MaxHeaderBytes,"Trace header exceeds 64 KiB");}
struct Decoder
{
    const QByteArray &bytes;qsizetype position=0;
    quint64 integer(){require(position<=bytes.size()-8,"Truncated trace integer");quint64 value=0;for(uint i=0;i<8;++i)value|=quint64(uchar(bytes[position++]))<<(8*i);return value;}
    QByteArray blob(quint64 length){require(length<=quint64(bytes.size()-position),"Truncated trace field");const auto b=bytes.mid(position,qsizetype(length));position+=qsizetype(length);return b;}
    QString text(){const auto length=integer();require(length<=quint64(Trace::MaxHeaderBytes),"Trace string exceeds header bound");const auto b=blob(length);const auto value=QString::fromUtf8(b);require(value.toUtf8()==b,"Trace string is not canonical UTF-8");return value;}
    double real(){const auto bits=integer();double value=0;std::memcpy(&value,&bits,8);require(std::isfinite(value),"Trace value is nonfinite");return value;}
};
void device(QIODevice &d){require(d.isOpen() && d.isReadable() && !d.isSequential(),"Trace needs an open readable seekable device");const auto size=d.size();require(size>=0 && size<=Trace::MaxFileBytes,"Trace file exceeds 4 GiB");}
QByteArray read(QIODevice &d,qint64 count){require(count>=0 && count<=Trace::MaxPayloadBytes+recordOverhead,"Trace read exceeds bounded frame");QByteArray b;while(b.size()<count){const auto chunk=d.read(count-b.size());require(!chunk.isEmpty(),"Truncated trace bytes");b+=chunk;}return b;}
void write(QIODevice &d,const QByteArray &b){require(d.isWritable(),"Trace device is not writable");qint64 offset=0;while(offset<b.size()){const auto n=d.write(b.constData()+offset,b.size()-offset);require(n>0 && n<=b.size()-offset,"Trace write failed");offset+=n;}if(auto *f=qobject_cast<QFile*>(&d))require(f->flush(),"Trace file flush failed");}
void metadata(const Trace::Header &h){
    require(h.schema==1,"Unsupported trace schema");require(h.family=="axial" || h.family=="layered","Unsupported trace family");
    const auto sha=[](const QString &v){if(v.size()!=64)return false;for(auto c:v)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;return true;};
    require(sha(h.sourceFingerprint) && sha(h.configurationIdentity),"Trace source/configuration identity is invalid");require(!h.backendIdentity.isEmpty(),"Trace backend identity is missing");
    require(h.baseModelSha256.size()==32 && h.contextSha256.size()==32 && h.targetTimeGridSha256.size()==32,"Trace SHA-256 metadata must contain 32 raw bytes");
    require(!h.baseModelFile.isEmpty() && !QDir::isAbsolutePath(h.baseModelFile) && !h.baseModelFile.contains('\\') && !h.baseModelFile.contains(':') && !h.baseModelFile.contains(QChar(0)),"Trace base MODEL path must be relative and canonical");
    for(const auto &part:h.baseModelFile.split('/'))require(!part.isEmpty() && part!="." && part!="..","Trace base MODEL path escapes its context");
    require(std::isfinite(h.initialTime) && std::isfinite(h.dt) && h.dt>0 && h.targetSteps>0 && h.targetSteps<=maxTarget && h.baseSeq<=h.targetSteps,"Trace canonical clock is invalid");
}
QByteArray headerBytes(const Trace::Header &h){
    metadata(h);QByteArray body;integer(body,h.schema);text(body,h.family);text(body,h.sourceFingerprint);text(body,h.configurationIdentity);text(body,h.backendIdentity);text(body,h.baseModelFile);
    body+=h.baseModelSha256;body+=h.contextSha256;integer(body,h.baseSeq);real(body,h.initialTime);real(body,h.dt);integer(body,h.targetSteps);body+=h.targetTimeGridSha256;
    require(body.size()+48<=Trace::MaxHeaderBytes,"Trace header exceeds 64 KiB");QByteArray out=magic;integer(out,quint64(body.size()));out+=body;out+=hash(out);return out;
}
double time(const Trace::Header &h,quint64 sequence){const double t=sequence?h.initialTime+double(sequence)*h.dt:h.initialTime;require(std::isfinite(t),"Trace canonical time overflow");return t;}
void cursorShape(const Trace::Header &h,const Trace::Cursor &c){
    if(c.lastRecordOffset==-1)require(c.byteEnd==h.byteEnd && c.sequence==h.baseSeq && c.recordSha32==h.headerSha32 && c.stateSha32.isEmpty(),"Trace header-only cursor changed");
    else require(c.lastRecordOffset>=h.byteEnd && c.lastRecordOffset<c.byteEnd && c.byteEnd<=Trace::MaxFileBytes && c.sequence>=h.baseSeq && c.sequence<=h.targetSteps && c.sequence-h.baseSeq<Trace::MaxRecords && c.recordSha32.size()==32 && c.stateSha32.size()==32,"Trace cursor is invalid");
}
void cursor(const Trace::Header &h,const Trace::Cursor &c){
    require(h.byteEnd>=48 && h.byteEnd<=Trace::MaxHeaderBytes && h.headerSha32.size()==32,"Trace header proof is missing");
    const auto immutable=headerBytes(h);require(h.byteEnd==immutable.size() && h.headerSha32==immutable.right(32),"Trace header proof changed");
    cursorShape(h,c);
}
QByteArray recordBytes(quint64 sequence,double t,const QByteArray &payload,const QByteArray &previous){
    require(!payload.isEmpty() && payload.size()<=Trace::MaxPayloadBytes,"Trace payload must contain 1..16 MiB bounded bytes");require(previous.size()==32,"Trace previous SHA must contain 32 raw bytes");
    QByteArray out;out.reserve(payload.size()+recordOverhead);integer(out,quint64(payload.size()+recordOverhead));
    integer(out,sequence);real(out,t);integer(out,quint64(payload.size()));out+=payload;out+=previous;
    out+=hash(QByteArrayView(out).sliced(8));return out;
}
Trace::Record frame(QIODevice &d,const Trace::Header &h,qint64 offset,qint64 end,RHashSha256 *witness=nullptr){
    require(offset>=h.byteEnd && offset<=end-8 && end<=d.size() && end<=Trace::MaxFileBytes,"Truncated trace record prefix");require(d.seek(offset),"Trace record seek failed");
    const auto lengthBytes=read(d,8);Decoder length{lengthBytes};const auto size=length.integer();
    require(size>quint64(recordOverhead) && size<=quint64(Trace::MaxPayloadBytes+recordOverhead) && size<=quint64(end-offset),"Trace record length is invalid or truncated");
    const auto body=read(d,qint64(size)-8);Decoder in{body};Trace::Record r;r.sequence=in.integer();r.time=in.real();const auto n=in.integer();
    require(n>0 && n<=quint64(Trace::MaxPayloadBytes) && size==n+quint64(recordOverhead),"Trace payload/frame length mismatch");r.payload=in.blob(n);r.previousSha32=in.blob(32);const auto current=in.blob(32);require(in.position==body.size(),"Trace record has unknown trailing fields");
    require(current==hash(QByteArrayView(body).first(body.size()-32)),"Trace record SHA-256 mismatch");
    require(r.sequence>=h.baseSeq && r.sequence<=h.targetSteps && r.sequence-h.baseSeq<Trace::MaxRecords && same(r.time,time(h,r.sequence)),"Trace record sequence/canonical time changed");
    r.cursor={offset+qint64(size),offset,r.sequence,current,hash(r.payload)};
    // These exact bytes have the same canonical fields/length/hash as a
    // reconstructed frame. Partial witness updates never escape a rejection.
    if(witness){witness->addData(lengthBytes);witness->addData(body);}return r;
}
Trace::Record checkedRecord(QIODevice &d,const Trace::Header &h,const Trace::Cursor &expected,qint64 prefixEnd,
                           bool ownedHeaderAdmitted,RHashSha256 *witness=nullptr){
    device(d);
    // Only verifyPrefix's privately owned, immutable header uses this path,
    // after readHeader/cursor admitted its complete canonical proof. Public
    // readRecord keeps full header proof validation on every invocation.
    if(ownedHeaderAdmitted)cursorShape(h,expected);else cursor(h,expected);
    const auto r=frame(d,h,expected.byteEnd,prefixEnd<0?d.size():prefixEnd,witness);
    require(r.previousSha32==expected.recordSha32,"Trace previous record SHA changed");const auto next=expected.lastRecordOffset==-1?h.baseSeq:expected.sequence+1;require(r.sequence==next,"Trace records are duplicated, reordered or discontinuous");return r;
}
QByteArray prefixHash(QIODevice &d,qint64 end){require(d.seek(0),"Trace prefix seek failed");RHashSha256 h;qint64 left=end;while(left){const auto b=read(d,std::min(left,qint64(64*1024)));h.addData(b);left-=b.size();}return h.result();}
}
QByteArray PiezoAcceptedTrace::targetTimeGridSha256(double initial,double dt,quint64 target){
    require(std::isfinite(initial) && std::isfinite(dt) && dt>0 && target>0 && target<=maxTarget,"Trace target grid is invalid");
    RHashSha256 h;double previous=initial;
    for(quint64 seq=1;seq<=target;++seq){const double t=initial+double(seq)*dt;require(std::isfinite(t) && t>previous,"Trace target grid does not advance binary64 time");QByteArray b;real(b,t);h.addData(b);previous=t;}return h.result();
}
PiezoAcceptedTrace::Header PiezoAcceptedTrace::writeHeader(QIODevice &d,const Header &h){
    device(d);require(d.isWritable() && d.size()==0,"Trace header requires an empty writable device");
    require(h.targetTimeGridSha256==targetTimeGridSha256(h.initialTime,h.dt,h.targetSteps),"Trace target grid SHA differs from canonical exact bits");const auto expected=headerBytes(h);require(d.seek(0),"Trace header seek failed");write(d,expected);
    require(d.size()==expected.size(),"Trace header file length changed");require(d.seek(0) && read(d,expected.size())==expected,"Trace header readback failed");return readHeader(d);
}
PiezoAcceptedTrace::Header PiezoAcceptedTrace::readHeader(QIODevice &d){
    device(d);require(d.seek(0),"Trace header seek failed");const auto fixed=read(d,16);require(fixed.left(8)==magic,"Trace header magic changed");Decoder size{fixed};size.position=8;const auto n=size.integer();
    require(n>0 && n<=quint64(MaxHeaderBytes-48) && n+48<=quint64(d.size()),"Trace header length is invalid or truncated");const auto body=read(d,qint64(n));const auto digest=read(d,32);require(digest==hash(fixed+body),"Trace header SHA-256 mismatch");
    Decoder in{body};Header h;h.schema=in.integer();h.family=in.text();h.sourceFingerprint=in.text();h.configurationIdentity=in.text();h.backendIdentity=in.text();h.baseModelFile=in.text();h.baseModelSha256=in.blob(32);h.contextSha256=in.blob(32);h.baseSeq=in.integer();h.initialTime=in.real();h.dt=in.real();h.targetSteps=in.integer();h.targetTimeGridSha256=in.blob(32);
    require(in.position==body.size(),"Trace header has unknown trailing fields");metadata(h);h.headerSha32=digest;h.byteEnd=qint64(n)+48;require(headerBytes(h)==fixed+body+digest,"Trace header encoding is not canonical");return h;
}
PiezoAcceptedTrace::Cursor PiezoAcceptedTrace::initialCursor(const Header &h){metadata(h);require(h.headerSha32.size()==32 && h.byteEnd==headerBytes(h).size() && h.headerSha32==headerBytes(h).right(32),"Trace header proof changed");return {h.byteEnd,-1,h.baseSeq,h.headerSha32,{}};}
PiezoAcceptedTrace::Record PiezoAcceptedTrace::readRecord(QIODevice &d,const Header &h,const Cursor &expected,qint64 prefixEnd){
    return checkedRecord(d,h,expected,prefixEnd,false);
}
PiezoAcceptedTrace::Cursor PiezoAcceptedTrace::append(QIODevice &d,const Header &h,const Cursor &old,quint64 sequence,double t,const QByteArray &payload){
    device(d);require(d.isWritable(),"Trace append device is not writable");const auto actual=readHeader(d);require(headerBytes(h)==headerBytes(actual) && h.headerSha32==actual.headerSha32 && h.byteEnd==actual.byteEnd,"Trace immutable header differs from append authority");cursor(h,old);require(d.size()==old.byteEnd,"Trace append has an uncommitted tail or changed file length");
    if(old.lastRecordOffset!=-1){const auto terminal=frame(d,h,old.lastRecordOffset,old.byteEnd);require(same(terminal.cursor,old),"Trace old terminal differs from its cursor");require((old.sequence==h.baseSeq)==(old.lastRecordOffset==h.byteEnd),"Trace first record/cursor offset changed");if(old.sequence==h.baseSeq)require(terminal.previousSha32==h.headerSha32,"Trace first record header chain changed");}
    const auto next=old.lastRecordOffset==-1?h.baseSeq:old.sequence+1;require(sequence==next && sequence<=h.targetSteps && sequence-h.baseSeq<MaxRecords && same(t,time(h,sequence)),"Trace append sequence/canonical time changed");const auto encoded=recordBytes(sequence,t,payload,old.recordSha32);require(encoded.size()<=MaxFileBytes-old.byteEnd,"Trace append exceeds 4 GiB");
    require(d.seek(old.byteEnd),"Trace append seek failed");write(d,encoded);const auto end=old.byteEnd+encoded.size();require(d.size()==end,"Trace appended file length changed");const auto r=readRecord(d,h,old,end);require(r.payload==payload && same(r.time,t) && r.cursor.byteEnd==end,"Trace appended record readback failed");
    require(readHeader(d).headerSha32==h.headerSha32,"Trace header changed during append");require(d.seek(end),"Trace appended cursor seek failed");return r.cursor;
}
PiezoAcceptedTrace::Prefix PiezoAcceptedTrace::verifyPrefix(QIODevice &d,const Cursor &authority,bool allowTail){
    device(d);const auto size=d.size();Prefix result;result.header=readHeader(d);const auto &h=result.header;cursor(h,authority);require(authority.byteEnd<=size && (allowTail || authority.byteEnd==size),"Trace cursor/file length mismatch or uncommitted tail");
    require(h.targetTimeGridSha256==targetTimeGridSha256(h.initialTime,h.dt,h.targetSteps),"Trace target grid SHA differs from canonical exact bits");RHashSha256 witness;witness.addData(headerBytes(h));auto current=initialCursor(h);
    while(current.byteEnd<authority.byteEnd){const auto r=checkedRecord(d,h,current,authority.byteEnd,true,&witness);require(++result.records<=MaxRecords,"Trace record count exceeds 20001");current=r.cursor;result.terminal=r;}
    require(same(current,authority),"Trace terminal record differs from committed cursor");result.prefixSha32=witness.result();require(prefixHash(d,authority.byteEnd)==result.prefixSha32 && d.size()==size,"Trace prefix changed during verification");result.cursor=current;result.uncommittedTailBytes=size-authority.byteEnd;require(d.seek(authority.byteEnd),"Trace verified cursor seek failed");return result;
}
