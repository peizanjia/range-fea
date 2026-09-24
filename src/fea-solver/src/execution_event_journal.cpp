#include "execution_event_journal.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

#include <cmath>

namespace
{
constexpr int schemaVersion = 1;
// QJsonValue stores JSON numbers as IEEE doubles; keep integer sequence exact.
constexpr quint64 largestExactJsonInteger = (quint64(1) << 53) - 1;

bool parseEvent(const QByteArray &line, ExecutionEvent &event, QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line,&parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("Invalid JSON event: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("schemaVersion")).isDouble() ||
        object.value(QStringLiteral("schemaVersion")).toInt() != schemaVersion ||
        !object.value(QStringLiteral("eventId")).isString() ||
        !object.value(QStringLiteral("runId")).isString() ||
        !object.value(QStringLiteral("sequence")).isDouble() ||
        !object.value(QStringLiteral("utc")).isString() ||
        !object.value(QStringLiteral("type")).isString() ||
        !object.value(QStringLiteral("status")).isString() ||
        !object.value(QStringLiteral("payload")).isObject())
    {
        error = QStringLiteral("Event schema is incomplete or has invalid field types");
        return false;
    }
    const double numericSequence = object.value(QStringLiteral("sequence")).toDouble();
    if (!std::isfinite(numericSequence) || numericSequence < 1.0 ||
        numericSequence > double(largestExactJsonInteger) ||
        std::floor(numericSequence) != numericSequence)
    {
        error = QStringLiteral("Event sequence is not an exact positive JSON integer");
        return false;
    }
    event.schemaVersion = schemaVersion;
    event.eventId = QUuid(object.value(QStringLiteral("eventId")).toString());
    event.runId = QUuid(object.value(QStringLiteral("runId")).toString());
    event.sequence = quint64(numericSequence);
    event.utc = object.value(QStringLiteral("utc")).toString();
    event.type = object.value(QStringLiteral("type")).toString();
    event.status = object.value(QStringLiteral("status")).toString();
    event.payload = object.value(QStringLiteral("payload")).toObject();
    const QDateTime timestamp = QDateTime::fromString(event.utc,Qt::ISODateWithMs);
    if (event.eventId.isNull() || event.runId.isNull() ||
        !timestamp.isValid() || !event.utc.endsWith(QLatin1Char('Z')) ||
        event.type.isEmpty() || event.status.isEmpty())
    {
        error = QStringLiteral("Event contains an invalid UUID, UTC time, type or status");
        return false;
    }
    return true;
}

QJsonObject toJson(const ExecutionEvent &event)
{
    return {
        {QStringLiteral("schemaVersion"),event.schemaVersion},
        {QStringLiteral("eventId"),event.eventId.toString(QUuid::WithoutBraces)},
        {QStringLiteral("runId"),event.runId.toString(QUuid::WithoutBraces)},
        {QStringLiteral("sequence"),double(event.sequence)},
        {QStringLiteral("utc"),event.utc},
        {QStringLiteral("type"),event.type},
        {QStringLiteral("status"),event.status},
        {QStringLiteral("payload"),event.payload}
    };
}
}

ExecutionEventJournal::ExecutionEventJournal(const QString &fileName,
                                             const QUuid &runId)
    : lock(fileName + QStringLiteral(".lock"))
    , file(fileName)
    , runId(runId)
{
    // Runs can last much longer than QLockFile's default 30-second stale age.
    // Let process identity, not lock age, decide whether another writer lives.
    this->lock.setStaleLockTime(0);
}

QUuid ExecutionEventJournal::createRunId()
{
    return QUuid::createUuid();
}

QUuid ExecutionEventJournal::getRunId() const
{
    return this->runId;
}

ExecutionEventReadResult ExecutionEventJournal::read(const QString &fileName)
{
    ExecutionEventReadResult result;
    const QFileInfo info(fileName);
    if (info.isSymLink() || !info.isFile())
    {
        result.error = QStringLiteral("Journal is not a regular file: %1").arg(fileName);
        return result;
    }
    QFile input(fileName);
    if (!input.open(QIODevice::ReadOnly))
    {
        result.error = QStringLiteral("Cannot read journal '%1': %2")
                           .arg(fileName,input.errorString());
        return result;
    }

    QSet<QString> seenEventIds;
    QUuid firstRunId;
    quint64 expectedSequence = 1;
    quint64 lineNumber = 0;
    while (!input.atEnd())
    {
        const QByteArray line = input.readLine();
        ++lineNumber;
        if (line.isEmpty() && input.error() != QFileDevice::NoError)
        {
            result.error = QStringLiteral("Journal read failure at line %1: %2")
                               .arg(lineNumber).arg(input.errorString());
            return result;
        }
        if (!line.endsWith('\n'))
        {
            result.truncatedTail = true;
            break;
        }
        ExecutionEvent event;
        QString eventError;
        if (!parseEvent(line,event,eventError))
        {
            result.error = QStringLiteral("Journal line %1: %2")
                               .arg(lineNumber).arg(eventError);
            return result;
        }
        if (event.sequence != expectedSequence ||
            (!firstRunId.isNull() && event.runId != firstRunId) ||
            seenEventIds.contains(event.eventId.toString(QUuid::WithoutBraces)))
        {
            result.error = QStringLiteral("Journal line %1 has a duplicate, gap or mixed run ID")
                               .arg(lineNumber);
            return result;
        }
        firstRunId = event.runId;
        seenEventIds.insert(event.eventId.toString(QUuid::WithoutBraces));
        result.events.push_back(event);
        ++expectedSequence;
    }
    if (input.error() != QFileDevice::NoError)
    {
        result.error = QStringLiteral("Journal read failure: %1").arg(input.errorString());
    }
    return result;
}

