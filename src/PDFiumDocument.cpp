#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"

#include <QDir>
#include <QFile>

#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct PDFiumDocument::Reader
{
#ifdef Q_OS_WIN
    HANDLE file = INVALID_HANDLE_VALUE;
    FPDF_FILEACCESS access = {};

    ~Reader()
    {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }

    static int readBlock(void *parameter, unsigned long position,
                         unsigned char *buffer, unsigned long size)
    {
        assertPDFiumProcessLockHeld();
        auto &reader = *static_cast<Reader *>(parameter);
        if (!buffer || position > reader.access.m_FileLen
            || size > reader.access.m_FileLen - position)
            return 0;

        LARGE_INTEGER offset;
        offset.QuadPart = position;
        if (!SetFilePointerEx(reader.file, offset, nullptr, FILE_BEGIN))
            return 0;
        while (size != 0) {
            DWORD bytesRead = 0;
            if (!ReadFile(reader.file, buffer, size, &bytesRead, nullptr)
                || bytesRead == 0)
                return 0;
            buffer += bytesRead;
            size -= bytesRead;
        }
        return 1;
    }
#endif
};

PDFiumDocument::PDFiumDocument() noexcept = default;

PDFiumDocument::~PDFiumDocument()
{
    reset();
}

PDFiumDocument::PDFiumDocument(PDFiumDocument &&other) noexcept
    : m_reader(std::move(other.m_reader)),
      m_document(std::exchange(other.m_document, nullptr)),
      m_error(std::exchange(other.m_error, FPDF_ERR_SUCCESS))
{
}

PDFiumDocument &PDFiumDocument::operator=(PDFiumDocument &&other) noexcept
{
    if (this != &other) {
        reset();
        m_reader = std::move(other.m_reader);
        m_document = std::exchange(other.m_document, nullptr);
        m_error = std::exchange(other.m_error, FPDF_ERR_SUCCESS);
    }
    return *this;
}

void PDFiumDocument::reset()
{
    if (m_document) {
        assertPDFiumProcessLockHeld();
        FPDF_CloseDocument(m_document);
        m_document = nullptr;
    }
    // PDFium may still call the reader while closing the document.
    m_reader.reset();
    m_error = FPDF_ERR_SUCCESS;
}

PDFiumDocument PDFiumDocument::open(const QString &filePath,
                                   const QByteArray &password)
{
    assertPDFiumProcessLockHeld();
    PDFiumDocument result;
    if (filePath.isEmpty() || filePath.contains(QChar::Null)) {
        result.m_error = FPDF_ERR_FILE;
        return result;
    }

#ifdef Q_OS_WIN
    auto reader = std::make_unique<Reader>();
    const QString nativePath = QDir::toNativeSeparators(filePath);
    // Default PDFium file access does not share deletion on Windows. Every
    // source reader must permit atomic replacement while retaining its own
    // original file object until the new saved document has been reopened.
    reader->file = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER fileSize;
    if (reader->file == INVALID_HANDLE_VALUE
        || !GetFileSizeEx(reader->file, &fileSize) || fileSize.QuadPart < 0) {
        result.m_error = FPDF_ERR_FILE;
        return result;
    }
    if (static_cast<quint64>(fileSize.QuadPart)
        <= std::numeric_limits<unsigned long>::max()) {
        reader->access.m_FileLen = static_cast<unsigned long>(fileSize.QuadPart);
        reader->access.m_GetBlock = Reader::readBlock;
        reader->access.m_Param = reader.get();
        result.m_document = FPDF_LoadCustomDocument(&reader->access, password.constData());
        if (!result.m_document) {
            result.m_error = FPDF_GetLastError();
            return result;
        }
        result.m_reader = std::move(reader);
        return result;
    }
    // PDFium's custom access length is only 32 bits on Windows. Preserve the
    // existing native loader for larger files instead of truncating the input;
    // that API still has its original same-path replacement limitation.
    reader.reset();
#endif

    const QByteArray encodedPath = QFile::encodeName(filePath);
    result.m_document = FPDF_LoadDocument(encodedPath.constData(), password.constData());
    if (!result.m_document)
        result.m_error = FPDF_GetLastError();
    return result;
}
