#pragma once

#include "PDFDocument.h"
#include "PDFEditController.h"

#include <atomic>
#include <memory>

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QSGNode;

class PDFPageItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PDFPageView)
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(PDFEditController *editController READ editController WRITE setEditController NOTIFY editControllerChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(QColor pageColor READ pageColor WRITE setPageColor NOTIFY pageColorChanged FINAL)
    Q_PROPERTY(bool draftTextureCurrent READ draftTextureCurrent NOTIFY draftTextureCurrentChanged FINAL)
    Q_PROPERTY(bool draftTextureReady READ draftTextureReady NOTIFY draftTextureReadyChanged FINAL)

public:
    explicit PDFPageItem(QQuickItem *parent = nullptr);
    ~PDFPageItem() override;

    PDFDocument *document() const;
    PDFEditController *editController() const;
    int page() const;
    qreal zoom() const;
    QColor pageColor() const;
    bool draftTextureCurrent() const;
    bool draftTextureReady() const;

    void setDocument(PDFDocument *document);
    void setEditController(PDFEditController *editController);
    void setPage(int page);
    void setZoom(qreal zoom);
    void setPageColor(const QColor &pageColor);

signals:
    void documentChanged();
    void editControllerChanged();
    void pageChanged();
    void zoomChanged();
    void pageColorChanged();
    void draftTextureCurrentChanged();
    void draftTextureReadyChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    void refreshMetrics();
    void invalidateCache();
    void invalidateDraftCache(bool immediate);
    void cancelPendingRender();
    void handleDraftRenderStateChanged(int page, quint64 revision, bool immediate);
    quint64 currentDraftRevision() const;
    void setDraftTextureCurrent(bool current);
    void setDraftTextureReady(bool ready);
    void markTextureReady(quint64 requestId,
                          quint64 draftRevision,
                          bool suppressionApplied);
    void scheduleRender();
    void requestRenderForCurrentSize();
    void requestRender(const QSize &pixelSize, qreal devicePixelRatio);
    void handleRenderResult(quint64 requestId,
                            PDFDocument *document,
                            PDFEditController *editController,
                            int revision,
                            int page,
                            qreal zoom,
                            qreal devicePixelRatio,
                            const QSize &pixelSize,
                            quint64 draftRevision,
                            bool suppressionApplied,
                            const QImage &image);

    QPointer<PDFDocument> m_document;
    QPointer<PDFEditController> m_editController;
    int m_page = 0;
    qreal m_zoom = 1.0;
    QColor m_pageColor = Qt::white;
    bool m_cacheDirty = true;
    QImage m_pendingImage;
    bool m_textureDirty = false;
    bool m_clearTexture = true;
    bool m_draftTextureCurrent = false;
    bool m_draftTextureReady = false;
    bool m_renderScheduled = false;
    QTimer m_draftRenderTimer;
    QSize m_cachedPixelSize;
    int m_cachedPage = -1;
    qreal m_cachedZoom = 0.0;
    qreal m_cachedDevicePixelRatio = 0.0;
    int m_cachedRevision = -1;
    PDFDocument *m_cachedDocument = nullptr;
    quint64 m_cachedDraftRevision = 0;
    PDFEditController *m_cachedEditController = nullptr;
    bool m_renderPending = false;
    QSize m_requestedPixelSize;
    int m_requestedPage = -1;
    qreal m_requestedZoom = 0.0;
    qreal m_requestedDevicePixelRatio = 0.0;
    int m_requestedRevision = -1;
    PDFDocument *m_requestedDocument = nullptr;
    quint64 m_requestedDraftRevision = 0;
    PDFEditController *m_requestedEditController = nullptr;
    quint64 m_pendingTextureRequestId = 0;
    quint64 m_pendingTextureDraftRevision = 0;
    bool m_pendingSuppressionApplied = false;
    std::shared_ptr<std::atomic_bool> m_renderCancellation;
    quint64 m_requestSerial = 0;
};
