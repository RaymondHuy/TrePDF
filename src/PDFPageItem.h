#pragma once

#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFRasterBudget.h"
#include "PDFRasterPlan.h"
#include "PDFMemoryTelemetry.h"

#include <atomic>
#include <memory>
#include <functional>

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QSGNode;

class PDFPageItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PDFPageView)
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(PDFEditController *editController READ editController WRITE setEditController NOTIFY editControllerChanged FINAL)
    Q_PROPERTY(PDFFormController *formController READ formController WRITE setFormController NOTIFY formControllerChanged FINAL)
    Q_PROPERTY(PDFFormDesignerController *formDesignerController
                   READ formDesignerController
                   WRITE setFormDesignerController
                   NOTIFY formDesignerControllerChanged FINAL)
    Q_PROPERTY(bool formDesignMode READ formDesignMode WRITE setFormDesignMode
                   NOTIFY formDesignModeChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(QRectF visiblePageRect READ visiblePageRect WRITE setVisiblePageRect
                   NOTIFY visiblePageRectChanged FINAL)
    Q_PROPERTY(QColor pageColor READ pageColor WRITE setPageColor NOTIFY pageColorChanged FINAL)
    Q_PROPERTY(bool draftTextureCurrent READ draftTextureCurrent NOTIFY draftTextureCurrentChanged FINAL)
    Q_PROPERTY(bool draftTextureReady READ draftTextureReady NOTIFY draftTextureReadyChanged FINAL)
    Q_PROPERTY(QStringList displayedSuppressedCommentObjectIds
                   READ displayedSuppressedCommentObjectIds
                   NOTIFY displayedSuppressedCommentObjectIdsChanged FINAL)

public:
    explicit PDFPageItem(QQuickItem *parent = nullptr);
    ~PDFPageItem() override;

    PDFDocument *document() const;
    PDFEditController *editController() const;
    PDFFormController *formController() const;
    PDFFormDesignerController *formDesignerController() const;
    bool formDesignMode() const;
    int page() const;
    qreal zoom() const;
    QRectF visiblePageRect() const;
    QColor pageColor() const;
    bool draftTextureCurrent() const;
    bool draftTextureReady() const;
    QStringList displayedSuppressedCommentObjectIds() const;

    void setDocument(PDFDocument *document);
    void setEditController(PDFEditController *editController);
    void setFormController(PDFFormController *formController);
    void setFormDesignerController(
        PDFFormDesignerController *formDesignerController);
    void setFormDesignMode(bool formDesignMode);
    void setPage(int page);
    void setZoom(qreal zoom);
    void setVisiblePageRect(const QRectF &rect);
    void setPageColor(const QColor &pageColor);

signals:
    void documentChanged();
    void editControllerChanged();
    void formControllerChanged();
    void formDesignerControllerChanged();
    void formDesignModeChanged();
    void pageChanged();
    void zoomChanged();
    void visiblePageRectChanged();
    void pageColorChanged();
    void draftTextureCurrentChanged();
    void draftTextureReadyChanged();
    void displayedSuppressedCommentObjectIdsChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    friend class PDFMemoryTelemetry;
    // Tests observe request generations without adding a QML diagnostics API.
    friend class PDFEditControllerTest;

    struct RasterJob {
        QSize rasterSize;
        QRect rect;
        QRect renderRect;
        bool base = false;
    };
    struct RasterEntry {
        quint64 id = 0;
        RasterJob job;
        QImage image; // Released at scene-graph upload; never a CPU raster cache.
        PDFRasterBudget::Lease reservation;
        quint64 lastUsed = 0;
#if TREPDF_ENABLE_MEMORY_TELEMETRY
        PDFMemoryTelemetry::PendingImage pendingImage;
#endif
    };

    void refreshMetrics();
    void updateImplicitPageSize();
    void invalidateCache();
    void invalidateDraftCache(bool immediate);
    void invalidateRasterCache();
    void cancelPendingRender();
    void handleDraftRenderStateChanged(int page, quint64 revision, bool immediate);
    void handleFormRenderStateChanged(int page, quint64 revision);
    bool refreshSuppressedFormWidgetIds();
    quint64 currentDraftRevision() const;
    quint64 currentFormRevision() const;
    bool cachedRasterMatchesCurrentState(const QSize &pixelSize,
                                         qreal devicePixelRatio) const;
    bool pendingRenderMatchesCurrentState(const QSize &pixelSize,
                                          qreal devicePixelRatio) const;
    void queueTextureLossRecovery();
    void setDraftTextureCurrent(bool current);
    void setDraftTextureReady(bool ready);
    void setDisplayedSuppressedCommentObjectIds(QStringList objectIds);
    void markTextureReady(quint64 requestId,
                          quint64 draftRevision,
                          quint64 formRevision,
                          bool suppressionApplied);
    void scheduleRender();
    void requestRenderForCurrentSize();
    void requestRender(const QSize &pixelSize, qreal devicePixelRatio);
    void dispatchNextRaster();
    QRectF normalizedVisibleRect() const;
    bool hasRasterTile(const QRect &rect) const;
    void trimRasterCache();
    static void evictUnusedRasters(qint64 bytesNeeded);
    static void retryBudgetBlockedRenders();
    void handleRenderResult(quint64 requestId,
                            PDFDocument *document,
                            PDFEditController *editController,
                            PDFFormController *formController,
                            int revision,
                            int page,
                            qreal devicePixelRatio,
                            const QSize &pixelSize,
                            quint64 draftRevision,
                            quint64 formRevision,
                            bool suppressionApplied,
                            const QStringList &suppressedCommentObjectIds,
                            const RasterJob &job,
                            const QImage &image,
                            PDFRasterBudget::Lease reservation,
                            bool budgetDenied
#if TREPDF_ENABLE_MEMORY_TELEMETRY
                            , const PDFMemoryTelemetry::PendingImage &pendingImage
#endif
                            );

    QPointer<PDFDocument> m_document;
    QPointer<PDFEditController> m_editController;
    QPointer<PDFFormController> m_formController;
    QPointer<PDFFormDesignerController> m_formDesignerController;
    bool m_formDesignMode = false;
    QStringList m_suppressedFormWidgetIds;
    int m_page = 0;
    qreal m_zoom = 1.0;
    QRectF m_visiblePageRect;
    bool m_hasExplicitVisibleRect = false;
    QColor m_pageColor = Qt::white;
    bool m_cacheDirty = true;
    QVector<RasterEntry> m_rasters;
    QVector<RasterJob> m_renderJobs;
    std::function<void(const RasterJob &)> m_renderWork;
    quint64 m_nextRasterId = 0;
    bool m_cachedTiled = false;
    bool m_requestedTiled = false;
    bool m_forceTiledForBudget = false;
    bool m_budgetBlocked = false;
    qint64 m_budgetRetryBytes = 0;
    bool m_cachedSuppressionApplied = false;
    bool m_textureDirty = false;
    bool m_clearTexture = true;
    bool m_draftTextureCurrent = false;
    bool m_draftTextureReady = false;
    QStringList m_displayedSuppressedCommentObjectIds;
    bool m_renderScheduled = false;
    QTimer m_draftRenderTimer;
    QTimer m_zoomRenderTimer;
    quint64 m_cachedRequestId = 0;
    QSize m_cachedPixelSize;
    int m_cachedPage = -1;
    qreal m_cachedDevicePixelRatio = 0.0;
    int m_cachedRevision = -1;
    PDFDocument *m_cachedDocument = nullptr;
    quint64 m_cachedDraftRevision = 0;
    PDFEditController *m_cachedEditController = nullptr;
    quint64 m_cachedFormRevision = 0;
    PDFFormController *m_cachedFormController = nullptr;
    bool m_renderPending = false;
    QSize m_requestedPixelSize;
    int m_requestedPage = -1;
    qreal m_requestedDevicePixelRatio = 0.0;
    int m_requestedRevision = -1;
    PDFDocument *m_requestedDocument = nullptr;
    quint64 m_requestedDraftRevision = 0;
    PDFEditController *m_requestedEditController = nullptr;
    quint64 m_requestedFormRevision = 0;
    PDFFormController *m_requestedFormController = nullptr;
    quint64 m_pendingTextureRequestId = 0;
    quint64 m_pendingTextureDraftRevision = 0;
    quint64 m_pendingTextureFormRevision = 0;
    bool m_pendingSuppressionApplied = false;
    std::shared_ptr<std::atomic_bool> m_renderCancellation;
    quint64 m_requestSerial = 0;
};
