#include "PDFPageItem.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QThreadPool>
#include <QQuickWindow>

namespace {

using CancellationFlag = std::shared_ptr<std::atomic_bool>;

bool isCancelled(const CancellationFlag &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

QThreadPool &renderThreadPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(10000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

class PDFiumRenderDocument
{
public:
    ~PDFiumRenderDocument()
    {
        close();
    }

    FPDF_DOCUMENT document(const QString &filePath, int revision)
    {
        if (m_document && m_filePath == filePath && m_revision == revision) {
            return m_document;
        }

        close();

        const QByteArray encodedPath = QFile::encodeName(filePath);
        m_document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        if (m_document) {
            m_filePath = filePath;
            m_revision = revision;
        }
        return m_document;
    }

private:
    void close()
    {
        if (m_document) {
            FPDF_CloseDocument(m_document);
        }
        m_document = nullptr;
        m_filePath.clear();
        m_revision = -1;
    }

    FPDF_DOCUMENT m_document = nullptr;
    QString m_filePath;
    int m_revision = -1;
};

FPDF_DWORD pdfiumColor(const QColor &color)
{
    return (static_cast<FPDF_DWORD>(color.alpha()) << 24)
        | (static_cast<FPDF_DWORD>(color.red()) << 16)
        | (static_cast<FPDF_DWORD>(color.green()) << 8)
        | static_cast<FPDF_DWORD>(color.blue());
}

QImage renderPageFromFile(const QString &filePath,
                          int revision,
                          int page,
                          const QSize &pixelSize,
                          const QColor &background,
                          qreal devicePixelRatio,
                          const CancellationFlag &cancellation)
{
    if (filePath.isEmpty() || page < 0 || pixelSize.isEmpty() || isCancelled(cancellation)) {
        return {};
    }

    thread_local PDFiumRenderDocument renderDocument;
    FPDF_DOCUMENT document = renderDocument.document(filePath, revision);
    if (!document || isCancelled(cancellation)) {
        return {};
    }

    FPDF_PAGE pdfPage = FPDF_LoadPage(document, page);
    if (!pdfPage || isCancelled(cancellation)) {
        if (pdfPage) {
            FPDF_ClosePage(pdfPage);
        }
        return {};
    }

    QImage image(pixelSize, QImage::Format_RGB32);
    if (image.isNull() || isCancelled(cancellation)) {
        FPDF_ClosePage(pdfPage);
        return {};
    }
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

    if (isCancelled(cancellation)) {
        return {};
    }

    image.setDevicePixelRatio(devicePixelRatio);
    return image;
}

} // namespace

PDFPageItem::PDFPageItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAntialiasing(false);
}

PDFPageItem::~PDFPageItem()
{
    cancelPendingRender();
}

PDFDocument *PDFPageItem::document() const
{
    return m_document;
}

int PDFPageItem::page() const
{
    return m_page;
}

qreal PDFPageItem::zoom() const
{
    return m_zoom;
}

QColor PDFPageItem::pageColor() const
{
    return m_pageColor;
}

void PDFPageItem::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }

    m_document = document;

    if (m_document) {
        connect(m_document, &PDFDocument::loadedChanged, this, &PDFPageItem::refreshMetrics);
        connect(m_document, &PDFDocument::pageCountChanged, this, &PDFPageItem::refreshMetrics);
        connect(m_document, &QObject::destroyed, this, &PDFPageItem::refreshMetrics);
    }

    refreshMetrics();
    emit documentChanged();
}

void PDFPageItem::setPage(int page)
{
    if (m_page == page) {
        return;
    }

    m_page = page;
    refreshMetrics();
    emit pageChanged();
}

void PDFPageItem::setZoom(qreal zoom)
{
    const qreal clampedZoom = qBound(0.10, zoom, 5.0);
    if (qFuzzyCompare(m_zoom, clampedZoom)) {
        return;
    }

    m_zoom = clampedZoom;
    refreshMetrics();
    emit zoomChanged();
}

