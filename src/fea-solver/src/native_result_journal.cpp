#include "native_result_journal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>

#include <cmath>

namespace
{
constexpr qint64 maximumJournalBytes = 16 * 1024 * 1024;
constexpr quint64 largestExactJsonInteger = (quint64(1) << 53) - 1;
constexpr qint64 hashChunkBytes = 1024 * 1024;

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QString::compare(QDir::cleanPath(left),QDir::cleanPath(right),
                            Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(left) == QDir::cleanPath(right);
#endif
}

QString cleanNativePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool canonicalExistingDirectory(const QString &path, QString &canonical,
                                const QString &description, QString &error)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isAbsolute() || !info.exists() || !info.isDir() || info.isSymLink())
    {
        error = QStringLiteral("%1 must be an absolute existing non-symlink directory").arg(description);
        return false;
    }
    const QString absolute = cleanNativePath(info.absoluteFilePath());
    canonical = cleanNativePath(info.canonicalFilePath());
    if (canonical.isEmpty() || !samePath(absolute,canonical))
    {
        error = QStringLiteral("%1 must use its canonical path and contain no symlinked ancestor")
                    .arg(description);
        return false;
    }
    return true;
}

bool canonicalResultFile(const QString &requestedPath, const QString &resultDirectory,
                         QString &canonical, QString &relativePath, QString &error)
{
    const QFileInfo info(requestedPath);
    if (requestedPath.isEmpty() || !info.isAbsolute() || !info.exists() ||
        !info.isFile() || info.isSymLink())
    {
        error = QStringLiteral("Result frame path must name an existing non-symlink regular file");
        return false;
    }

    const QString absolute = cleanNativePath(info.absoluteFilePath());
    canonical = cleanNativePath(info.canonicalFilePath());
    const QString canonicalParent = cleanNativePath(QFileInfo(absolute).dir().canonicalPath());
    if (canonical.isEmpty() || canonicalParent.isEmpty() ||
        !samePath(QFileInfo(canonical).dir().absolutePath(),canonicalParent) ||
        !samePath(absolute,canonical))
    {
        error = QStringLiteral("Result frame path must not contain a symlinked path component");
        return false;
    }

    QString prefix = resultDirectory;
    if (!prefix.endsWith(QLatin1Char('/')))
    {
        prefix.append(QLatin1Char('/'));
    }
    if (!canonical.startsWith(prefix,
#ifdef Q_OS_WIN
                              Qt::CaseInsensitive
#else
                              Qt::CaseSensitive
#endif
                              ))
    {
        error = QStringLiteral("Result frame path is outside the trusted result directory");
        return false;
    }

    relativePath = QDir(resultDirectory).relativeFilePath(canonical);
    relativePath = QDir::fromNativeSeparators(relativePath);
    if (relativePath.isEmpty() || relativePath == QStringLiteral(".") ||
        relativePath == QStringLiteral("..") || relativePath.startsWith(QStringLiteral("../")))
    {
        error = QStringLiteral("Result frame path does not produce a safe relative path");
        return false;
    }
    return true;
}

bool sha256File(const QString &path, QString &hashText, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("Cannot open persisted result frame '%1' for hashing: %2")
                    .arg(path,file.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray block = file.read(hashChunkBytes);
        if (file.error() != QFileDevice::NoError)
        {
            error = QStringLiteral("Cannot read persisted result frame '%1' for hashing: %2")
                        .arg(path,file.errorString());
            return false;
        }
        if (!block.isEmpty())
        {
            hash.addData(block);
        }
    }
    hashText = QString::fromLatin1(hash.result().toHex());
    return true;
}
}

NativeResultJournal::NativeResultJournal(const QString &jobId, const QString &fileName,
                                         const QString &resultDirectory)
    : jobId(jobId)
    , resultDirectory(resultDirectory)
    , file(fileName)
{
}

std::unique_ptr<NativeResultJournal> NativeResultJournal::createFromEnvironment(QString *error)
{
    const QString jobId = qEnvironmentVariable(jobIdEnvironmentVariable);
    const QString fileName = qEnvironmentVariable(pathEnvironmentVariable);
    const QString resultDirectory = qEnvironmentVariable(resultDirectoryEnvironmentVariable);
    if (jobId.isEmpty() && fileName.isEmpty() && resultDirectory.isEmpty())
    {
        if (error) error->clear();
        return nullptr;
    }
    if (jobId.isEmpty() || fileName.isEmpty() || resultDirectory.isEmpty())
    {
        if (error)
        {
            *error = QStringLiteral("RANGE_NATIVE_JOB_ID, RANGE_NATIVE_RESULT_JOURNAL_PATH and RANGE_NATIVE_RESULT_DIRECTORY are required together");
        }
        return nullptr;
    }
    return create(jobId,fileName,resultDirectory,error);
}

