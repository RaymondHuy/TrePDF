#include "PDFDocument.h"
#include "PDFiumSynchronization.h"

#include <QFile>
#include <QtTranslation>

#include <utility>

namespace {

constexpr unsigned long kModifyContentsPermission = 1UL << 3;

class PdfiumRuntime
{
public:
    PdfiumRuntime()
    {
        PDFiumProcessLock pdfiumLock;
        FPDF_InitLibrary();
    }

    ~PdfiumRuntime()
    {
        PDFiumProcessLock pdfiumLock;
        FPDF_DestroyLibrary();
    }
};

PdfiumRuntime &pdfiumRuntime()
{
    static PdfiumRuntime runtime;
    return runtime;
}

QString pdfiumErrorText(unsigned long error)
{
    switch (error) {
    case FPDF_ERR_SUCCESS:
        //% "No error"
        return qtTrId("NoError");
    case FPDF_ERR_UNKNOWN:
        //% "Unknown PDFium error"
        return qtTrId("UnknownPdfiumError");
    case FPDF_ERR_FILE:
        //% "The file could not be opened or read"
        return qtTrId("FileOpenOrReadFailed");
    case FPDF_ERR_FORMAT:
        //% "The file is not a valid PDF"
        return qtTrId("InvalidPdfFile");
    case FPDF_ERR_PASSWORD:
        //% "The PDF requires a password"
        return qtTrId("PdfPasswordRequired");
    case FPDF_ERR_SECURITY:
        //% "The PDF security handler is unsupported"
        return qtTrId("UnsupportedPdfSecurity");
    case FPDF_ERR_PAGE:
        //% "The requested PDF page could not be loaded"
        return qtTrId("PdfPageLoadFailed");
    default:
        //% "PDFium failed with an unknown status"
        return qtTrId("UnknownPdfiumStatus");
    }
}

} // namespace

PDFDocument::PDFDocument(QObject *parent)
    : QObject(parent)
{
    pdfiumRuntime();
    //% "No document open"
    setStatusText(qtTrId("NoDocumentOpen"));
}

PDFDocument::~PDFDocument()
{
    PDFiumProcessLock pdfiumLock;
    closeDocumentLocked();
}

QUrl PDFDocument::source() const
{
    return m_source;
}

int PDFDocument::pageCount() const
{
    return m_pageCount;
}

int PDFDocument::currentPage() const
{
    return m_currentPage;
}

bool PDFDocument::isLoaded() const
{
    return m_document != nullptr;
}

QString PDFDocument::statusText() const
{
    return m_statusText;
}

qreal PDFDocument::currentPageWidth() const
{
    return pageSize(m_currentPage).width();
}

qreal PDFDocument::currentPageHeight() const
{
    return pageSize(m_currentPage).height();
}

qreal PDFDocument::maximumPageWidth() const
{
    return m_maximumPageWidth;
}

bool PDFDocument::open(const QUrl &source)
{
    if (!source.isLocalFile()) {
        //% "Only local PDF files are supported"
        setStatusText(qtTrId("LocalPdfFilesOnly"));
        return false;
    }

    const QString filePath = source.toLocalFile();
    const QByteArray encodedPath = QFile::encodeName(filePath);
    unsigned long error = FPDF_ERR_SUCCESS;
    bool opened = false;
    {
        // This is the sole owner of the PDFium open/metadata/old-document-close
        // transaction. QObject signals are emitted only after the lock is gone.
        PDFiumProcessLock pdfiumLock;
        FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        if (!document) {
            error = FPDF_GetLastError();
        } else {
            const int pageCount = FPDF_GetPageCount(document);
            QVector<QSizeF> pageSizes;
            pageSizes.reserve(pageCount);
            qreal maximumPageWidth = 0.0;
            for (int page = 0; page < pageCount; ++page) {
                double width = 0.0;
                double height = 0.0;
                const QSizeF size = FPDF_GetPageSizeByIndex(
                                        document,
                                        page,
                                        &width,
                                        &height)
                    ? QSizeF(width, height)
                    : QSizeF();
                pageSizes.append(size);
                maximumPageWidth = qMax(maximumPageWidth, size.width());
            }
            const unsigned long permissions = FPDF_GetDocPermissions(document);

            closeDocumentLocked();
            m_document = document;
            m_source = source;
            m_filePath = filePath;
            m_pageCount = pageCount;
            m_currentPage = 0;
            m_pageSizes = std::move(pageSizes);
            m_maximumPageWidth = maximumPageWidth;
            m_allowsContentEditing = permissions == 0xffffffffUL
                || (permissions & kModifyContentsPermission) != 0;
            ++m_revision;
            opened = true;
        }
    }

    if (!opened) {
        setStatusText(pdfiumErrorText(error));
        return false;
    }
    //% "Loaded %n page(s)"
    setStatusText(qtTrId("LoadedPageCount", m_pageCount));
    emitDocumentStateChanged();

    return true;
}

void PDFDocument::close()
{
    if (!m_document && m_source.isEmpty() && m_filePath.isEmpty()) {
        return;
    }

    {
        PDFiumProcessLock pdfiumLock;
        closeDocumentLocked();
    }
    //% "No document open"
    setStatusText(qtTrId("NoDocumentOpen"));
    emitDocumentStateChanged();
}

void PDFDocument::setCurrentPage(int page)
{
    if (!m_document || m_pageCount <= 0) {
        return;
    }

    const int clampedPage = qBound(0, page, m_pageCount - 1);
    if (m_currentPage == clampedPage) {
        return;
    }

    m_currentPage = clampedPage;
    emit currentPageChanged();
    emit currentPageSizeChanged();
}

void PDFDocument::nextPage()
{
    setCurrentPage(m_currentPage + 1);
}

void PDFDocument::previousPage()
{
    setCurrentPage(m_currentPage - 1);
}

QSizeF PDFDocument::pageSize(int page) const
{
    if (!m_document || page < 0 || page >= m_pageSizes.size()) {
        return {};
    }

    return m_pageSizes.at(page);
}

QString PDFDocument::renderFilePath() const
{
    return m_filePath;
}

int PDFDocument::renderRevision() const
{
    return m_revision;
}

void PDFDocument::closeDocumentLocked()
{
    assertPDFiumProcessLockHeld();
    if (m_document) {
        FPDF_CloseDocument(m_document);
    }

    m_document = nullptr;
    m_source = QUrl();
    m_filePath.clear();
    m_pageSizes.clear();
    m_maximumPageWidth = 0.0;
    m_pageCount = 0;
    m_currentPage = 0;
    m_allowsContentEditing = false;
    ++m_revision;
}

void PDFDocument::setStatusText(const QString &statusText)
{
    if (m_statusText == statusText) {
        return;
    }

    m_statusText = statusText;
    emit statusTextChanged();
}

void PDFDocument::emitDocumentStateChanged()
{
    emit sourceChanged();
    emit pageCountChanged();
    emit currentPageChanged();
    emit loadedChanged();
    emit currentPageSizeChanged();
}