void PDFPageItem::setPageColor(const QColor &pageColor)
{
    if (m_pageColor == pageColor) {
        return;
    }

    m_pageColor = pageColor;
    invalidateCache();
    emit pageColorChanged();
}

void PDFPageItem::componentComplete()
{
    QQuickItem::componentComplete();
    scheduleRender();
}

void PDFPageItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        scheduleRender();
    }
}

void PDFPageItem::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);

    switch (change) {
    case ItemVisibleHasChanged:
        if (data.boolValue) {
            scheduleRender();
        } else {
            invalidateCache();
        }
        break;
    case ItemSceneChange:
        if (data.window) {
            scheduleRender();
        } else {
            invalidateCache();
        }
        break;
    case ItemDevicePixelRatioHasChanged:
        invalidateCache();
        break;
    default:
        break;
    }
}

QSGNode *PDFPageItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);

    if (m_clearTexture || width() <= 0.0 || height() <= 0.0) {
        delete node;
        m_pendingImage = QImage();
        m_textureDirty = false;
        m_clearTexture = false;
        return nullptr;
    }

    if (!node && !m_textureDirty) {
        return nullptr;
    }

    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }

    if (m_textureDirty) {
        QQuickWindow *quickWindow = window();
        QSGTexture *texture = quickWindow
            ? quickWindow->createTextureFromImage(m_pendingImage, QQuickWindow::TextureIsOpaque)
            : nullptr;
        m_pendingImage = QImage();
        m_textureDirty = false;

        if (!texture) {
            delete node;
            m_cacheDirty = true;
            return nullptr;
        }
        node->setTexture(texture);
    }

    node->setRect(boundingRect());
    return node;
}

void PDFPageItem::refreshMetrics()
{
    QSizeF pageSize;
    if (m_document && m_document->isLoaded()) {
        pageSize = m_document->pageSize(m_page);
    }

    setImplicitWidth(qMax<qreal>(0.0, pageSize.width() * m_zoom));
    setImplicitHeight(qMax<qreal>(0.0, pageSize.height() * m_zoom));
    invalidateCache();
}

void PDFPageItem::invalidateCache()
{
    cancelPendingRender();
    ++m_requestSerial;
    m_pendingImage = QImage();
    m_textureDirty = false;
    m_clearTexture = true;
    m_cachedPixelSize = QSize();
    m_cachedPage = -1;
    m_cachedZoom = 0.0;
    m_cachedDevicePixelRatio = 0.0;
    m_cachedRevision = -1;
    m_cachedDocument = nullptr;
    m_renderPending = false;
    m_requestedPixelSize = QSize();
    m_requestedPage = -1;
    m_requestedZoom = 0.0;
    m_requestedDevicePixelRatio = 0.0;
    m_requestedRevision = -1;
    m_requestedDocument = nullptr;
    m_cacheDirty = true;
    update();
    scheduleRender();
}

void PDFPageItem::cancelPendingRender()
{
    if (m_renderCancellation) {
        m_renderCancellation->store(true, std::memory_order_relaxed);
        m_renderCancellation.reset();
    }
}

void PDFPageItem::scheduleRender()
{
    if (!isComponentComplete() || m_renderScheduled) {
        return;
    }

    m_renderScheduled = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_renderScheduled = false;
        requestRenderForCurrentSize();
    }, Qt::QueuedConnection);
}

void PDFPageItem::requestRenderForCurrentSize()
{
    if (!isVisible() || !window() || !m_document || !m_document->isLoaded()
        || width() <= 0.0 || height() <= 0.0) {
        return;
    }

    const qreal devicePixelRatio = window()->devicePixelRatio();
    const QSize pixelSize(
        qMax(1, qRound(width() * devicePixelRatio)),
        qMax(1, qRound(height() * devicePixelRatio)));
    const int revision = m_document->renderRevision();
    const bool cacheMatches = !m_cacheDirty
        && m_cachedPixelSize == pixelSize
        && m_cachedPage == m_page
        && qFuzzyCompare(m_cachedZoom, m_zoom)
        && qFuzzyCompare(m_cachedDevicePixelRatio, devicePixelRatio)
        && m_cachedRevision == revision
        && m_cachedDocument == m_document.data();

    if (!cacheMatches) {
        requestRender(pixelSize, devicePixelRatio);
    }
}

