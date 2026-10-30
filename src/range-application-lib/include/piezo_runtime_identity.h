#ifndef PIEZO_RUNTIME_IDENTITY_H
#define PIEZO_RUNTIME_IDENTITY_H
#include <QByteArray>
#include <QString>
#include <QtGlobal>
class RModel;

//! Compatibility evidence. The factory certifies actual source and complete
//! accepted state; the hashes alone are not authentication or restore authority.
struct RPiezoRuntimeCertificate
{
    QString family, sourceFingerprint;
    QByteArray stateIdentity;
    bool available = false, canonicalTarget = false;
    quint64 completedSteps = 0, targetSteps = 0;
    double time = 0, fixedTimeStep = 0, initialTime = 0;
};
//! Same backend selection and callback identity as the native CLI.
QByteArray rPiezoRuntimeControlSha256(const RModel &model);
//! acceptedModel=true explicitly requests a complete checkpoint restore.
//! Otherwise the source's restart flag selects the baseline, exactly as CLI.
RPiezoRuntimeCertificate rPiezoRuntimeCertify(const RModel &model, bool acceptedModel = false);
bool rPiezoRuntimeSameAcceptedState(const RPiezoRuntimeCertificate &a,
                                   const RPiezoRuntimeCertificate &b);
#endif
