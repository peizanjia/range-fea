#ifndef PIEZO_ACCEPTED_TRACE_H
#define PIEZO_ACCEPTED_TRACE_H
#include <QByteArray>
#include <QString>
#include <QtGlobal>
class QIODevice;

//! Bounded immutable-prefix storage of opaque complete native restart bytes.
//! Integrity/compatibility only; no payload schema or physical certification.
//! Caller owns exclusive access, PENDING publication and rollback/truncation.
class PiezoAcceptedTrace
{
public:
    static constexpr qint64 MaxHeaderBytes=64*1024;
    static constexpr qint64 MaxPayloadBytes=16*1024*1024;
    static constexpr qint64 MaxFileBytes=qint64(4)*1024*1024*1024;
    static constexpr quint64 MaxRecords=20001;
    struct Header
    {
        quint64 schema=1;
        QString family,sourceFingerprint,configurationIdentity,backendIdentity,baseModelFile;
        //! Raw 32-byte SHA-256 values, not hexadecimal text.
        QByteArray baseModelSha256,contextSha256,targetTimeGridSha256;
        quint64 baseSeq=0,targetSteps=0;
        double initialTime=0,dt=0;
        //! Derived read/write proof, never supplied as mutable metadata.
        QByteArray headerSha32;
        qint64 byteEnd=0;
    };
    struct Cursor
    {
        qint64 byteEnd=0,lastRecordOffset=-1;
        quint64 sequence=0;
        QByteArray recordSha32,stateSha32;
    };
    struct Record
    {
        quint64 sequence=0;
        double time=0;
        QByteArray payload,previousSha32;
        Cursor cursor;
    };
    struct Prefix
    {
        Header header;
        Cursor cursor;
        Record terminal;
        quint64 records=0;
        qint64 uncommittedTailBytes=0;
        QByteArray prefixSha32;
    };
    //! Canonical SHA-256 of little-endian binary64 target times, steps 1..target.
    static QByteArray targetTimeGridSha256(double initialTime,double dt,quint64 targetSteps);
    //! Empty, readable/writable seekable device; flush and exact header readback.
    static Header writeHeader(QIODevice &device,const Header &header);
    static Header readHeader(QIODevice &device);
    //! Header-only cursor; first append MUST contain baseSeq, including restart bases.
    static Cursor initialCursor(const Header &header);
    //! Verify header, old terminal/cursor and exact file length, then append/readback
    //! one record. Earlier prefix bytes require verifyPrefix at full admission.
    static Cursor append(QIODevice &device,const Header &header,const Cursor &oldCursor,
                         quint64 sequence,double time,const QByteArray &restartCodecBytes);
    //! Read exactly the successor to expected. prefixEnd bounds committed reads.
    static Record readRecord(QIODevice &device,const Header &header,const Cursor &expected,
                             qint64 prefixEnd=-1);
    //! Streaming full-chain verification, retains ONLY terminal payload. Tail bytes
    //! are rejected unless explicitly diagnostic; never decoded or admitted.
    static Prefix verifyPrefix(QIODevice &device,const Cursor &cursor,bool allowUncommittedTail=false);
};
#endif
