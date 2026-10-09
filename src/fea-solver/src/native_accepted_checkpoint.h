#ifndef NATIVE_ACCEPTED_CHECKPOINT_H
#define NATIVE_ACCEPTED_CHECKPOINT_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <memory>

class QLockFile;
class RModel;

//! Bounded, provenance-bound context supplied by the native workflow host.
struct NativeAcceptedCheckpointContext
{
    QString sourceSha256;
    QString modelSha256;
    QString controlSha256;
    QString runtimeSha256;
    QString packageSha256;
    QString policy;
    uint maxAcceptedSteps = 0;
    double maxSimulationSeconds = 0.0;
    double dtS = 0.0;

    static NativeAcceptedCheckpointContext parse(const QByteArray &canonicalJson);
    static QString noControlSha256();
    QJsonObject toJsonObject() const;
};

//! Writes immutable binary accepted-state MODELs and verified envelopes.
//! latest.json is an atomically replaced pointer; lastvalid.json keeps the
//! previous verified pointer for diagnosis, never for automatic restart.
class NativeAcceptedCheckpointStore final
{
public:
    struct VerifiedCheckpoint
    {
        QString modelPath;
        QJsonObject envelope;
        bool usedLastValid = false;
    };

    NativeAcceptedCheckpointStore(const QString &directory,
                                  const QByteArray &contextJson,
                                  const QString &inputModelSha256,
                                  const QString &controlSha256,
                                  const QString &runtimeSha256,
                                  bool restart,
                                  const RModel &initialModel);
    ~NativeAcceptedCheckpointStore();

    NativeAcceptedCheckpointStore(const NativeAcceptedCheckpointStore &) = delete;
    NativeAcceptedCheckpointStore &operator=(const NativeAcceptedCheckpointStore &) = delete;

    //! Called after every fully committed structural step. A due write failure
    //! throws, stopping the solver before it can accept another step.
    void accept(const RModel &acceptedModel);

    //! Resolve and verify latest.json. A damaged committed latest fails closed.
    VerifiedCheckpoint readLatest() const;

    const NativeAcceptedCheckpointContext &context() const;

private:
    QString directory;
    NativeAcceptedCheckpointContext checkpointContext;
    std::unique_ptr<QLockFile> writerLock;
    quint64 lastObservedSequence = 0;
    quint64 lastPublishedSequence = 0;
    double lastPublishedTimeS = 0.0;
    bool started = false;
    bool hasObservedStep = false;

    VerifiedCheckpoint readPointer(const QString &pointerName,
                                  bool usedLastValid) const;
    VerifiedCheckpoint readEnvelope(const QString &envelopeFile,
                                    const QString &envelopeHash,
                                    bool usedLastValid) const;
    void publish(const RModel &acceptedModel,
                 quint64 acceptedSequence,
                 double acceptedTimeS,
                 double dtS);
};

#endif // NATIVE_ACCEPTED_CHECKPOINT_H
