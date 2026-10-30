#ifndef PIEZO_CERTIFICATION_BATCH_CACHE_H
#define PIEZO_CERTIFICATION_BATCH_CACHE_H
#include <rstructural_piezo_axial_native_run.h>
#include <rstructural_piezo_layered_native_run.h>
#include <QJsonObject>
#include <functional>
#include <memory>

//! Owner-thread private backend proofs. Native frozen-run physics certification
//! remains mandatory; this cache only replaces admitted backend RPC results.
class PiezoCertificationBatchCache final
{
public:
    using Request=std::function<QJsonObject(const QJsonObject &)>;
    struct Pipeline {
        std::function<void(const QJsonObject &)> begin;
        std::function<QJsonObject()> finish;
        std::function<void()> abort;
    };
    PiezoCertificationBatchCache(Request request,const QJsonObject &initializedIdentity,
        const QString &family,double initialTime,double dt,uint targetSteps,
        uint controlEveryFESteps,std::size_t physicalForceSize,Pipeline pipeline={});
    ~PiezoCertificationBatchCache();
    PiezoCertificationBatchCache(const PiezoCertificationBatchCache &)=delete;
    PiezoCertificationBatchCache &operator=(const PiezoCertificationBatchCache &)=delete;
    RStructuralPiezoAxialRuntimeCallbacks wrapAxial(const RStructuralPiezoAxialRuntimeCallbacks &) const;
    RStructuralPiezoLayeredRuntimeCallbacks wrapLayered(const RStructuralPiezoLayeredRuntimeCallbacks &) const;
    void prepare(const std::vector<QByteArray> &completeRestartPayloads);
    //! Next bank is private until finish validates the entire response.
    //! Current bank remains available while exactly one next RPC is pending.
    void beginPrepare(const std::vector<QByteArray> &completeRestartPayloads);
    void finishPrepare();
    void abortPending();
    void recheck();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
#endif