void PDFPageItem::requestRender(const QSize &pixelSize, qreal devicePixelRatio)
{
    if (!m_document || !m_document->isLoaded() || pixelSize.isEmpty()) {
        return;
    }

    const QString filePath = m_document->renderFilePath();
    if (filePath.isEmpty()) {
        return;
    }

    const int revision = m_document->renderRevision();
    if (m_renderPending
        && m_requestedPixelSize == pixelSize
        && m_requestedPage == m_page
        && qFuzzyCompare(m_requestedZoom, m_zoom)
        && qFuzzyCompare(m_requestedDevicePixelRatio, devicePixelRatio)
        && m_requestedRevision == revision
        && m_requestedDocument == m_document.data()) {
        return;
    }

    cancelPendingRender();

    const quint64 requestId = ++m_requestSerial;
    PDFDocument *document = m_document.data();
    const int page = m_page;
    const qreal zoom = m_zoom;
    const QColor background = m_pageColor;
    const QPointer<PDFPageItem> item(this);
    const CancellationFlag cancellation = std::make_shared<std::atomic_bool>(false);
    m_renderCancellation = cancellation;

    m_renderPending = true;
    m_requestedPixelSize = pixelSize;
    m_requestedPage = page;
    m_requestedZoom = zoom;
    m_requestedDevicePixelRatio = devicePixelRatio;
    m_requestedRevision = revision;
    m_requestedDocument = document;

    auto *task = QRunnable::create([item, filePath, document, revision, page, zoom, devicePixelRatio, pixelSize, background, requestId, cancellation]() {
        QImage image = renderPageFromFile(
            filePath,
            revision,
            page,
            pixelSize,
            background,
            devicePixelRatio,
            cancellation);
        if (isCancelled(cancellation)) {
            return;
        }

        QObject *app = QCoreApplication::instance();
        if (!app) {
            return;
        }

        QMetaObject::invokeMethod(app, [item, document, revision, page, zoom, devicePixelRatio, pixelSize, image = std::move(image), requestId, cancellation]() {
            if (!item || isCancelled(cancellation)) {
                return;
            }

            item->handleRenderResult(requestId, document, revision, page, zoom, devicePixelRatio, pixelSize, image);
        }, Qt::QueuedConnection);
    });

    renderThreadPool().start(task);
}

void PDFPageItem::handleRenderResult(quint64 requestId,
                                     PDFDocument *document,
                                     int revision,
                                     int page,
                                     qreal zoom,
                                     qreal devicePixelRatio,
                                     const QSize &pixelSize,
                                     const QImage &image)
{
    if (requestId != m_requestSerial
        || m_document.data() != document
        || !m_document
        || m_document->renderRevision() != revision
        || page != m_page
        || !qFuzzyCompare(zoom, m_zoom)
        || !qFuzzyCompare(devicePixelRatio, window() ? window()->devicePixelRatio() : 1.0)
        || pixelSize != m_requestedPixelSize) {
        return;
    }

    m_renderPending = false;
    m_renderCancellation.reset();
    if (image.isNull()) {
        m_cacheDirty = true;
        return;
    }

    m_pendingImage = image;
    m_cachedPixelSize = pixelSize;
    m_cachedPage = page;
    m_cachedZoom = zoom;
    m_cachedDevicePixelRatio = devicePixelRatio;
    m_cachedRevision = revision;
    m_cachedDocument = document;
    m_cacheDirty = false;
    m_textureDirty = true;
    m_clearTexture = false;

    update();
}
