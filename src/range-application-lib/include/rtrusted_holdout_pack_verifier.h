#ifndef RTRUSTED_HOLDOUT_PACK_VERIFIER_H
#define RTRUSTED_HOLDOUT_PACK_VERIFIER_H

#include <QString>

// Authenticates public pack bytes against separately supplied trust anchors.
// It cannot establish independent custody or when an external record was made.
class RTrustedHoldoutPackVerifier
{
public:
    struct Request
    {
        QString manifestPath, publicCasesPath, publicKeyPath;
        QString independentlyPinnedKeySha256;
        QString externallyRecordedPreRunCommitmentSha256;
        QString runId, armId, slotId;
    };
    struct VerifiedSlot
    {
        QString runId, armId, holdoutId, slotId, caseId;
        int repeat = 0;
        QString manifestCommitmentSha256, promptSha256;
    };
    struct Outcome
    {
        bool valid = false;
        QString code, message;
        VerifiedSlot slot;
    };
    static Outcome verifyPublicSlot(const Request &request);
};

#endif
