#include "isolation_launcher.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef Q_OS_WIN
#error The WF12 LPAC launcher is a Windows-only executable.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define INITKNOWNFOLDERS
#include <KnownFolders.h>
#undef INITKNOWNFOLDERS
#include <Shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <networkisolation.h>
#include <io.h>
#include <fcntl.h>

namespace {
constexpr qint64 RequestLimit = 64 * 1024;
constexpr qint64 ResultLimit = 32 * 1024 * 1024;
constexpr qint64 LogLimit = 4 * 1024 * 1024; // Combined; base64 receipt fits host's 8 MiB pipe bound.
constexpr qint64 RuntimeLimit = qint64(2) * 1024 * 1024 * 1024;
constexpr DWORD JobFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
    JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY |
    JOB_OBJECT_LIMIT_PROCESS_TIME | JOB_OBJECT_LIMIT_JOB_TIME;

struct Failure : std::runtime_error {
    QString code;
    Failure(QString c, const char *m) : std::runtime_error(m), code(std::move(c)) {}
};
void require(bool ok, const char *message, const char *code = "ISOLATION_FAILED") {
    if (!ok) throw Failure(QString::fromLatin1(code), message);
}
void win(bool ok, const char *message) {
    if (!ok) throw Failure(QStringLiteral("WIN32_%1").arg(GetLastError()), message);
}
LPCWSTR wide(const QString &s) { return reinterpret_cast<LPCWSTR>(s.utf16()); }
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { close(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) { close(); value = std::exchange(other.value, nullptr); }
        return *this;
    }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
    void close() { if (valid()) CloseHandle(value); value = nullptr; }
};
struct LocalMemory {
    void *value = nullptr;
    ~LocalMemory() { if (value) LocalFree(value); }
};
QString sidString(PSID sid) {
    LPWSTR value = nullptr;
    win(ConvertSidToStringSidW(sid, &value), "Cannot format SID.");
    LocalMemory memory{value};
    return QString::fromWCharArray(value);
}
QByteArray tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL sized = GetTokenInformation(token, kind, nullptr, 0, &size);
    const DWORD sizeError = GetLastError();
    require(size > 0 && size < 1024 * 1024,
        QStringLiteral("Token class %1 size unavailable (returned %2, Win32 %3).")
            .arg(int(kind)).arg(int(sized)).arg(sizeError).toUtf8().constData(), "TOKEN_QUERY_FAILED");
    QByteArray data(size, '\0');
    win(GetTokenInformation(token, kind, data.data(), size, &size), "Cannot inspect token.");
    return data;
}
DWORD tokenDword(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    const auto data = tokenInfo(token, kind);
    require(data.size() == sizeof(DWORD), "Unexpected token information format.");
    DWORD value = 0; std::memcpy(&value, data.constData(), sizeof(value)); return value;
}
QString hostSid() {
    HANDLE raw = nullptr;
    win(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "Cannot inspect launcher token.");
    Handle token(raw); const auto data = tokenInfo(raw, TokenUser);
    return sidString(reinterpret_cast<const TOKEN_USER *>(data.constData())->User.Sid);
}

