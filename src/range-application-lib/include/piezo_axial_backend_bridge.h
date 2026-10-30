#ifndef PIEZO_AXIAL_BACKEND_BRIDGE_H
#define PIEZO_AXIAL_BACKEND_BRIDGE_H
#include <rstructural_piezo_axial_native_run.h>
#include <QJsonObject>
#include "piezo_certification_batch_cache.h"
class PiezoAxialBackendBridge
{
public:
    struct Result {
        RStructuralPiezoAxialRuntimeCallbacks callbacks;
        QJsonObject exportPackage;
        std::function<QJsonObject(const QJsonObject &)> certificationBatchRequest;
        QJsonObject backendInitializationIdentity;
        PiezoCertificationBatchCache::Pipeline certificationBatchPipeline;
    };
    static Result create(const RModel &model);
    static Result create(const RModel &model,const QString &python,const QString &moduleRoot);
};
#endif
