#ifndef NATIVE_RESULT_JOURNAL_H
#define NATIVE_RESULT_JOURNAL_H

#include <QFile>
#include <QMutex>
#include <QString>

#include <rsolvergeneric.h>

#include <memory>

//! Bounded private JSONL index of actual native model frames.
class NativeResultJournal final
{
public:
    static constexpr const char *protocolVersion = "range.native-result-frame/v1";
    static constexpr const char *jobIdEnvironmentVariable = "RANGE_NATIVE_JOB_ID";
    static constexpr const char *pathEnvironmentVariable = "RANGE_NATIVE_RESULT_JOURNAL_PATH";
    static constexpr const char *resultDirectoryEnvironmentVariable = "RANGE_NATIVE_RESULT_DIRECTORY";

    NativeResultJournal(const NativeResultJournal &) = delete;
    NativeResultJournal &operator=(const NativeResultJournal &) = delete;
    ~NativeResultJournal() = default;

    //! Returns null without an error when none of the trusted variables is set.
    //! A partial environment handshake or invalid path returns an error.
    static std::unique_ptr<NativeResultJournal> createFromEnvironment(QString *error = nullptr);

    //! Opens a new private journal after validating its parent and result directory.
    static std::unique_ptr<NativeResultJournal> create(const QString &jobId,
                                                       const QString &fileName,
                                                       const QString &resultDirectory,
                                                       QString *error = nullptr);

    bool isAvailable() const;
    QString unavailableReason() const;

    //! Hash and append one fully persisted result frame. Invalid records disable
    //! the sidechannel without throwing into the numerical solver.
    bool appendFrame(const RSolverResultFrame &frame);

private:
    NativeResultJournal(const QString &jobId, const QString &fileName,
                        const QString &resultDirectory);

    bool appendLocked(const RSolverResultFrame &frame);
    void disableLocked(const QString &reason);

    const QString jobId;
    const QString resultDirectory;
    mutable QMutex mutex;
    QFile file;
    quint64 sequence = 1;
    qint64 bytesWritten = 0;
    QString failureReason;
    bool available = false;
};

#endif // NATIVE_RESULT_JOURNAL_H