// Accept only local, absolute DOS paths with unambiguous components. Final paths
// are also obtained from handles; string checks alone never establish containment.
QString absolutePath(const QString &input) {
    QString path = QDir::toNativeSeparators(input);
    require(path.size() >= 3 && path.size() <= 24000 && path[0].isLetter() && path[1] == ':' &&
            path[2] == '\\' && !path.contains(QChar(0)), "Expected a local absolute DOS path.", "INVALID_REQUEST");
    require(path.indexOf(':', 2) < 0, "Alternate streams and device paths are forbidden.", "INVALID_REQUEST");
    const auto parts = path.mid(3).split('\\', Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        require(part != "." && part != ".." && !part.endsWith('.') && !part.endsWith(' ') &&
                !part.contains(QRegularExpression(QStringLiteral("[\x01-\x1f<>\"|?*]"))),
                "Ambiguous path component.", "INVALID_REQUEST");
        const QString stem = part.section('.', 0, 0).toUpper();
        require(!QRegularExpression(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$")).match(stem).hasMatch(),
                "Reserved DOS path component.", "INVALID_REQUEST");
    }
    require(!parts.isEmpty(), "A volume root cannot be managed.", "INVALID_REQUEST");
    path = path.left(3) + parts.join('\\');
    return path;
}
QString finalPath(HANDLE handle) {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    win(count > 0 && count < buffer.size(), "Cannot resolve final path.");
    QString path = QString::fromWCharArray(buffer.data(), int(count));
    require(path.startsWith(QStringLiteral("\\\\?\\")), "Final path is not a local DOS path.");
    return absolutePath(path.mid(4));
}
bool samePath(const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; }
bool below(const QString &child, const QString &parent) {
    return child.startsWith(parent + '\\', Qt::CaseInsensitive);
}
struct Opened {
    QString path;
    Handle handle;
    bool directory = false;
    qint64 size = 0;
};
Opened openPath(const QString &requestedPath, bool directory, bool changeAcl = false, bool output = false) {
    const QString path = absolutePath(requestedPath);
    DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL;
    if (!directory) access |= GENERIC_READ;
    if (changeAcl) access |= WRITE_DAC;
    if (output) access |= WRITE_OWNER;
    Handle handle(CreateFileW(wide(path), access,
        FILE_SHARE_READ | (directory ? FILE_SHARE_WRITE : 0), nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
    win(handle.valid(), "Cannot pin managed path.");
    BY_HANDLE_FILE_INFORMATION info{};
    win(GetFileInformationByHandle(handle.value, &info), "Cannot inspect managed path.");
    require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Reparse point is forbidden.", "PATH_REJECTED");
    require(bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory, "Unexpected path type.", "PATH_REJECTED");
    if (!directory) require(info.nNumberOfLinks == 1, "Hard-linked input/runtime/output is forbidden.", "PATH_REJECTED");
    require(samePath(finalPath(handle.value), path), "Final path differs from requested path.", "PATH_REJECTED");
    const qint64 size = (qint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return {path, std::move(handle), directory, size};
}
struct Pins {
    QSet<QString> paths;
    std::vector<Opened> directories;
    void ancestors(const QString &requestedPath) {
        const QString path = absolutePath(requestedPath);
        QString current = path.left(3);
        // Pin the volume root too, even though it is never an ACL target.
        Handle root(CreateFileW(wide(current), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        win(root.valid(), "Cannot pin volume root.");
        BY_HANDLE_FILE_INFORMATION info{};
        win(GetFileInformationByHandle(root.value, &info), "Cannot inspect volume root.");
        require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Reparse volume root forbidden.");
        if (!paths.contains(current.toUpper())) {
            paths.insert(current.toUpper()); directories.push_back({current, std::move(root), true, 0});
        }
        const auto parts = path.mid(3).split('\\');
        for (const auto &part : parts) {
            current += (current.endsWith('\\') ? QString() : QStringLiteral("\\")) + part;
            if (!paths.contains(current.toUpper())) {
                directories.push_back(openPath(current, true)); paths.insert(current.toUpper());
            }
        }
    }
};
QByteArray readHandle(HANDLE handle, qint64 limit) {
    LARGE_INTEGER zero{};
    win(SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN), "Cannot rewind input.");
    QByteArray bytes;
    char block[65536]; DWORD count = 0;
    do {
        win(ReadFile(handle, block, sizeof(block), &count, nullptr), "Cannot read input.");
        require(bytes.size() + qint64(count) <= limit, "Input exceeds its bound.", "INPUT_LIMIT");
        bytes.append(block, count);
    } while (count);
    return bytes;
}
QByteArray digestHandle(HANDLE handle) {
    LARGE_INTEGER zero{};
    win(SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN), "Cannot rewind hash input.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char block[65536]; DWORD count = 0;
    do {
        win(ReadFile(handle, block, sizeof(block), &count, nullptr), "Cannot hash input.");
        hash.addData(QByteArrayView(block, count));
    } while (count);
    return hash.result().toHex();
}

// Qt's general JSON reader accepts duplicate keys. This deliberately flat
// parser rejects duplicate/unknown keys and accepts only strings and integers.
QJsonObject requestObject(const QByteArray &bytes) {
    qsizetype pos = 0;
    auto ws = [&] { while (pos < bytes.size() && QByteArray(" \r\n\t").contains(bytes[pos])) ++pos; };
    auto string = [&]() -> QString {
        require(pos < bytes.size() && bytes[pos] == '"', "Expected request string.", "INVALID_REQUEST");
        const qsizetype start = pos++;
        bool escaped = false;
        while (pos < bytes.size()) {
            const char c = bytes[pos++];
            if (!escaped && c == '"') {
                QJsonParseError error{};
                const auto doc = QJsonDocument::fromJson('[' + bytes.mid(start, pos - start) + ']', &error);
                require(error.error == QJsonParseError::NoError && doc.isArray() && doc.array().size() == 1 &&
                        doc.array()[0].isString(), "Invalid request string.", "INVALID_REQUEST");
                return doc.array()[0].toString();
            }
            if (!escaped && c == '\\') escaped = true; else escaped = false;
        }
        throw Failure("INVALID_REQUEST", "Unterminated request string.");
    };
    ws(); require(pos < bytes.size() && bytes[pos++] == '{', "Expected request object.", "INVALID_REQUEST");
    QJsonObject result;
    ws();
    while (pos < bytes.size() && bytes[pos] != '}') {
        const auto key = string();
        require(!result.contains(key), "Duplicate request key.", "INVALID_REQUEST");
        ws(); require(pos < bytes.size() && bytes[pos++] == ':', "Missing request colon.", "INVALID_REQUEST"); ws();
        if (pos < bytes.size() && bytes[pos] == '"') result.insert(key, string());
        else {
            const qsizetype start = pos;
            while (pos < bytes.size() && bytes[pos] >= '0' && bytes[pos] <= '9') ++pos;
            const QByteArray number = bytes.mid(start, pos - start);
            bool ok = false; const auto integer = number.toLongLong(&ok);
            require(ok && number.size() <= 8 && (number.size() == 1 || !number.startsWith('0')),
                    "Expected bounded request integer.", "INVALID_REQUEST");
            result.insert(key, double(integer));
        }
        ws();
        if (pos < bytes.size() && bytes[pos] == ',') { ++pos; ws(); require(pos < bytes.size() && bytes[pos] != '}', "Trailing comma.", "INVALID_REQUEST"); }
        else break;
    }
    require(pos < bytes.size() && bytes[pos++] == '}', "Expected end of request.", "INVALID_REQUEST");
    ws(); require(pos == bytes.size(), "Trailing request data.", "INVALID_REQUEST");
    const QSet<QString> expected{"schema", "managedRoot", "jobRoot", "runtimeRoot", "harnessPath", "sourcePath",
        "inputPath", "outputRoot", "mode", "timeoutMs", "pythonSha256", "runtimeSha256", "harnessSha256",
        "contractSha256", "sourceSha256", "inputSha256"};
    const auto keys = result.keys();
    require(QSet<QString>(keys.begin(), keys.end()) == expected, "Unexpected request fields.", "INVALID_REQUEST");
    require(result["schema"] == QStringLiteral("range.workflow.lpac.request/v1"), "Unsupported request schema.", "INVALID_REQUEST");
    for (const auto &key : expected) {
        if (key == "timeoutMs") continue;
        require(result[key].isString() && !result[key].toString().contains(QChar(0)), "Invalid request field type.", "INVALID_REQUEST");
        if (key.endsWith("Sha256")) require(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(result[key].toString()).hasMatch(),
                "Expected lowercase SHA256.", "INVALID_REQUEST");
    }
    require(result["timeoutMs"].isDouble() && result["timeoutMs"].toInt() >= 1 && result["timeoutMs"].toInt() <= 30000,
            "Timeout must be 1..30000 ms.", "INVALID_REQUEST");
    require(result["mode"] == QStringLiteral("run") || result["mode"] == QStringLiteral("test"), "Unsupported mode.", "INVALID_REQUEST");
    return result;
}

struct RuntimeTree {
    std::vector<Opened> paths;
    qint64 total = 0;
    int files = 0;
    void walk(const QString &path, int depth = 0) {
        require(depth <= 32 && paths.size() < 16384, "Runtime tree is too large.", "INPUT_LIMIT");
        paths.push_back(openPath(path, true, true));
        WIN32_FIND_DATAW entry{};
        HANDLE raw = FindFirstFileW(wide(path + QStringLiteral("\\*")), &entry);
        win(raw != INVALID_HANDLE_VALUE, "Cannot enumerate staged runtime.");
        struct FindCloseGuard { HANDLE h; ~FindCloseGuard() { FindClose(h); } } guard{raw};
        do {
            const QString name = QString::fromWCharArray(entry.cFileName);
            if (name == "." || name == "..") continue;
            require(!(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Runtime reparse point forbidden.", "PATH_REJECTED");
            const QString child = absolutePath(path + '\\' + name);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(child, depth + 1);
            else {
                auto opened = openPath(child, false, true);
                total += opened.size;
                require(++files <= 8192 && total <= RuntimeLimit, "Staged runtime exceeds its bound.", "INPUT_LIMIT");
                paths.push_back(std::move(opened));
            }
        } while (FindNextFileW(raw, &entry));
        require(GetLastError() == ERROR_NO_MORE_FILES, "Runtime enumeration failed.");
    }
    QByteArray hash(const QString &root) {
        std::vector<Opened *> files;
        for (auto &path : paths) if (!path.directory) files.push_back(&path);
        const auto relative = [&root](const Opened *file) {
            return file->path.mid(root.size() + 1).replace('\\', '/').toUtf8();
        };
        std::sort(files.begin(), files.end(), [&relative](const auto a, const auto b) {
            return relative(a) < relative(b);
        });
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const auto file : files) {
            hash.addData(relative(file));
            hash.addData(QByteArray(1, '\0'));
            hash.addData(digestHandle(file->handle.value)); hash.addData(QByteArray(1, '\n'));
        }
        return hash.result().toHex();
    }
};

QString originalSecurity(Opened &path, const QString &owner) {
    PSID sid = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    const SECURITY_INFORMATION info = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
        (path.directory ? LABEL_SECURITY_INFORMATION : 0);
    const DWORD status = GetSecurityInfo(path.handle.value, SE_FILE_OBJECT, info, &sid, nullptr, nullptr, nullptr, &sd);
    require(status == ERROR_SUCCESS, "Cannot snapshot ACL.");
    LocalMemory memory{sd};
    require(samePath(sidString(sid), owner), "Managed staging must be owned by launcher user.", "PATH_REJECTED");
    LPWSTR text = nullptr;
    win(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
        DACL_SECURITY_INFORMATION | (path.directory ? LABEL_SECURITY_INFORMATION : 0), &text, nullptr), "Cannot serialize original ACL.");
    LocalMemory converted{text}; return QString::fromWCharArray(text);
}
bool setSecurity(HANDLE handle, const QString &sddl, bool label) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wide(sddl), SDDL_REVISION_1, &sd, nullptr)) return false;
    LocalMemory memory{sd};
    PACL dacl = nullptr, sacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) return false;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (!GetSecurityDescriptorControl(sd, &control, &revision)) return false;
    SECURITY_INFORMATION flags = DACL_SECURITY_INFORMATION |
        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
    if (label) {
        if (!GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted)) return false;
        flags |= LABEL_SECURITY_INFORMATION;
    }
    return SetSecurityInfo(handle, SE_FILE_OBJECT, flags, nullptr, nullptr, dacl, sacl) == ERROR_SUCCESS;
}
struct Cleanup {
    struct Acl { Opened *path; QString sddl; bool label; };
    QString manifest;
    QString profile;
    PSID sid = nullptr;
    std::vector<Acl> acls;
    QJsonObject *receipt = nullptr;
    bool active = false;
    bool profileCreated = false;
    bool complete = false;
    ~Cleanup() { if (active) finish(); if (sid) FreeSid(sid); }
    void save(const QString &state) {
        QJsonArray paths;
        for (const auto &entry : acls) paths.append(QJsonObject{{"path",entry.path->path}, {"sddl",entry.sddl}, {"label",entry.label}});
        const auto bytes = QJsonDocument(QJsonObject{{"schema","range.workflow.lpac.cleanup/v1"}, {"state",state},
            {"profile",profile}, {"paths",paths}, {"jobKillOnLauncherExit",true}}).toJson(QJsonDocument::Compact);
        QSaveFile file(manifest);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot save recovery manifest.", "CLEANUP_MANIFEST_FAILED");
    }
    bool finish() noexcept {
        if (!active) return complete;
        bool ok = true;
        int failedAcls = 0;
        for (auto entry = acls.rbegin(); entry != acls.rend(); ++entry)
            if (!setSecurity(entry->path->handle.value, entry->sddl, entry->label)) { ok = false; ++failedAcls; }
        const HRESULT profileResult = profileCreated ? DeleteAppContainerProfile(wide(profile)) : S_OK;
        if (FAILED(profileResult)) ok = false;
        bool manifestSaved = true;
        try { save(ok ? QStringLiteral("complete") : QStringLiteral("recovery_required")); }
        catch (...) { ok = false; manifestSaved = false; }
        active = false;
        complete = ok;
        // Also report cleanup performed during exception unwinding, before the
        // outer failure handler publishes the receipt. Do not replace its phase
        // or original startup failure with the cleanup phase.
        try {
            if (receipt) {
                (*receipt)["cleanupComplete"] = ok;
                (*receipt)["cleanupAttempted"] = true;
                (*receipt)["cleanup"] = QJsonObject{{"aclRestoreAttempts",int(acls.size())},
                    {"aclRestoreFailures",failedAcls}, {"profileDeleteAttempted",profileCreated},
                    {"profileDeleteHResult",double(static_cast<LONG>(profileResult))},
                    {"manifestSaved",manifestSaved}, {"state",ok ? "complete" : "recovery_required"}};
                if (!ok) (*receipt)["cleanupFailureCode"] = "CLEANUP_REQUIRED";
            }
        } catch (...) { /* The durable manifest remains the recovery authority. */ }
        return ok;
    }
};

