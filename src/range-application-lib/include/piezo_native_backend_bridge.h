#ifndef PIEZO_NATIVE_BACKEND_BRIDGE_H
#define PIEZO_NATIVE_BACKEND_BRIDGE_H

#include <QJsonObject>
#include "piezo_certification_batch_cache.h"
#include <QString>
#include <rstructural_piezo_layered_native_run.h>
class RModel;

//! The persistent provider owns numerical designs, never accepted history.
//! Callbacks share its lifetime and must execute on the factory caller thread.
class PiezoNativeBackendBridge
{
public:
    struct Result
    {
        RStructuralPiezoLayeredRuntimeCallbacks callbacks;
        QJsonObject exportPackage;
        //! Pure certification service sharing this initialized owner-thread provider.
        std::function<QJsonObject(const QJsonObject &)> certificationBatchRequest;
        QJsonObject backendInitializationIdentity;
        PiezoCertificationBatchCache::Pipeline certificationBatchPipeline;
    };
    //! Installed private runtime first, then the compiled development runtime.
    static Result create(const RModel &source);
    static Result create(const RModel &source,const QString &pythonExecutable,
                         const QString &pythonModuleRoot);
};
#endif
