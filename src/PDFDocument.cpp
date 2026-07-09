#include "PDFDocument.h"

#include <QFile>

#include <limits>
#include <utility>

namespace {

class PdfiumRuntime
{
public:
    PdfiumRuntime()
    {
        FPDF_InitLibrary();
    }

    ~PdfiumRuntime()
    {
        FPDF_DestroyLibrary();
    }
};

PdfiumRuntime &pdfiumRuntime()
{
    static PdfiumRuntime runtime;
    return runtime;
}

QString pdfiumErrorText()
{
    switch (FPDF_GetLastError()) {
    case FPDF_ERR_SUCCESS:
        return QStringLiteral("No error");
    case FPDF_ERR_UNKNOWN:
        return QStringLiteral("Unknown PDFium error");
    case FPDF_ERR_FILE:
        return QStringLiteral("The file could not be opened or read");
    case FPDF_ERR_FORMAT:
        return QStringLiteral("The file is not a valid PDF");
    case FPDF_ERR_PASSWORD:
        return QStringLiteral("The PDF requires a password");
    case FPDF_ERR_SECURITY:
        return QStringLiteral("The PDF security handler is unsupported");
    case FPDF_ERR_PAGE:
        return QStringLiteral("The requested PDF page could not be loaded");
    default:
        return QStringLiteral("PDFium failed with an unknown status");
    }
}

FPDF_DWORD pdfiumColor(const QColor &color)
{
    return (static_cast<FPDF_DWORD>(color.alpha()) << 24)
        | (static_cast<FPDF_DWORD>(color.red()) << 16)
        | (static_cast<FPDF_DWORD>(color.green()) << 8)
        | static_cast<FPDF_DWORD>(color.blue());
}

} // namespace

PDFDocument::PDFDocument(QObject *parent)
    : QObject(parent)
{
    pdfiumRuntime();
    setStatusText(QStringLiteral("No document open"));
}

PDFDocument::~PDFDocument()
{
    closeDocument();
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
    qreal maximumWidth = 0.0;
    for (int page = 0; page < m_pageCount; ++page) {
        maximumWidth = qMax(maximumWidth, pageSize(page).width());
    }
    return maximumWidth;
}

bool PDFDocument::open(const QUrl &source)
{
    const QString filePath = source.isLocalFile() ? source.toLocalFile() : source.toString();
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusText(QStringLiteral("Could not open %1").arg(filePath));
        return false;
    }

    QByteArray data = file.readAll();
    if (data.size() > std::numeric_limits<int>::max()) {
        setStatusText(QStringLiteral("The PDF is too large to load"));
        return false;
    }

    FPDF_DOCUMENT document = FPDF_LoadMemDocument(data.constData(), static_cast<int>(data.size()), nullptr);
    if (!document) {
        setStatusText(pdfiumErrorText());
        return false;
    }

    closeDocument();

    m_data = std::move(data);
    m_document = document;
    m_source = source;
    m_pageCount = FPDF_GetPageCount(m_document);
    m_currentPage = 0;
    setStatusText(QStringLiteral("Loaded %1 page(s)").arg(m_pageCount));
    emitDocumentStateChanged();

    return true;
}

void PDFDocument::close()
{
    if (!m_document && m_source.isEmpty() && m_data.isEmpty()) {
        return;
    }

    closeDocument();
    setStatusText(QStringLiteral("No document open"));
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
    if (!m_document || page < 0 || page >= m_pageCount) {
        return {};
    }

    double width = 0.0;
    double height = 0.0;
    if (!FPDF_GetPageSizeByIndex(m_document, page, &width, &height)) {
        return {};
    }

    return {width, height};
}

QImage PDFDocument::renderPage(int page, const QSize &pixelSize, const QColor &background) const
{
    if (!m_document || page < 0 || page >= m_pageCount || pixelSize.isEmpty()) {
        return {};
    }

    FPDF_PAGE pdfPage = FPDF_LoadPage(m_document, page);
    if (!pdfPage) {
        return {};
    }

    QImage image(pixelSize, QImage::Format_ARGB32);
    image.fill(background);

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        pixelSize.width(),
        pixelSize.height(),
        FPDFBitmap_BGRA,
        image.bits(),
        image.bytesPerLine());

    if (!bitmap) {
        FPDF_ClosePage(pdfPage);
        return {};
    }

    FPDFBitmap_FillRect(bitmap, 0, 0, pixelSize.width(), pixelSize.height(), pdfiumColor(background));
    FPDF_RenderPageBitmap(
        bitmap,
        pdfPage,
        0,
        0,
        pixelSize.width(),
        pixelSize.height(),
        0,
        FPDF_ANNOT);

    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(pdfPage);

    return image;
}

void PDFDocument::closeDocument()
{
    if (m_document) {
        FPDF_CloseDocument(m_document);
    }

    m_document = nullptr;
    m_data.clear();
    m_source = QUrl();
    m_pageCount = 0;
    m_currentPage = 0;
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