QString quoted(const QString &path) {
    // Paths reject quotes. Double trailing backslashes in CRT quoting.
    QString result = path; int trailing = 0;
    for (int i = result.size() - 1; i >= 0 && result[i] == '\\'; --i) ++trailing;
    result += QString(trailing, '\\'); return '"' + result + '"';
}
struct Attributes {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    bool initialized = false;
    Attributes() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 5, 0, &size);
        require(size != 0, "Startup attributes unavailable.");
        bytes.resize(size); value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        win(InitializeProcThreadAttributeList(value, 5, 0, &size), "Cannot initialize startup attributes."); initialized = true;
    }
    ~Attributes() { if (initialized) DeleteProcThreadAttributeList(value); }
    void add(DWORD_PTR kind, void *data, SIZE_T size) {
        win(UpdateProcThreadAttribute(value, 0, kind, data, size, nullptr, nullptr), "Required startup security attribute unavailable.");
    }
};

// A single controlled caller-side LOCALAPPDATA intervention. This process owns
// its environment; it does not change the parent/system environment, and the
// worker still receives the separately constructed explicit allowlist block.
struct ScopedCallerLocalAppData {
    std::vector<wchar_t> buffer = std::vector<wchar_t>(32768);
    std::wstring previous;
    bool presentBefore = false;
    bool presentDuring = false;
    bool presentAfter = false;
    bool changed = false;
    bool restoreAttempted = false;
    bool restored = false;
    DWORD restoreError = ERROR_SUCCESS;

