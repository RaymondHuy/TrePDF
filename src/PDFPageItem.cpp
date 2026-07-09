#include "PDFPageItem.h"

#include <QPainter>
#include <QQuickWindow>

PDFPageItem::PDFPageItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    setMipmap(false);
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
        connect(m_document, &PDFDocument::currentPageSizeChanged, this, &PDFPageItem::refreshMetrics);
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

void PDFPageItem::paint(QPainter *painter)
{
    if (!m_document || !m_document->isLoaded() || width() <= 0.0 || height() <= 0.0) {
        return;
    }

    const qreal devicePixelRatio = window() ? window()->devicePixelRatio() : 1.0;
    const QSize pixelSize(
        qMax(1, qRound(width() * devicePixelRatio)),
        qMax(1, qRound(height() * devicePixelRatio)));

    if (m_cacheDirty
        || m_cachedPixelSize != pixelSize
        || m_cachedPage != m_page
        || !qFuzzyCompare(m_cachedZoom, m_zoom)
        || m_cachedDocument != m_document.data()) {
        m_cachedImage = m_document->renderPage(m_page, pixelSize, m_pageColor);
        m_cachedImage.setDevicePixelRatio(devicePixelRatio);
        m_cachedPixelSize = pixelSize;
        m_cachedPage = m_page;
        m_cachedZoom = m_zoom;
        m_cachedDocument = m_document.data();
        m_cacheDirty = false;
    }

    painter->fillRect(boundingRect(), m_pageColor);
    if (!m_cachedImage.isNull()) {
        painter->drawImage(boundingRect(), m_cachedImage);
    }
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
    m_cacheDirty = true;
    update();
}
