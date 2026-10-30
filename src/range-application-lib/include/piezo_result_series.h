#ifndef PIEZO_RESULT_SERIES_H
#define PIEZO_RESULT_SERIES_H
#include <QString>
#include <QStringList>
#include <QVector>
#include "piezo_accepted_trace.h"
#include <functional>
#include <memory>
#include <vector>
class RModel;

//! Certified native accepted records, never display-variable observations.
//! Identities describe compatibility, not authentication. Published objects are
//! immutable to consumers. No interpolation or complete-history inference.
class PiezoResultSeries
{
public:
    struct Signal
    {
        QString channelId,quantity,axis,unit,nodeTarget,portId;
        uint physicalNodeId=0;
        bool actionRecord=false;
        double value=0;
    };
    struct Readout
    {
        QString portId,unit;
        int mode=0;
        bool actionRecord=false;
        double value=0,deltaValue=0,voltage=0,deltaVoltage=0,charge=0,deltaCharge=0;
        bool amplifierAvailable=false;
        double amplifierVoltage=0,deltaAmplifierVoltage=0;
    };
    struct Action
    {
        QString portId;
        bool intervalPresent=false,amplitudeClipped=false,rateClipped=false;
        double heldRequested=0,requested=0,amplitudeLimited=0,previousApplied=0,applied=0;
    };
    struct Frame
    {
        QString family,modelFamily,sourceFingerprint,configurationIdentity,backendIdentity,backendControlSha256,stateSha256,controlKind;
        uint completedFE=0,targetSteps=0,controlTick=0,controlPhase=0;
        double timeS=0,dtS=0,initialTimeS=0,electricalWorkJ=0;
        QString electricalWorkSource;
        QVector<Readout> readouts;
        QVector<Action> actions;
        QVector<Signal> channels;
    };
    struct RecordProof {QString fileName,sha256;uint completedFE=0;};
    struct Dataset
    {
        QString family,modelFamily,sourceFingerprint,configurationIdentity,backendIdentity,artifactSha256;
        QString coverage=QStringLiteral("exact selected certified accepted records; no interpolation or claim of complete FE history");
        bool continuousBetweenSelectedRecords=true;
        bool sampledCoverage=true;
        QVector<RecordProof> records;
        QVector<std::shared_ptr<const Frame>> frames;
    };
    struct TraceProof
    {
        QString baseModelPath,tracePath,expectedSourceFingerprint;
        //! Raw 32-byte SHA-256 values, matching PiezoAcceptedTrace::Header.
        QByteArray baseModelSha256,expectedHeaderSha256,expectedContextSha256;
        PiezoAcceptedTrace::Cursor cursor;
    };
    //! Column-major numeric history; metadata is retained once, never one Frame
    //! or copy of descriptive strings per accepted sample.
    struct NumericDataset
    {
        QString family,modelFamily,sourceFingerprint,configurationIdentity,backendIdentity,artifactSha256;
        QString coverage=QStringLiteral("exact committed certified accepted trace prefix; no interpolation or complete-target-history inference");
        quint64 baseSeq=0,targetSteps=0;
        double initialTimeS=0,dtS=0;
        bool continuousBetweenSelectedRecords=true,sampledCoverage=true;
        QVector<uint> completedFE;
        QVector<double> times;
        QVector<Signal> channels;
        QVector<QVector<double>> columns;
        QVector<Readout> readouts;
    };
    //! One backend factory per reader, callbacks reused on their owner thread.
    //! Frozen driver and backend are reused; each record retains complete source,
    //! runtime and original-clock certification. The 20k performance gate is separate.
    class Reader
    {
    public:
        //! Formal compact trace production requires the native factory even
        //! for kind=none. Raw legacy records retain declared-source selection.
        //! The workflow supplies this policy; accepted bytes/header cannot.
        explicit Reader(const RModel &source,bool factoryRequired=false);
        ~Reader();
        Reader(const Reader &)=delete;
        Reader &operator=(const Reader &)=delete;
        std::shared_ptr<const Frame> certifyAcceptedModel(const RModel &model) const;
        //! Source/clock authority comes from this frozen Reader, not the bytes.
        //! No MODEL decode/reconstruction or target-grid scan on this path.
        std::shared_ptr<const Frame> certifyAcceptedRuntime(const QByteArray &codecBytes) const;
        //! Optional bounded provider preparation; each raw native certification
        //! remains authoritative. A missing preparation is never an exemption.
        void prepareAcceptedRuntimeBatch(const std::vector<QByteArray> &codecBytes) const;
        void beginAcceptedRuntimeBatch(const std::vector<QByteArray> &codecBytes) const;
        void finishAcceptedRuntimeBatch() const;
        void recheckSources() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
    struct Result
    {
        bool valid=false;
        QString code,message;
        std::shared_ptr<const Dataset> dataset;
    };
    struct NumericResult
    {
        bool valid=false;
        QString code,message;
        std::shared_ptr<const NumericDataset> dataset;
    };
    using FrameConsumer=std::function<void(const Frame &,const QVector<Signal> &selectedMetadata)>;
    using SummaryConsumer=std::function<void(const NumericDataset &)>;
    static NumericResult readAcceptedTrace(const TraceProof &,const QStringList &selectedChannelIds={});
    //! Consumer sees transient certified Frames; publish its staged output only
    //! if valid is true. Completion also only stages output, before the final
    //! file/provider rechecks. Summary retains clocks and metadata, no columns.
    //! Recovery preview may explicitly certify a committed prefix with a
    //! trailing unpublished suffix; callers must bind it to a validated pending
    //! checkpoint recovery plan. Strict exact-length behavior remains default.
    static NumericResult streamAcceptedTrace(const TraceProof &,const QStringList &selectedChannelIds,
                                            const FrameConsumer &consumer,const SummaryConsumer &stageCompletion={},
                                            bool allowUncommittedTail=false);
    //! Explicit bounded raw RBM/RAM records only. Optional hashes follow caller
    //! path order. Files and backend sources are revalidated before returning.
    static Result readAcceptedRecords(const QStringList &paths,const QString &expectedSourceFingerprint={},
                                      const QStringList &expectedFileSha256s={});
};
#endif
