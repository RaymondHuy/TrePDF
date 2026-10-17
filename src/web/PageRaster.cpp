#include "PageRaster.h"
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

PageRaster::PageRaster(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(60);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        disconnect(m_sceneGraphConnection);
        if (window) {
            m_sceneGraphConnection = connect(window, &QQuickWindow::sceneGraphInvalidated,
                this, [this] { invalidate(true); }, Qt::QueuedConnection);
        }
        invalidate(true);
    });
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        if (m_active && m_document)
            m_request = m_document->requestPage(this, m_page, m_renderWidth);
        m_loading = m_request != 0;
        emit statusChanged();
    });
}

PageRaster::~PageRaster()
{
    if (m_document)
        m_document->cancelRequest(m_request);
}

void PageRaster::setDocument(BrowserDocument *document)
{
    if (m_document == document)
        return;
    if (m_document) {
        m_document->cancelRequest(m_request);
        disconnect(m_document, nullptr, this, nullptr);
    }
    m_document = document;
    if (document)
        connect(document, &BrowserDocument::reset, this, [this] { invalidate(true); });
    invalidate(true);
    emit documentChanged();
}

void PageRaster::setPage(int page)
{
    if (m_page == page)
        return;
    m_page = page;
    invalidate(true);
    emit pageChanged();
}

void PageRaster::setRenderWidth(int width)
{
    width = qBound(0, width, 4096);
    if (m_renderWidth == width)
        return;
    m_renderWidth = width;
    invalidate(false);
    emit renderWidthChanged();
}

void PageRaster::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    invalidate(!active);
    emit activeChanged();
}

void PageRaster::invalidate(bool clear)
{
    m_debounce.stop();
    if (m_document)
        m_document->cancelRequest(m_request);
    m_request = 0;
    m_pendingImage = {};
    m_loading = false;
    m_error.clear();
    if (clear) {
        m_clear = true;
        update();
    }
    if (m_active && m_document && m_renderWidth > 0 && m_page >= 0)
        m_debounce.start();
    emit statusChanged();
}

void PageRaster::acceptImage(int request, QImage image)
{
    if (!m_active || request != m_request)
        return;
    m_request = 0;
    m_loading = false;
    m_clear = false;
    m_pendingImage = std::move(image);
    update();
    emit statusChanged();
}

void PageRaster::acceptError(int request, const QString &error)
{
    if (request != m_request)
        return;
    m_request = 0;
    m_loading = false;
    m_error = error;
    emit statusChanged();
}

QSGNode *PageRaster::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (m_clear || !m_active) {
        delete node;
        return nullptr;
    }
    if (!m_pendingImage.isNull() && window()) {
        auto *texture = window()->createTextureFromImage(m_pendingImage);
        m_pendingImage = {}; // The scene graph owns the only retained page raster.
        if (!texture)
            return node;
        if (!node) {
            node = new QSGSimpleTextureNode;
            node->setOwnsTexture(true);
        }
        // An owning node deletes the previous texture inside setTexture().
        node->setTexture(texture);
        node->setFiltering(QSGTexture::Linear);
    }
    if (node)
        node->setRect(boundingRect());
    return node;
}