std::unique_ptr<NativeResultJournal> NativeResultJournal::create(
    const QString &jobId, const QString &fileName, const QString &resultDirectory,
    QString *error)
{
    auto fail = [error](const QString &message)
    {
        if (error) *error = message;
        return std::unique_ptr<NativeResultJournal>();
    };
    if (error) error->clear();

    if (jobId.trimmed().isEmpty() || jobId.size() > 256)
    {
        return fail(QStringLiteral("Native result job ID is empty or too long"));
    }
    const QFileInfo requestedJournalInfo(fileName);
    if (fileName.isEmpty() || !requestedJournalInfo.isAbsolute())
    {
        return fail(QStringLiteral("Native result journal path must be absolute"));
    }
    const QFileInfo cleanedJournalInfo(QDir::cleanPath(requestedJournalInfo.absoluteFilePath()));
    if (cleanedJournalInfo.fileName().isEmpty())
    {
        return fail(QStringLiteral("Native result journal path must name a file"));
    }

    QString canonicalOutputDirectory;
    QString directoryError;
    if (!canonicalExistingDirectory(resultDirectory,canonicalOutputDirectory,
                                    QStringLiteral("Native result directory"),directoryError))
    {
        return fail(directoryError);
    }

    QString canonicalJournalParent;
    QString parentError;
    if (!canonicalExistingDirectory(cleanedJournalInfo.absolutePath(),canonicalJournalParent,
                                    QStringLiteral("Native result journal parent"),parentError))
    {
        return fail(parentError);
    }
    const QString canonicalJournalPath = QDir(canonicalJournalParent).filePath(cleanedJournalInfo.fileName());
    const QFileInfo targetInfo(canonicalJournalPath);
    if (targetInfo.exists() || targetInfo.isSymLink())
    {
        return fail(QStringLiteral("Native result journal target already exists; refusing to overwrite it"));
    }

    std::unique_ptr<NativeResultJournal> journal(
        new NativeResultJournal(jobId,canonicalJournalPath,canonicalOutputDirectory));
    if (!journal->file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return fail(QStringLiteral("Cannot create native result journal '%1': %2")
                        .arg(canonicalJournalPath,journal->file.errorString()));
    }
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!journal->file.setPermissions(privatePermissions))
    {
        const QString reason = QStringLiteral("Cannot restrict native result journal permissions: %1")
                                   .arg(journal->file.errorString());
        journal->file.close();
        QFile::remove(canonicalJournalPath);
        return fail(reason);
    }
    journal->available = true;
    return journal;
}

bool NativeResultJournal::isAvailable() const
{
    QMutexLocker locker(&this->mutex);
    return this->available;
}

QString NativeResultJournal::unavailableReason() const
{
    QMutexLocker locker(&this->mutex);
    return this->failureReason;
}

bool NativeResultJournal::appendFrame(const RSolverResultFrame &frame)
{
    QMutexLocker locker(&this->mutex);
    if (!this->available)
    {
        return false;
    }
    return this->appendLocked(frame);
}

bool NativeResultJournal::appendLocked(const RSolverResultFrame &frame)
{
    if (!std::isfinite(frame.checkpointTime))
    {
        this->disableLocked(QStringLiteral("Native result frame has a non-finite checkpoint time"));
        return false;
    }
    if (this->sequence == 0 || this->sequence > largestExactJsonInteger)
    {
        this->disableLocked(QStringLiteral("Native result sequence exceeded the exact JSON integer limit"));
        return false;
    }

    QString actualRecordPath;
    QString relativePath;
    QString pathError;
    if (!canonicalResultFile(frame.actualRecordPath,this->resultDirectory,
                             actualRecordPath,relativePath,pathError))
    {
        this->disableLocked(pathError);
        return false;
    }
    QString logicalPath;
    QString unusedRelativePath;
    if (!canonicalResultFile(frame.logicalPath,this->resultDirectory,
                             logicalPath,unusedRelativePath,pathError))
    {
        this->disableLocked(QStringLiteral("Logical result link: %1").arg(pathError));
        return false;
    }
    Q_UNUSED(logicalPath);
    Q_UNUSED(unusedRelativePath);

    QString digest;
    if (!sha256File(actualRecordPath,digest,pathError))
    {
        this->disableLocked(pathError);
        return false;
    }

    QJsonObject record{
        {QStringLiteral("protocolVersion"),QString::fromLatin1(protocolVersion)},
        {QStringLiteral("jobId"),this->jobId},
        {QStringLiteral("seq"),double(this->sequence)},
        {QStringLiteral("recordNo"),double(frame.recordNo)},
        {QStringLiteral("timeS"),frame.checkpointTime},
        {QStringLiteral("relativePath"),relativePath},
        {QStringLiteral("sha256"),digest}
    };
    QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (line.size() > maximumJournalBytes-this->bytesWritten)
    {
        this->disableLocked(QStringLiteral("Native result journal reached its 16 MiB size limit"));
        return false;
    }

    qint64 offset = 0;
    while (offset < line.size())
    {
        const qint64 written = this->file.write(line.constData()+offset,line.size()-offset);
        if (written <= 0)
        {
            this->disableLocked(QStringLiteral("Cannot append native result journal: %1")
                                    .arg(this->file.errorString()));
            return false;
        }
        offset += written;
    }
    if (!this->file.flush())
    {
        this->disableLocked(QStringLiteral("Cannot flush native result journal: %1")
                                .arg(this->file.errorString()));
        return false;
    }
    this->bytesWritten += line.size();
    ++this->sequence;
    return true;
}

void NativeResultJournal::disableLocked(const QString &reason)
{
    this->available = false;
    this->failureReason = reason;
}
