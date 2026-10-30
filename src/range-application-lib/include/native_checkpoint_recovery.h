#ifndef NATIVE_CHECKPOINT_RECOVERY_H
#define NATIVE_CHECKPOINT_RECOVERY_H

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

#include <piezo_accepted_trace.h>

class QLockFile;

//! Shared inspection and rollback for interrupted native checkpoint publishes.
//! Callers must retain a WriterLease from inspection through certification and
//! finish so a solver writer cannot race the recovery decision.
namespace NativeCheckpointRecovery
{
class WriterLease final
{
public:
    explicit WriterLease(const QString &directory);
    ~WriterLease();

    WriterLease(const WriterLease &) = delete;
    WriterLease &operator=(const WriterLease &) = delete;
    WriterLease(WriterLease &&) noexcept;
    WriterLease &operator=(WriterLease &&) noexcept;

    const QString &directory() const noexcept;
    bool ownsLease() const noexcept;

private:
    QString canonicalDirectory;
    std::unique_ptr<QLockFile> lock;

    friend class Plan;
};

//! Immutable recovery description bound to marker and rollback evidence.
//! inspect() is read-only; restore() keeps PENDING in place; finish() removes
//! it only after callers have strictly certified the restored authority.
class Plan final
{
public:
    static std::optional<Plan> inspect(const QString &directory,
                                       const QByteArray &canonicalContextJson,
                                       QString *error = nullptr);

    bool restore(const WriterLease &lease, QString *error = nullptr) const;
    bool finish(const WriterLease &lease, QString *error = nullptr) const;

    const QString &directory() const noexcept;
    const QString &token() const noexcept;
    const QByteArray &markerBytes() const noexcept;
    bool latestExists() const noexcept;
    const QByteArray &latestBytes() const noexcept;
    bool lastValidExists() const noexcept;
    const QByteArray &lastValidBytes() const noexcept;
    bool traceMode() const noexcept;
    const PiezoAcceptedTrace::Cursor &oldTraceCursor() const noexcept;
    const QString &traceHeaderSha256() const noexcept;
    quint64 candidateSequence() const noexcept;
    const QString &contextSha256() const noexcept;

private:
    friend struct PlanAccess;
    QString checkpointDirectory;
    QByteArray contextJson;
    QString stableToken;
    QByteArray pendingMarkerBytes;
    bool hadLatest = false;
    QByteArray oldLatestBytes;
    bool hadLastValid = false;
    QByteArray oldLastValidBytes;
    bool trace = false;
    PiezoAcceptedTrace::Cursor traceCursor;
    QString traceHeaderHash;
    quint64 candidateSeq = 0;
    QString contextHash;
};
}

#endif // NATIVE_CHECKPOINT_RECOVERY_H
