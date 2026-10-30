#ifndef TRUSTED_ARTIFACT_PATHS_H
#define TRUSTED_ARTIFACT_PATHS_H

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>
#include <limits>
#include <memory>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <windows.h>
#include <io.h>
#elif defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace TrustedArtifactPaths
{
inline Qt::CaseSensitivity pathCaseSensitivity()
{
    // Compare canonical paths, whose spelling is resolved by the filesystem.
    // This also handles case-insensitive macOS volumes without treating a
    // case-variant sibling as contained on a case-sensitive volume.
    return Qt::CaseSensitive;
}

inline bool canonicalFileUnderRoot(const QString &path, const QString &root,
                                   QString *canonicalPath = nullptr)
{
    const QFileInfo fileInfo(path);
    const QFileInfo rootInfo(root);
    if (!fileInfo.exists() || !fileInfo.isFile() || fileInfo.isSymLink() ||
        !rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink())
        return false;

    const Qt::CaseSensitivity sensitivity = pathCaseSensitivity();
    const QString resolvedFile = QDir::cleanPath(QDir::fromNativeSeparators(fileInfo.canonicalFilePath()));
    const QString resolvedRoot = QDir::cleanPath(QDir::fromNativeSeparators(rootInfo.canonicalFilePath()));
    if (resolvedFile.isEmpty() || resolvedRoot.isEmpty()) return false;

    const QString prefix = resolvedRoot + QLatin1Char('/');
    if (!resolvedFile.startsWith(prefix, sensitivity)) return false;
    if (canonicalPath) *canonicalPath = resolvedFile;
    return true;
}

inline QString canonicalPathFromOpenHandle(const QFile &file)
{
    if (!file.isOpen()) return {};
#ifdef Q_OS_WIN
    const intptr_t rawHandle = _get_osfhandle(int(file.handle()));
    if (rawHandle == -1) return {};
    const HANDLE handle = reinterpret_cast<HANDLE>(rawHandle);
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) return {};
    std::wstring buffer(size_t(required), L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, buffer.data(), required, flags);
    if (written == 0 || written >= required) return {};
    buffer.resize(size_t(written));
    QString path = QString::fromWCharArray(buffer.data(), qsizetype(buffer.size()));
    if (path.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive))
        path = QStringLiteral("\\\\") + path.mid(8);
    else if (path.startsWith(QStringLiteral("\\\\?\\"), Qt::CaseInsensitive))
        path = path.mid(4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
#elif defined(Q_OS_LINUX)
    const QByteArray descriptorPath = QByteArray("/proc/self/fd/") + QByteArray::number(file.handle());
    QByteArray buffer(4096, Qt::Uninitialized);
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const ssize_t count = ::readlink(descriptorPath.constData(), buffer.data(), size_t(buffer.size()));
        if (count < 0) return {};
        if (count < buffer.size())
        {
            buffer.resize(qsizetype(count));
            return QDir::cleanPath(QDir::fromNativeSeparators(QString::fromLocal8Bit(buffer)));
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
#elif defined(Q_OS_MACOS)
    char buffer[PATH_MAX + 1]{};
    if (::fcntl(int(file.handle()), F_GETPATH, buffer) != 0) return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(QString::fromLocal8Bit(buffer)));
#else
    const QFileInfo fileInfo(file);
    return QDir::cleanPath(QDir::fromNativeSeparators(fileInfo.canonicalFilePath()));
#endif
}

inline bool isRegularFileHandle(const QFile &file)
{
    if (!file.isOpen()) return false;
#ifdef Q_OS_WIN
    BY_HANDLE_FILE_INFORMATION information{};
    const intptr_t rawHandle = _get_osfhandle(int(file.handle()));
    if (rawHandle == -1) return false;
    const HANDLE handle = reinterpret_cast<HANDLE>(rawHandle);
    return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &information) &&
           (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    struct stat information{};
    return ::fstat(int(file.handle()), &information) == 0 && S_ISREG(information.st_mode);
#else
    return QFileInfo(file).isFile();
#endif
}

