#include "PDFAtomicSaveFile.h"

#ifdef Q_OS_WIN
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryFile>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <vector>

namespace {

QString nativePath(const QString &path)
{
    QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

QString windowsError(DWORD error)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                           | FORMAT_MESSAGE_FROM_SYSTEM
                                           | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, error, 0,
                                       reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    const QString result = length ? QString::fromWCharArray(message, int(length)).trimmed()
                                  : QStringLiteral("Windows error %1").arg(error);
    if (message)
        LocalFree(message);
    return result;
}

bool renameWithOpenReaders(const QString &source, const QString &target, DWORD &error)
{
    const QString from = nativePath(source);
    const QString to = nativePath(target);
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(from.utf16()), DELETE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    const auto closeFile = qScopeGuard([&] { CloseHandle(file); });

    // Same ABI as FILE_RENAME_INFO, including its DWORD Flags union. Some
    // MinGW SDK configurations expose only the older BOOLEAN union member.
    struct RenameInfo {
        DWORD flags;
        HANDLE rootDirectory;
        DWORD fileNameLength;
        WCHAR fileName[1];
    };
    static_assert(offsetof(RenameInfo, fileName) == offsetof(FILE_RENAME_INFO, FileName));
    constexpr DWORD replaceExisting = 0x1;
    constexpr DWORD posixSemantics = 0x2;
    // FileRenameInfoEx is value 22; older MinGW headers hide its name behind
    // a different _WIN32_WINNT guard than the Windows SDK.
    constexpr auto extendedRename = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    const DWORD nameBytes = DWORD(to.size() * sizeof(wchar_t));
    const size_t infoBytes = sizeof(RenameInfo) + nameBytes;
    // max_align_t storage keeps the variable-length native structure aligned.
    std::vector<std::max_align_t> storage(
        (infoBytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    std::memset(storage.data(), 0, storage.size() * sizeof(std::max_align_t));
    auto *info = reinterpret_cast<RenameInfo *>(storage.data());
    info->flags = replaceExisting | posixSemantics;
    info->rootDirectory = nullptr;
    info->fileNameLength = nameBytes;
    // SetFileInformationByHandle also needs a terminated Win32 path when
    // translating it to the native NT path; FileNameLength excludes the NUL.
    std::memcpy(info->fileName, to.utf16(), nameBytes + sizeof(wchar_t));
    if (SetFileInformationByHandle(file, extendedRename, info, DWORD(infoBytes)))
        return true;
    error = GetLastError();
    return false;
}

} // namespace
#endif

PDFAtomicSaveFile::PDFAtomicSaveFile(const QString &filePath)
    : QSaveFile(filePath)
#ifdef Q_OS_WIN
    , m_targetPath(filePath)
#endif
{
}

PDFAtomicSaveFile::~PDFAtomicSaveFile()
{
#ifdef Q_OS_WIN
    if (!m_stagingPath.isEmpty())
        QFile::remove(m_stagingPath);
#endif
}

bool PDFAtomicSaveFile::open(QIODevice::OpenMode mode)
{
#ifdef Q_OS_WIN
    if (m_targetPath.isEmpty() || m_targetPath.contains(QChar::Null)) {
        setErrorString(QStringLiteral("Invalid PDF destination path"));
        return false;
    }
    const QFileInfo target(m_targetPath);
    if (target.exists() && (!target.isFile() || !target.isWritable())) {
        setErrorString(windowsError(ERROR_ACCESS_DENIED));
        return false;
    }
    // Preserve QSaveFile's behavior of writing through a filesystem symlink.
    m_targetPath = target.isSymbolicLink()
        ? (target.exists() ? target.canonicalFilePath() : target.symLinkTarget())
        : target.absoluteFilePath();
    {
        QTemporaryFile staging(m_targetPath + QStringLiteral(".XXXXXX"));
        if (!staging.open()) {
            setErrorString(staging.errorString());
            return false;
        }
        m_stagingPath = staging.fileName();
        // Destroy the temporary file: QTemporaryFile::close() alone retains
        // its native handle and would block QSaveFile's own staging commit.
    }
    setFileName(m_stagingPath);
    if (!QSaveFile::open(mode))
        return false;
    return !target.exists() || setPermissions(target.permissions());
#else
    return QSaveFile::open(mode);
#endif
}

bool PDFAtomicSaveFile::commit()
{
    if (!QSaveFile::commit())
        return false;
#ifdef Q_OS_WIN
    DWORD error = ERROR_SUCCESS;
    if (!renameWithOpenReaders(m_stagingPath, m_targetPath, error)) {
        // Older/network filesystems may not implement extended rename. The
        // ordinary atomic rename still supports Save As and closed targets;
        // never delete the destination or fall back to writing it in place.
        if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED
            && error != ERROR_INVALID_FUNCTION) {
            setErrorString(windowsError(error));
            return false;
        }
        const QString from = nativePath(m_stagingPath);
        const QString to = nativePath(m_targetPath);
        if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                         reinterpret_cast<LPCWSTR>(to.utf16()), MOVEFILE_REPLACE_EXISTING)) {
            setErrorString(windowsError(GetLastError()));
            return false;
        }
    }
    m_stagingPath.clear();
#endif
    return true;
}