bool ExecutionEventJournal::initialize(QString &error)
{
    if (this->initialized)
    {
        return true;
    }
    if (this->runId.isNull() || this->file.fileName().isEmpty())
    {
        error = QStringLiteral("A non-null run ID and journal path are required");
        return false;
    }
    // A failed open may be retried after permissions are repaired; derive the
    // sequence from the file again instead of retaining state from that try.
    this->nextSequence = 1;
    this->knownSize = 0;
    this->eventsById.clear();

    if (!this->lock.tryLock(0))
    {
        error = QStringLiteral("Journal is locked by another writer or its lock is unavailable");
        return false;
    }

    const QFileInfo info(this->file.fileName());
    if (info.isSymLink())
    {
        error = QStringLiteral("Refusing to append through a symbolic link");
        this->lock.unlock();
        return false;
    }
    if (info.exists())
    {
        if (!info.isFile())
        {
            error = QStringLiteral("Journal path is not a regular file");
            this->lock.unlock();
            return false;
        }
        const ExecutionEventReadResult existing = read(this->file.fileName());
        if (!existing.error.isEmpty() || existing.truncatedTail)
        {
            error = existing.truncatedTail
                ? QStringLiteral("Journal has a truncated tail; refusing to alter it")
                : existing.error;
            this->lock.unlock();
            return false;
        }
        if (!existing.events.isEmpty())
        {
            if (existing.events.front().runId != this->runId)
            {
                error = QStringLiteral("Existing journal belongs to another run ID");
                this->lock.unlock();
                return false;
            }
            this->nextSequence = existing.events.back().sequence+1;
            for (const ExecutionEvent &event : existing.events)
            {
                this->eventsById.insert(event.eventId.toString(QUuid::WithoutBraces),event);
            }
        }
        this->knownSize = info.size();
    }
    if (!this->file.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        error = QStringLiteral("Cannot open journal for append: %1")
                    .arg(this->file.errorString());
        this->lock.unlock();
        return false;
    }
    if (this->file.size() != this->knownSize)
    {
        error = QStringLiteral("Journal changed while opening for append");
        this->file.close();
        this->lock.unlock();
        return false;
    }
    this->initialized = true;
    return true;
}

bool ExecutionEventJournal::append(const QString &type, const QString &status,
                                   const QJsonObject &payload, QString *error,
                                   ExecutionEvent *writtenEvent)
{
    return this->append(type,status,payload,QUuid::createUuid(),error,writtenEvent);
}

bool ExecutionEventJournal::append(const QString &type, const QString &status,
                                   const QJsonObject &payload, const QUuid &eventId,
                                   QString *error, ExecutionEvent *writtenEvent)
{
    QString failure;
    if (this->failed)
    {
        failure = QStringLiteral("Journal has a previous write failure");
    }
    else if (type.isEmpty() || status.isEmpty())
    {
        failure = QStringLiteral("Event type and status must be nonempty");
    }
    else if (eventId.isNull())
    {
        failure = QStringLiteral("Event ID must be non-null");
    }
    else if (!this->initialize(failure))
    {
        // initialize() provides the diagnostic and does not alter existing data.
    }
    else if (this->file.size() != this->knownSize)
    {
        failure = QStringLiteral("Journal changed outside its single writer");
        this->failed = true;
    }
    else if (const auto existing = this->eventsById.constFind(eventId.toString(QUuid::WithoutBraces));
             existing != this->eventsById.cend())
    {
        if (existing->type != type || existing->status != status || existing->payload != payload)
        {
            failure = QStringLiteral("Event ID already exists with different content");
        }
        else if (writtenEvent)
        {
            *writtenEvent = *existing;
        }
    }
    else if (this->nextSequence > largestExactJsonInteger)
    {
        failure = QStringLiteral("Event sequence exceeds exact JSON integer range");
    }
    else
    {
        ExecutionEvent event;
        event.eventId = eventId;
        event.runId = this->runId;
        event.sequence = this->nextSequence;
        event.utc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        event.type = type;
        event.status = status;
        event.payload = payload;
        QByteArray bytes = QJsonDocument(toJson(event)).toJson(QJsonDocument::Compact);
        bytes.append('\n');
        const qint64 written = this->file.write(bytes);
        if (written != bytes.size() || !this->file.flush())
        {
            failure = QStringLiteral("Journal write/flush failed: %1")
                          .arg(this->file.errorString());
            this->failed = true;
        }
        else
        {
            this->knownSize += bytes.size();
            ++this->nextSequence;
            this->eventsById.insert(eventId.toString(QUuid::WithoutBraces),event);
            if (writtenEvent)
            {
                *writtenEvent = event;
            }
        }
    }
    if (error)
    {
        *error = failure;
    }
    return failure.isEmpty();
}
