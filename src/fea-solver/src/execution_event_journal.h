#ifndef EXECUTION_EVENT_JOURNAL_H
#define EXECUTION_EVENT_JOURNAL_H

#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QLockFile>
#include <QString>
#include <QUuid>
#include <QVector>

//! One complete, newline-terminated UTF-8 JSON event in a run journal.
struct ExecutionEvent
{
    int schemaVersion = 1;
    QUuid eventId;
    QUuid runId;
    quint64 sequence = 0;
    QString utc;
    QString type;
    QString status;
    QJsonObject payload;
};

struct ExecutionEventReadResult
{
    QVector<ExecutionEvent> events;
    bool truncatedTail = false;
    QString error;
};

//! Single-writer append-only JSONL storage. It never truncates or repairs a file.
//!
//! A complete line is accepted only when it ends in '\n'. On a torn tail,
//! read() returns the preceding events and marks truncatedTail; a writer then
//! refuses to append rather than silently overwriting potentially useful data.
//! An adjacent lock file excludes another writer using the same path. This is
//! not a tamper-evident log and cannot detect in-place edits of equal length.
class ExecutionEventJournal
{
public:
    ExecutionEventJournal(const QString &fileName, const QUuid &runId);

    ExecutionEventJournal(const ExecutionEventJournal &) = delete;
    ExecutionEventJournal &operator=(const ExecutionEventJournal &) = delete;

    static QUuid createRunId();
    static ExecutionEventReadResult read(const QString &fileName);

    bool append(const QString &type, const QString &status,
                const QJsonObject &payload, QString *error = nullptr,
                ExecutionEvent *writtenEvent = nullptr);
    //! A stable ID makes retry after an uncertain write idempotent. Reusing it
    //! with different type/status/payload is rejected without appending.
    bool append(const QString &type, const QString &status,
                const QJsonObject &payload, const QUuid &eventId,
                QString *error = nullptr, ExecutionEvent *writtenEvent = nullptr);

    QUuid getRunId() const;

private:
    bool initialize(QString &error);

    // Declare the lock first so destruction closes the file before unlocking.
    QLockFile lock;
    QFile file;
    QUuid runId;
    QHash<QString,ExecutionEvent> eventsById;
    quint64 nextSequence = 1;
    qint64 knownSize = 0;
    bool initialized = false;
    bool failed = false;
};

#endif // EXECUTION_EVENT_JOURNAL_H