inline std::unique_ptr<QFile> openRegularFileNoFollow(const QString &path,
                                                       QString *error = nullptr,
                                                       bool preventConcurrentMutation = false)
{
    if (error) error->clear();
#ifdef Q_OS_WIN
    const DWORD shareMode = FILE_SHARE_READ |
        (preventConcurrentMutation ? 0 : (FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE nativeHandle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES, shareMode, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (nativeHandle == INVALID_HANDLE_VALUE)
    {
        const DWORD nativeError = GetLastError();
        if (error) *error = QStringLiteral("The trusted artifact could not be opened without following links (Win32 error %1).")
                .arg(nativeError);
        return {};
    }
    BY_HANDLE_FILE_INFORMATION nativeInformation{};
    if (GetFileType(nativeHandle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(nativeHandle, &nativeInformation) ||
        (nativeInformation.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    {
        CloseHandle(nativeHandle);
        if (error) *error = QStringLiteral("The opened artifact is not a regular, unlinked file.");
        return {};
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(nativeHandle), _O_RDONLY | _O_BINARY);
    if (descriptor == -1)
    {
        CloseHandle(nativeHandle);
        if (error) *error = QStringLiteral("The trusted artifact handle could not be wrapped for bounded reading.");
        return {};
    }
    auto file = std::make_unique<QFile>();
    if (!file->open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
    {
        _close(descriptor);
        if (error) *error = QStringLiteral("The trusted artifact handle could not be opened for reading.");
        return {};
    }
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    const QByteArray nativePath = QFile::encodeName(path);
    const int nativeFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    const int descriptor = ::open(nativePath.constData(), nativeFlags);
    if (descriptor < 0)
    {
        if (error) *error = QStringLiteral("The trusted artifact could not be opened without following links.");
        return {};
    }
    struct stat nativeInformation{};
    if (::fstat(descriptor, &nativeInformation) != 0 || !S_ISREG(nativeInformation.st_mode) ||
        (preventConcurrentMutation && ::flock(descriptor, LOCK_SH | LOCK_NB) != 0))
    {
        ::close(descriptor);
        if (error) *error = QStringLiteral("The opened artifact is not a regular, unlocked file.");
        return {};
    }
    auto file = std::make_unique<QFile>();
    if (!file->open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
    {
        ::close(descriptor);
        if (error) *error = QStringLiteral("The trusted artifact handle could not be opened for reading.");
        return {};
    }
#else
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly) || !isRegularFileHandle(*file))
    {
        if (error) *error = QStringLiteral("The trusted artifact is not a readable regular file.");
        return {};
    }
#endif
    return file;
}

inline std::unique_ptr<QFile> openFileUnderRoot(const QString &path, const QString &root,
                                                QString *canonicalPath = nullptr,
                                                QString *error = nullptr,
                                                bool preventConcurrentMutation = false)
{
    if (error) error->clear();
    QString lexicalCanonicalPath;
    if (!canonicalFileUnderRoot(path, root, &lexicalCanonicalPath))
    {
        if (error) *error = QStringLiteral("The artifact path is missing, linked, or outside its trusted root.");
        return {};
    }
    auto file = openRegularFileNoFollow(lexicalCanonicalPath, error, preventConcurrentMutation);
    if (!file) return {};

    const QString openedPath = canonicalPathFromOpenHandle(*file);
    QFileInfo rootInfo(root);
    if (openedPath.isEmpty() || !isRegularFileHandle(*file) || rootInfo.isSymLink())
    {
        if (error) *error = QStringLiteral("The opened artifact identity could not be verified.");
        return {};
    }
    const QString resolvedRoot = QDir::cleanPath(QDir::fromNativeSeparators(rootInfo.canonicalFilePath()));
    const QString resolvedFile = QDir::cleanPath(QDir::fromNativeSeparators(openedPath));
    const QString prefix = resolvedRoot + QLatin1Char('/');
    if (resolvedRoot.isEmpty() || resolvedFile.isEmpty() || !resolvedFile.startsWith(prefix, pathCaseSensitivity()))
    {
        if (error) *error = QStringLiteral("The opened artifact handle resolved outside its trusted root.");
        return {};
    }
    if (canonicalPath) *canonicalPath = resolvedFile;
    return file;
}

inline QByteArray readOpenedFileSnapshot(QFile &file, qsizetype maximumBytes,
                                         QString *error = nullptr)
{
    if (error) error->clear();
    const qint64 size = file.size();
    if (!file.isOpen() || size < 0 || maximumBytes < 0 ||
        maximumBytes >= std::numeric_limits<qint64>::max() || size > maximumBytes)
    {
        if (error) *error = QStringLiteral("The opened artifact is unavailable or exceeds the bounded snapshot size.");
        return {};
    }
    if (!file.seek(0))
    {
        if (error) *error = QStringLiteral("The opened artifact could not be rewound for snapshot capture.");
        return {};
    }
    const QByteArray bytes = file.read(qint64(maximumBytes) + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumBytes ||
        bytes.size() != size || file.size() != size)
    {
        if (error) *error = QStringLiteral("The opened artifact changed or could not be read completely during snapshot capture.");
        return {};
    }
    return bytes;
}
}

#endif // TRUSTED_ARTIFACT_PATHS_H
