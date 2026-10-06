#pragma once

#include <QByteArray>
#include <QString>

#include <fpdfview.h>

#include <memory>

// Owns the PDFium document and its backing reader. Open/reset and destruction
// of a live document belong to the caller's existing PDFiumProcessLock.
class PDFiumDocument final
{
public:
    PDFiumDocument() noexcept;
    ~PDFiumDocument();
    PDFiumDocument(PDFiumDocument &&other) noexcept;
    PDFiumDocument &operator=(PDFiumDocument &&other) noexcept;

    static PDFiumDocument open(const QString &filePath,
                              const QByteArray &password = {});

    FPDF_DOCUMENT get() const noexcept { return m_document; }
    operator FPDF_DOCUMENT() const noexcept { return get(); }
    unsigned long error() const noexcept { return m_error; }
    void reset();

    PDFiumDocument(const PDFiumDocument &) = delete;
    PDFiumDocument &operator=(const PDFiumDocument &) = delete;

private:
    struct Reader;
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    void trackOpen();
    bool m_telemetryWorkerDocument = false;
#endif
    std::unique_ptr<Reader> m_reader;
    FPDF_DOCUMENT m_document = nullptr;
    unsigned long m_error = FPDF_ERR_SUCCESS;
};
