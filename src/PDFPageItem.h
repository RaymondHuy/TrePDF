#pragma once

#include "PDFDocument.h"

#include <atomic>
#include <memory>

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class QSGNode;

class PDFPageItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PDFPageView)
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(QColor pageColor READ pageColor WRITE setPageColor NOTIFY pageColorChanged FINAL)

public:
    explicit PDFPageItem(QQuickItem *parent = nullptr);
    ~PDFPageItem() override;

    PDFDocument *document() const;
    int page() const;
    qreal zoom() const;
    QColor pageColor() const;

    void setDocument(PDFDocument *document);
    void setPage(int page);
    void setZoom(qreal zoom);
    void setPageColor(const QColor &pageColor);

signals:
    void documentChanged();
    void pageChanged();
    void zoomChanged();
    void pageColorChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    void refreshMetrics();
    void invalidateCache();
    void cancelPendingRender();
    void scheduleRender();
    void requestRenderForCurrentSize();
    void requestRender(const QSize &pixelSize, qreal devicePixelRatio);
    void handleRenderResult(quint64 requestId,
                            PDFDocument *document,
                            int revision,
                            int page,
                            qreal zoom,
                            qreal devicePixelRatio,
                            const QSize &pixelSize,
                            const QImage &image);

    QPointer<PDFDocument> m_document;
    int m_page = 0;
    qreal m_zoom = 1.0;
    QColor m_pageColor = Qt::white;
    bool m_cacheDirty = true;
    QImage m_pendingImage;
    bool m_textureDirty = false;
    bool m_clearTexture = true;
    bool m_renderScheduled = false;
    QSize m_cachedPixelSize;
    int m_cachedPage = -1;
    qreal m_cachedZoom = 0.0;
    qreal m_cachedDevicePixelRatio = 0.0;
    int m_cachedRevision = -1;
    PDFDocument *m_cachedDocument = nullptr;
    bool m_renderPending = false;
    QSize m_requestedPixelSize;
    int m_requestedPage = -1;
    qreal m_requestedZoom = 0.0;
    qreal m_requestedDevicePixelRatio = 0.0;
    int m_requestedRevision = -1;
    PDFDocument *m_requestedDocument = nullptr;
    std::shared_ptr<std::atomic_bool> m_renderCancellation;
    quint64 m_requestSerial = 0;
};
