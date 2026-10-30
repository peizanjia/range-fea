#ifndef RPIEZO_ARTIFACT_SERVICE_H
#define RPIEZO_ARTIFACT_SERVICE_H
#include <QString>
#include <QStringList>
#include "piezo_result_series.h"
class RModel;

//! Source-bound artifacts, published only after complete validation.
class RPiezoArtifactService
{
public:
    struct Report
    {
        QString sourceFingerprint,qualification,modelFamily;
        uint recordCount=0;
        double firstTime=0,lastTime=0;
        bool continuousFE=false;
    };
    static bool writeMatrixPackage(const RModel &,const QString &path,
                                  QString *error=nullptr,Report *report=nullptr);
    //! Explicit RBM/RAM model records only; link files are not admitted.
    static bool writeAcceptedTimeSeries(const QStringList &nativeRecordPaths,const QString &csvPath,
                                       QString *error=nullptr,Report *report=nullptr,
                                       const QString &expectedSourceFingerprint={});
    //! Stream all registered certified channels from one authority-bound trace.
    //! No uncommitted tail is admitted; destination changes only after final checks.
    static bool writeAcceptedTraceTimeSeries(const PiezoResultSeries::TraceProof &,const QString &csvPath,
                                            QString *error=nullptr,Report *report=nullptr,
                                            const std::function<void()> &finalAuthorityCheck={});
};
#endif