    ScopedCallerLocalAppData() {
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), DWORD(buffer.size()));
        const DWORD error = count ? ERROR_SUCCESS : GetLastError();
        require(count < buffer.size() && (count || error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND),
                "Cannot snapshot caller LOCALAPPDATA.", "CALLER_ENVIRONMENT_FAILED");
        presentBefore = count || error != ERROR_ENVVAR_NOT_FOUND;
        if (presentBefore) previous.assign(buffer.data(), count);

        PWSTR raw = nullptr;
        const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
        struct CoMemory { void *value; ~CoMemory() { if (value) CoTaskMemFree(value); } } memory{raw};
        require(SUCCEEDED(result) && raw, "Cannot derive current-user LocalAppData through Windows API.", "CALLER_ENVIRONMENT_FAILED");
        const std::wstring derived = absolutePath(QString::fromWCharArray(raw)).toStdWString();
        win(SetEnvironmentVariableW(L"LOCALAPPDATA", derived.c_str()), "Cannot initialize caller LOCALAPPDATA.");
        changed = true;
        // No throwing operation follows the mutation inside the constructor;
        // callers can safely rely on destruction to restore after an exception.
        presentDuring = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), DWORD(buffer.size())) > 0;
    }
    ~ScopedCallerLocalAppData() { if (changed) restore(); }
    ScopedCallerLocalAppData(const ScopedCallerLocalAppData &) = delete;
    ScopedCallerLocalAppData &operator=(const ScopedCallerLocalAppData &) = delete;

    bool restore() noexcept {
        if (!changed) return restored;
        restoreAttempted = true;
        const BOOL ok = SetEnvironmentVariableW(L"LOCALAPPDATA", presentBefore ? previous.c_str() : nullptr);
        restoreError = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok) return false;
        changed = false;
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), DWORD(buffer.size()));
        const DWORD error = count ? ERROR_SUCCESS : GetLastError();
        presentAfter = count || error != ERROR_ENVVAR_NOT_FOUND;
        restored = count < buffer.size() && (count || error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND) &&
            presentAfter == presentBefore && (!presentBefore ||
                (count == previous.size() && std::wmemcmp(buffer.data(), previous.data(), count) == 0));
        return restored;
    }
    QJsonObject observation() const {
        return {{"variable","LOCALAPPDATA"}, {"source","SHGetKnownFolderPath(FOLDERID_LocalAppData,currentUser)"},
            {"presentBeforeObserved",presentBefore}, {"presentDuringObserved",presentDuring},
            {"restoreAttempted",restoreAttempted}, {"restoredObserved",restored},
            {"presentAfterObserved",presentAfter}, {"restoreWin32Error",double(restoreError)},
            {"workerExplicitEnvironmentUnchanged",true}, {"pathValuesDisclosed",false}};
    }
};
void pipe(Handle &read, Handle &write) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE a = nullptr, b = nullptr;
    win(CreatePipe(&a, &b, &security, 65536), "Cannot create worker pipe.");
    read = Handle(a); write = Handle(b);
    win(SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0), "Cannot protect host pipe.");
}
void drain(HANDLE pipe, QByteArray &data, qint64 &combined) {
    char block[65536];
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) return;
            win(false, "Cannot inspect worker pipe.");
        }
        if (!available) return;
        DWORD got = 0;
        win(ReadFile(pipe, block, std::min<DWORD>(available, sizeof(block)), &got, nullptr), "Cannot drain worker pipe.");
        require(combined + got <= LogLimit, "Combined worker stdout/stderr exceeded 4 MiB.", "OUTPUT_LIMIT");
        data.append(block, got); combined += got;
    }
}
qint64 outputSize(const QString &path, int depth = 0, int *seen = nullptr) {
    int local = 0; if (!seen) seen = &local;
    require(depth <= 8, "Output nesting exceeds its bound.", "OUTPUT_LIMIT");
    // Prevent worker directory replacement between reparse checks and enumeration.
    auto pinned = openPath(path, true);
    WIN32_FIND_DATAW entry{}; const HANDLE raw = FindFirstFileW(wide(path + QStringLiteral("\\*")), &entry);
    win(raw != INVALID_HANDLE_VALUE, "Cannot monitor output directory.");
    struct Guard { HANDLE h; ~Guard() { FindClose(h); } } guard{raw};
    qint64 total = 0;
    do {
        const auto name = QString::fromWCharArray(entry.cFileName);
        if (name == "." || name == "..") continue;
        require(++*seen <= 128 && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Output entry/reparse bound exceeded.", "OUTPUT_LIMIT");
        const QString child = absolutePath(path + '\\' + name);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += outputSize(child, depth + 1, seen);
        else total += (qint64(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
        require(total <= ResultLimit, "Monitored output exceeded 32 MiB.", "OUTPUT_LIMIT");
    } while (FindNextFileW(raw, &entry));
    require(GetLastError() == ERROR_NO_MORE_FILES, "Output enumeration failed.");
    return total;
}
QJsonObject observeToken(HANDLE process, PSID expected) {
    HANDLE raw = nullptr; win(OpenProcessToken(process, TOKEN_QUERY, &raw), "Cannot inspect suspended worker token.");
    Handle token(raw);
    const DWORD appContainer = tokenDword(raw, TokenIsAppContainer);
    const DWORD lpac = tokenDword(raw, TokenIsLessPrivilegedAppContainer);
    const DWORD children = tokenDword(raw, TokenChildProcessFlags);
    const auto package = tokenInfo(raw, TokenAppContainerSid);
    const auto sid = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION *>(package.constData())->TokenAppContainer;
    const auto caps = tokenInfo(raw, TokenCapabilities);
    const DWORD count = reinterpret_cast<const TOKEN_GROUPS *>(caps.constData())->GroupCount;
    const auto integrity = tokenInfo(raw, TokenIntegrityLevel);
    const auto integritySid = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(integrity.constData())->Label.Sid;
    const DWORD rid = *GetSidSubAuthority(integritySid, *GetSidSubAuthorityCount(integritySid) - 1);
    require(appContainer == 1 && lpac == 1 && sid && EqualSid(sid, expected) && count == 0 &&
            rid == SECURITY_MANDATORY_LOW_RID && children == PROCESS_CREATION_CHILD_PROCESS_RESTRICTED,
            "Suspended worker token does not match required LPAC policy.", "TOKEN_MISMATCH");
    return {{"appContainer",true}, {"lessPrivilegedAppContainer",true}, {"appContainerSid",sidString(sid)},
        {"capabilityCount",int(count)}, {"integrityRid",int(rid)}, {"childProcessFlags",int(children)}};
}
void verifyNoLoopback(PSID expected) {
    // Microsoft exposes this API from Firewallapi.dll without an import
    // library in SDK 26100. Resolve only the system directory DLL; an absent
    // API is a failed verification, never an isolation downgrade.
    struct SystemModule {
        HMODULE handle;
        ~SystemModule() { if (handle) FreeLibrary(handle); }
    } firewall{LoadLibraryExW(L"Firewallapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    require(firewall.handle != nullptr, "System network isolation API is unavailable.", "NETWORK_POLICY_UNVERIFIED");
    const auto getConfiguration = reinterpret_cast<decltype(&NetworkIsolationGetAppContainerConfig)>(
        GetProcAddress(firewall.handle, "NetworkIsolationGetAppContainerConfig"));
    require(getConfiguration != nullptr, "Network isolation query is unavailable.", "NETWORK_POLICY_UNVERIFIED");
    DWORD count = 0; PSID_AND_ATTRIBUTES entries = nullptr;
    const DWORD status = getConfiguration(&count, &entries);
    require(status == ERROR_SUCCESS, "Cannot inspect AppContainer loopback exemption.", "NETWORK_POLICY_UNVERIFIED");
    struct ConfigMemory {
        DWORD count; PSID_AND_ATTRIBUTES entries;
        ~ConfigMemory() {
            for (DWORD i = 0; i < count; ++i) HeapFree(GetProcessHeap(), 0, entries[i].Sid);
            if (entries) HeapFree(GetProcessHeap(), 0, entries);
        }
    } memory{count, entries};
    for (DWORD i = 0; i < count; ++i)
        require(!EqualSid(expected, entries[i].Sid), "New worker SID has a loopback exemption.", "NETWORK_POLICY_MISMATCH");
}
QString osVersion() {
    using GetVersion = LONG (WINAPI *)(OSVERSIONINFOW *);
    const auto call = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
    require(call && call(&version) == 0, "Cannot observe OS build.");
    return QStringLiteral("%1.%2.%3").arg(version.dwMajorVersion).arg(version.dwMinorVersion).arg(version.dwBuildNumber);
}
QJsonObject limits() {
    return {{"commitBytes",double(512 * 1024 * 1024)}, {"cpuUserTime100ns",double(150000000)},
        {"activeProcesses",1}, {"combinedStdoutStderrBytes",double(LogLimit)}, {"outputBytes",double(ResultLimit)},
        {"outputHardQuota",false}, {"pollIntervalMs",10}};
}

int run(const QString &requestPath, QJsonObject &receipt) {
    receipt["phase"] = "validate_request";
    const QString request = absolutePath(requestPath);
    Pins pins; pins.ancestors(QFileInfo(request).path());
    auto requestFile = openPath(request, false);
    const auto spec = requestObject(readHandle(requestFile.handle.value, RequestLimit));
    const QString managed = absolutePath(spec["managedRoot"].toString());
    const QString jobRoot = absolutePath(spec["jobRoot"].toString());
    const QString runtimeRoot = absolutePath(spec["runtimeRoot"].toString());
    const QString outputRoot = absolutePath(spec["outputRoot"].toString());
    require(below(jobRoot, managed) && below(runtimeRoot, managed) && !below(jobRoot, runtimeRoot) &&
            !below(runtimeRoot, jobRoot) && !samePath(jobRoot, runtimeRoot), "Invalid managed staging roots.", "PATH_REJECTED");
    require(samePath(absolutePath(QFileInfo(outputRoot).path()), jobRoot) && below(request, jobRoot),
            "Output must be fresh direct child of job root; request must be inside job root.", "PATH_REJECTED");
    pins.ancestors(managed); pins.ancestors(jobRoot); pins.ancestors(runtimeRoot);
    const QString harness = absolutePath(spec["harnessPath"].toString());
    const QString source = absolutePath(spec["sourcePath"].toString());
    const QString input = absolutePath(spec["inputPath"].toString());
    require(below(harness, jobRoot) && below(source, jobRoot) && below(input, jobRoot) &&
            QFileInfo(harness).fileName() == "extension_child_v2.py", "Input/harness outside fixed job protocol.", "PATH_REJECTED");
    const QString contract = absolutePath(QFileInfo(harness).path() + QStringLiteral("/scientific_contract.py"));
    const QString python = runtimeRoot + QStringLiteral("\\python.exe");
    const QString manifest = jobRoot + QStringLiteral("\\wf12-cleanup.json");
    require(!QFileInfo::exists(manifest), "Job has a prior cleanup manifest; host recovery is required.", "RECOVERY_REQUIRED");
    QSet<QString> distinct;
    for (const auto &path : {request, harness, source, input, contract, manifest, python}) {
        require(!distinct.contains(path.toUpper()), "Protocol paths overlap.", "PATH_REJECTED"); distinct.insert(path.toUpper());
    }
    std::vector<Opened> inputs;
    for (const auto &path : {harness, contract, source, input}) {
        pins.ancestors(QFileInfo(path).path()); inputs.push_back(openPath(path, false, true));
    }
    require(inputs[0].size <= RequestLimit && inputs[1].size <= 1024 * 1024 && inputs[2].size <= RequestLimit && inputs[3].size <= ResultLimit,
            "Scientific protocol input exceeds bound.", "INPUT_LIMIT");
    RuntimeTree runtime; runtime.walk(runtimeRoot);
    receipt["phase"] = "verify_runtime_hashes";
    Opened *pythonFile = nullptr;
    for (auto &path : runtime.paths) if (!path.directory && samePath(path.path, python)) pythonFile = &path;
    require(pythonFile, "Staged runtime lacks fixed python.exe.", "RUNTIME_MISMATCH");
    QJsonObject observed;
    observed["runtimeSha256"] = QString::fromLatin1(runtime.hash(runtimeRoot));
    observed["pythonSha256"] = QString::fromLatin1(digestHandle(pythonFile->handle.value));
    const QStringList names{"harnessSha256", "contractSha256", "sourceSha256", "inputSha256"};
    for (qsizetype i = 0; i < names.size(); ++i) observed[names[i]] = QString::fromLatin1(digestHandle(inputs[i].handle.value));
    QJsonObject declared;
    for (auto key = observed.begin(); key != observed.end(); ++key) {
        declared[key.key()] = spec[key.key()];
        require(key.value() == spec[key.key()], "Frozen file/runtime hash does not match.", "HASH_MISMATCH");
    }
    receipt["declaredHashes"] = declared; receipt["observedHashes"] = observed;
    receipt["osVersionObserved"] = osVersion(); receipt["timeoutMs"] = spec["timeoutMs"];
    receipt["phase"] = "prepare_output_and_acl_snapshot";
    // Only the fresh output is created. Never modify the source private runtime.
    win(CreateDirectoryW(wide(outputRoot), nullptr), "Output directory must be fresh.");
    auto outputDirectory = openPath(outputRoot, true, true, true);
    const QString owner = hostSid();
    Cleanup cleanup; cleanup.manifest = manifest; cleanup.receipt = &receipt;
    cleanup.profile = QStringLiteral("RangeWF12.%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    receipt["profile"] = cleanup.profile; receipt["cleanupManifest"] = manifest;
    for (auto &path : runtime.paths) cleanup.acls.push_back({&path, originalSecurity(path, owner), false});
    for (auto &path : inputs) cleanup.acls.push_back({&path, originalSecurity(path, owner), false});
    cleanup.acls.push_back({&outputDirectory, originalSecurity(outputDirectory, owner), true});
    cleanup.save("prepared"); cleanup.active = true;
    receipt["cleanupComplete"] = false;
    receipt["phase"] = "create_profile";
    // The prepared manifest already names the profile, closing the creation/crash recovery gap.
    const HRESULT created = CreateAppContainerProfile(wide(cleanup.profile), L"Range WF12 worker", L"Disposable zero-capability LPAC worker", nullptr, 0, &cleanup.sid);
    require(SUCCEEDED(created) && cleanup.sid, "Cannot create a new AppContainer profile.", "PROFILE_FAILED");
    cleanup.profileCreated = true;
    const QString workerSid = sidString(cleanup.sid);
    receipt["phase"] = "apply_staged_acls";
    for (auto &entry : cleanup.acls) {
        const bool output = entry.path == &outputDirectory;
        // Protected ACL removes inherited broad package grants. Only disposable,
        // owner-verified staging is changed; original ACLs are saved first.
        const QString inheritance = output ? QStringLiteral("OICI") : QString();
        // OW suppresses the owner's implicit WRITE_DAC. Host access remains
        // explicitly granted, while the restricting package SID gets only RX/Modify.
        const QString prefix = QStringLiteral("D:P(A;%1;FA;;;SY)(A;%1;FA;;;BA)(A;%1;FA;;;%2)(A;%1;RC;;;OW)").arg(inheritance, owner);
        QString acl = prefix + QStringLiteral("(A;%1;0x%2;;;%3)")
            .arg(output ? QStringLiteral("OICI") : QString(),
                 QString::number(output ? FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE : FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, 16), workerSid);
        if (output) acl += QStringLiteral("S:(ML;OICI;NW;;;LW)");
        require(setSecurity(entry.path->handle.value, acl, output), "Cannot enforce managed ACL.", "ACL_FAILED");
    }
    cleanup.save("running");
    receipt["phase"] = "verify_loopback_policy";
    verifyNoLoopback(cleanup.sid); receipt["loopbackExemptionObserved"] = false;
    receipt["phase"] = "configure_job_and_startup_attributes";
    Handle outRead, outWrite, errRead, errWrite;
    pipe(outRead, outWrite); pipe(errRead, errWrite);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    win(in.valid(), "Cannot create null stdin.");
    Handle job(CreateJobObjectW(nullptr, nullptr)); win(job.valid(), "Cannot create resource job.");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION policy{};
    policy.BasicLimitInformation.LimitFlags = JobFlags;
    policy.BasicLimitInformation.ActiveProcessLimit = 1;
    policy.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 150000000;
    policy.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = 150000000;
    policy.ProcessMemoryLimit = 512 * 1024 * 1024; policy.JobMemoryLimit = policy.ProcessMemoryLimit;
    win(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &policy, sizeof(policy)), "Cannot configure resource job.");
    SECURITY_CAPABILITIES capabilities{}; capabilities.AppContainerSid = cleanup.sid;
    DWORD lpacPolicy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    DWORD childPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    HANDLE inherited[] = {in.value, outWrite.value, errWrite.value};
    HANDLE assignedJobs[] = {job.value};
    Attributes attributes;
    attributes.add(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof(capabilities));
    attributes.add(PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY, &lpacPolicy, sizeof(lpacPolicy));
    attributes.add(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy, sizeof(childPolicy));
    attributes.add(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited));
    attributes.add(PROC_THREAD_ATTRIBUTE_JOB_LIST, assignedJobs, sizeof(assignedJobs));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in.value; startup.StartupInfo.hStdOutput = outWrite.value; startup.StartupInfo.hStdError = errWrite.value;
    startup.lpAttributeList = attributes.value;
    receipt["phase"] = "prepare_environment_and_command";
    wchar_t windowsDirectory[32768]; const UINT length = GetWindowsDirectoryW(windowsDirectory, DWORD(std::size(windowsDirectory)));
    win(length > 0 && length < std::size(windowsDirectory), "Cannot obtain Windows directory.");
    const QString systemRoot = QString::fromWCharArray(windowsDirectory, int(length));
    PWSTR profileStorageRaw = nullptr;
    const HRESULT profileStorageStatus = GetAppContainerFolderPath(wide(workerSid), &profileStorageRaw);
    require(SUCCEEDED(profileStorageStatus) && profileStorageRaw,
            "Cannot obtain the new AppContainer profile storage.", "PROFILE_ENVIRONMENT_FAILED");
    const QString profileStorage = absolutePath(QString::fromWCharArray(profileStorageRaw));
    CoTaskMemFree(profileStorageRaw);
    auto profileStoragePinned = openPath(profileStorage, true);
    receipt["childProfileEnvironment"] = QJsonObject{{"variable","LOCALAPPDATA"},
        {"source","GetAppContainerFolderPath(newly-created-profile)"}, {"pathValuesDisclosed",false},
        {"hostUserDirectoryAclChanged",false}};
    const QStringList env{QStringLiteral("LOCALAPPDATA=") + profileStorage,
        QStringLiteral("OMP_NUM_THREADS=1"), QStringLiteral("OPENBLAS_NUM_THREADS=1"),
        QStringLiteral("SystemRoot=") + systemRoot, QStringLiteral("TEMP=") + outputRoot,
        QStringLiteral("TMP=") + outputRoot, QStringLiteral("WINDIR=") + systemRoot};
    std::vector<wchar_t> environment;
    for (const auto &entry : env) { const auto text = entry.toStdWString(); environment.insert(environment.end(), text.begin(), text.end()); environment.push_back(0); }
    environment.push_back(0);
    receipt["environmentNamesDeclared"] = QJsonArray{"LOCALAPPDATA", "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "SystemRoot", "TEMP", "TMP", "WINDIR"};
    receipt["environmentObserved"] = QJsonValue::Null; // No remote-memory inference pretending to be observation.
    receipt["dependencyVersionsDeclared"] = QJsonObject{{"python","3.13.5"}, {"numpy","2.2.6"}, {"scipy","1.15.3"}};
    receipt["dependencyVersionsObserved"] = QJsonValue::Null;
    const QString resultPath = outputRoot + QStringLiteral("\\result.json");
    const QString command = quoted(python) + QStringLiteral(" -I ") + quoted(harness) + QStringLiteral(" --source ") + quoted(source) +
        QStringLiteral(" --input ") + quoted(input) + QStringLiteral(" --output ") + quoted(resultPath) + " --" + spec["mode"].toString();
    auto commandBuffer = command.toStdWString(); commandBuffer.push_back(0);
    // Separate owners make simultaneous lpApplicationName/lpCurrentDirectory
    // arguments independent of any QString conversion/helper storage policy.
    const std::wstring applicationName = python.toStdWString();
    const std::wstring currentDirectory = outputRoot.toStdWString();
    PROCESS_INFORMATION process{};
    receipt["phase"] = "initialize_caller_local_app_data";
    ScopedCallerLocalAppData callerLocalAppData;
    receipt["phase"] = "create_suspended_worker";
    SetLastError(ERROR_SUCCESS);
    const BOOL processCreated = CreateProcessW(applicationName.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        environment.data(), currentDirectory.c_str(), &startup.StartupInfo, &process);
    // Capture immediately: no QString/JSON construction or other API call may
    // intervene between CreateProcessW failure and its diagnostic error value.
    const DWORD processError = processCreated ? ERROR_SUCCESS : GetLastError();
    const bool callerEnvironmentRestored = callerLocalAppData.restore();
    if (!processCreated) {
        receipt["callerEnvironment"] = callerLocalAppData.observation();
        receipt["processCreation"] = QJsonObject{{"returnedSuccess",false}, {"win32Error",double(processError)}};
        throw Failure(QStringLiteral("WIN32_%1").arg(processError), "Cannot create suspended LPAC worker.");
    }
    Handle worker(process.hProcess), thread(process.hThread);
    // This guard precedes every subsequent throwing operation: even failed job
    // assignment/token inspection cannot leave a suspended process behind.
    struct TerminateGuard {
        HANDLE process; HANDLE job;
        ~TerminateGuard() { TerminateJobObject(job, 1); TerminateProcess(process, 1); WaitForSingleObject(process, 5000); }
    } terminate{worker.value, job.value};
    receipt["callerEnvironment"] = callerLocalAppData.observation();
    receipt["processCreation"] = QJsonObject{{"returnedSuccess",true}, {"win32Error",0}};
    require(callerEnvironmentRestored && callerLocalAppData.presentDuring,
            "Caller LOCALAPPDATA initialization/restoration was not verified.", "CALLER_ENVIRONMENT_FAILED");
    receipt["phase"] = "verify_suspended_job";
    BOOL assigned = FALSE;
    win(IsProcessInJob(worker.value, job.value, &assigned) && assigned, "Worker job membership not observed.");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION actual{};
    win(QueryInformationJobObject(job.value, JobObjectExtendedLimitInformation, &actual, sizeof(actual), nullptr), "Cannot inspect configured job.");
    require(actual.BasicLimitInformation.LimitFlags == JobFlags && actual.BasicLimitInformation.ActiveProcessLimit == 1 &&
            actual.ProcessMemoryLimit == policy.ProcessMemoryLimit && actual.JobMemoryLimit == policy.JobMemoryLimit &&
            actual.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart == 150000000 &&
            actual.BasicLimitInformation.PerJobUserTimeLimit.QuadPart == 150000000, "Job readback differs from required limits.", "JOB_MISMATCH");
    receipt["job"] = QJsonObject{{"assignedObserved",true}, {"limitFlagsObserved",double(actual.BasicLimitInformation.LimitFlags)},
        {"killOnJobCloseObserved",true}, {"breakawayAllowedObserved",false}, {"limits",limits()}};
    receipt["phase"] = "verify_suspended_token";
    receipt["actualToken"] = observeToken(worker.value, cleanup.sid);
    receipt["processId"] = double(process.dwProcessId);
    outWrite.close(); errWrite.close(); in.close();
    receipt["phase"] = "resume_verified_worker";
    win(ResumeThread(thread.value) == 1, "Cannot resume verified worker."); thread.close();
    const ULONGLONG deadline = GetTickCount64() + spec["timeoutMs"].toInt();
    QByteArray stdoutBytes, stderrBytes; qint64 combined = 0; DWORD exitCode = STILL_ACTIVE;
    try {
        receipt["phase"] = "monitor_worker";
        while (true) {
            drain(outRead.value, stdoutBytes, combined); drain(errRead.value, stderrBytes, combined);
            outputSize(outputRoot);
            win(GetExitCodeProcess(worker.value, &exitCode), "Cannot inspect worker exit.");
            if (exitCode != STILL_ACTIVE) break;
            require(GetTickCount64() < deadline, "Worker exceeded wall time limit.", "TIMEOUT");
            const DWORD wait = WaitForSingleObject(worker.value, 10);
            win(wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT, "Worker wait failed.");
        }
        // Job forbids descendants, and termination closes any unforeseen ones before final reads.
        win(TerminateJobObject(job.value, exitCode), "Cannot close worker process tree.");
        drain(outRead.value, stdoutBytes, combined); drain(errRead.value, stderrBytes, combined);
        receipt["exitCode"] = double(exitCode);
        require(exitCode == 0, "Scientific worker exited unsuccessfully.", "PROCESS_FAILED");
        receipt["phase"] = "validate_result";
        outputSize(outputRoot);
        auto result = openPath(resultPath, false);
        require(result.size > 0 && result.size <= ResultLimit && below(result.path, outputDirectory.path), "Invalid final result file.", "OUTPUT_LIMIT");
        const auto data = readHandle(result.handle.value, ResultLimit);
        QJsonParseError parseError{}; const auto doc = QJsonDocument::fromJson(data, &parseError);
        require(parseError.error == QJsonParseError::NoError && doc.isObject(), "Worker result is not a JSON object.", "OUTPUT_INVALID");
        receipt["output"] = QJsonObject{{"path",resultPath}, {"bytes",double(data.size())},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())}};
    } catch (const Failure &error) {
        TerminateJobObject(job.value, 1); TerminateProcess(worker.value, 1); WaitForSingleObject(worker.value, 5000);
        receipt["status"] = "failed"; receipt["code"] = error.code; receipt["message"] = QString::fromUtf8(error.what());
        receipt["failurePhase"] = receipt.value("phase");
    }
    receipt["stdoutBase64"] = QString::fromLatin1(stdoutBytes.toBase64());
    receipt["stderrBase64"] = QString::fromLatin1(stderrBytes.toBase64());
    TerminateJobObject(job.value, 1); WaitForSingleObject(worker.value, 5000);
    receipt["phase"] = "cleanup";
    const bool cleaned = cleanup.finish(); receipt["cleanupComplete"] = cleaned;
    if (!cleaned) {
        if (receipt["status"] != "failed") {
            receipt["status"] = "failed"; receipt["code"] = "CLEANUP_REQUIRED";
            receipt["message"] = "ACL/profile cleanup requires host recovery.";
            receipt["failurePhase"] = "cleanup";
        }
    }
    if (receipt["status"] == "failed") return 1;
    receipt["phase"] = "complete";
    receipt["status"] = "succeeded"; receipt["code"] = "OK"; return 0;
}
}

int rangeWorkflowIsolationLauncherMain(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    _setmode(_fileno(stdout), _O_BINARY);
    QJsonObject receipt{{"schema","range.workflow.lpac.receipt/v1"}, {"backend","windows-lpac/v1"},
        {"outputHardQuota",false}, {"cleanupComplete",true}, {"cleanupAttempted",false},
        {"cleanup",QJsonObject{{"state","not_required"}}}, {"phase","validate_arguments"}, {"limits",limits()},
        {"ordinaryModeEnabled",false},
        {"unprotectedResources",QJsonArray{"AppContainer profile storage", "Some common system resources", "No hard disk quota"}},
        {"compilerMscFullVer",double(_MSC_FULL_VER)}, {"sdkContract","10.0.26100.0"}};
    int status = 1;
    try {
        const auto args = application.arguments();
        if (args.size() == 2 && args[1] == "--probe") {
            receipt = QJsonObject{{"schema","range.workflow.lpac.probe/v1"}, {"backend","windows-lpac/v1"},
                {"available",false}, {"requiresLaunchVerification",true}, {"osVersionObserved",osVersion()},
                {"outputHardQuota",false}, {"limits",limits()}, {"ordinaryModeEnabled",false},
                {"unprotectedResources",QJsonArray{"AppContainer profile storage", "Some common system resources", "No hard disk quota"}}};
            status = 0;
        } else {
            require(args.size() == 4 && args[1] == "run" && args[2] == "--request", "Use --probe or run --request owned-request.json.", "INVALID_REQUEST");
            status = run(args[3], receipt);
        }
    } catch (const Failure &error) {
        receipt["status"] = "failed"; receipt["code"] = error.code; receipt["message"] = QString::fromUtf8(error.what());
        receipt["failurePhase"] = receipt.value("phase");
    } catch (const std::exception &) {
        receipt["status"] = "failed"; receipt["code"] = "LAUNCHER_FAILED"; receipt["message"] = "Launcher failed without an isolated fallback.";
        receipt["failurePhase"] = receipt.value("phase");
    }
    const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Compact) + '\n';
    if (std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) != size_t(bytes.size())) return 2;
    return status;
}

int main(int argc, char **argv) { return rangeWorkflowIsolationLauncherMain(argc, argv); }
