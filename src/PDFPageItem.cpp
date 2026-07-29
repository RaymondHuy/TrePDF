#include "PDFPageItem.h"
#include "PDFFormWorker.h"
#include "PDFWidgetAnnotationSuppression.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQuickWindow>

#include <fpdf_edit.h>
#include <fpdf_formfill.h>

namespace {

using CancellationFlag = std::shared_ptr<std::atomic_bool>;

bool isCancelled(const CancellationFlag &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

FPDF_DWORD pdfiumColor(const QColor &color)
{
    return (static_cast<FPDF_DWORD>(color.alpha()) << 24)
        | (static_cast<FPDF_DWORD>(color.red()) << 16)
        | (static_cast<FPDF_DWORD>(color.green()) << 8)
        | static_cast<FPDF_DWORD>(color.blue());
}

FPDF_PAGEOBJECT pageObjectAtPath(FPDF_PAGE page, const QVector<int> &objectPath)
{
    assertPDFiumProcessLockHeld();
    if (!page || objectPath.isEmpty()) {
        return nullptr;
    }

    const int topLevelIndex = objectPath.first();
    const int topLevelCount = FPDFPage_CountObjects(page);
    if (topLevelIndex < 0 || topLevelIndex >= topLevelCount) {
        return nullptr;
    }

    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, topLevelIndex);
    for (qsizetype depth = 1; object && depth < objectPath.size(); ++depth) {
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_FORM) {
            return nullptr;
        }

        const int childIndex = objectPath.at(depth);
        const int childCount = FPDFFormObj_CountObjects(object);
        if (childIndex < 0 || childIndex >= childCount) {
            return nullptr;
        }
        object = FPDFFormObj_GetObject(object, static_cast<unsigned long>(childIndex));
    }

    return object;
}

class ScopedPageObjectSuppression
{
public:
    ~ScopedPageObjectSuppression()
    {
        restore();
    }

    bool suppress(FPDF_PAGE page, const QVector<QVector<int>> &objectPaths)
    {
        assertPDFiumProcessLockHeld();
        QVector<FPDF_PAGEOBJECT> objects;
        objects.reserve(objectPaths.size());
        for (const QVector<int> &objectPath : objectPaths) {
            FPDF_PAGEOBJECT object = pageObjectAtPath(page, objectPath);
            if (!object) {
                restore();
                return false;
            }
            if (std::find(objects.cbegin(), objects.cend(), object) == objects.cend()) {
                objects.append(object);
            }
        }

        m_states.reserve(objects.size());
        for (FPDF_PAGEOBJECT object : std::as_const(objects)) {
            FPDF_BOOL active = false;
            if (!FPDFPageObj_GetIsActive(object, &active)) {
                restore();
                return false;
            }
            m_states.append(State{object, active});
        }

        for (const State &state : std::as_const(m_states)) {
            if (state.wasActive && !FPDFPageObj_SetIsActive(state.object, false)) {
                restore();
                return false;
            }
        }
        return true;
    }

    bool restore()
    {
        assertPDFiumProcessLockHeld();
        if (m_restored) {
            return m_restoreSucceeded;
        }

        m_restored = true;
        for (auto state = m_states.crbegin(); state != m_states.crend(); ++state) {
            if (state->wasActive && !FPDFPageObj_SetIsActive(state->object, true)) {
                m_restoreSucceeded = false;
            }
        }
        m_states.clear();
        return m_restoreSucceeded;
    }

    bool restorationSucceeded() const
    {
        return m_restoreSucceeded;
    }

private:
    struct State {
        FPDF_PAGEOBJECT object = nullptr;
        FPDF_BOOL wasActive = false;
    };

    QVector<State> m_states;
    bool m_restored = false;
    bool m_restoreSucceeded = true;
};

class ScopedPageObjectMoves
{
public:
    ~ScopedPageObjectMoves()
    {
        restore();
    }

    bool apply(FPDF_PAGE page,
               const QSize &deviceSize,
               const QVector<PDFEditController::DraftObjectMove> &moves)
    {
        assertPDFiumProcessLockHeld();
        if (!page || deviceSize.isEmpty()) {
            return moves.isEmpty();
        }

        for (const PDFEditController::DraftObjectMove &move : moves) {
            if (move.objectPath.size() != 1) {
                restore();
                return false;
            }

            FPDF_PAGEOBJECT object = pageObjectAtPath(page, move.objectPath);
            const int expectedType
                = move.kind == PDFEditController::DraftObjectMove::Kind::Text
                ? FPDF_PAGEOBJ_TEXT
                : FPDF_PAGEOBJ_IMAGE;
            if (!object || FPDFPageObj_GetType(object) != expectedType) {
                restore();
                return false;
            }
            const auto duplicate = std::find_if(
                m_states.cbegin(),
                m_states.cend(),
                [object](const State &state) { return state.object == object; });
            if (duplicate != m_states.cend()) {
                restore();
                return false;
            }

            FS_MATRIX originalMatrix{};
            if (!FPDFPageObj_GetMatrix(object, &originalMatrix)) {
                restore();
                return false;
            }

            QPointF sourcePoint;
            QPointF targetPoint;
            if (!pagePoint(
                    page,
                    deviceSize,
                    move.sourceRect.topLeft(),
                    sourcePoint)
                || !pagePoint(
                    page,
                    deviceSize,
                    move.frameRect.topLeft(),
                    targetPoint)) {
                restore();
                return false;
            }

            FS_MATRIX movedMatrix = originalMatrix;
            movedMatrix.e += static_cast<float>(targetPoint.x() - sourcePoint.x());
            movedMatrix.f += static_cast<float>(targetPoint.y() - sourcePoint.y());
            m_states.append(State{object, originalMatrix});
            if (!FPDFPageObj_SetMatrix(object, &movedMatrix)) {
                restore();
                return false;
            }
        }
        return true;
    }

    bool restore()
    {
        assertPDFiumProcessLockHeld();
        if (m_restored) {
            return m_restoreSucceeded;
        }

        m_restored = true;
        for (auto state = m_states.crbegin(); state != m_states.crend(); ++state) {
            if (!FPDFPageObj_SetMatrix(state->object, &state->matrix)) {
                m_restoreSucceeded = false;
            }
        }
        m_states.clear();
        return m_restoreSucceeded;
    }

    bool restorationSucceeded() const
    {
        return m_restoreSucceeded;
    }

private:
    struct State {
        FPDF_PAGEOBJECT object = nullptr;
        FS_MATRIX matrix{};
    };

    static bool pagePoint(FPDF_PAGE page,
                          const QSize &deviceSize,
                          const QPointF &normalizedPoint,
                          QPointF &point)
    {
        assertPDFiumProcessLockHeld();
        double pageX = 0.0;
        double pageY = 0.0;
        if (!FPDF_DeviceToPage(
                page,
                0,
                0,
                deviceSize.width(),
                deviceSize.height(),
                0,
                qRound(normalizedPoint.x() * deviceSize.width()),
                qRound(normalizedPoint.y() * deviceSize.height()),
                &pageX,
                &pageY)) {
            return false;
        }
        point = QPointF(pageX, pageY);
        return true;
    }

    QVector<State> m_states;
    bool m_restored = false;
    bool m_restoreSucceeded = true;
};

struct PageRenderResult {
    QImage image;
    bool suppressionApplied = false;
};

PageRenderResult renderPageFromFile(
    const PDFFormWorker::DocumentKey &formDocument,
    int sourcePage,
    const QSize &pixelSize,
    const QColor &background,
    qreal devicePixelRatio,
    bool blankPage,
    const QVector<QVector<int>> &draftObjectPaths,
    const QVector<PDFEditController::DraftObjectMove> &draftObjectMoves,
    const QStringList &suppressedFormWidgetIds,
    const CancellationFlag &cancellation)
{
    if (!formDocument.isValid()
        || (!blankPage && sourcePage < 0)
        || pixelSize.isEmpty()
        || isCancelled(cancellation)) {
        return {};
    }

    QImage image(pixelSize, QImage::Format_RGB32);
    if (image.isNull() || isCancelled(cancellation)) {
        return {};
    }
    image.fill(background);

    if (blankPage) {
        image.setDevicePixelRatio(devicePixelRatio);
        return PageRenderResult{image, true};
    }

    bool suppressionApplied = draftObjectPaths.isEmpty();
    bool objectMovesApplied = draftObjectMoves.isEmpty();
    bool widgetSuppressionApplied = suppressedFormWidgetIds.isEmpty();
    bool renderPerformed = false;
    {
        // One process-wide transaction covers every PDFium call, including
        // restoring temporary draft transforms and destroying all page/bitmap
        // handles. Cancellation is checked again after waiting for the lock.
        PDFiumProcessLock pdfiumLock;
        if (isCancelled(cancellation)) {
            return {};
        }

        PDFFormWorker::withPageLocked(
            formDocument,
            sourcePage,
            [&](FPDF_DOCUMENT,
                FPDF_FORMHANDLE formHandle,
                FPDF_PAGE previewPage,
                FPDF_PAGE formPage) {
                if (!previewPage || !formPage || isCancelled(cancellation)) {
                    return true;
                }

                FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
                    pixelSize.width(),
                    pixelSize.height(),
                    FPDFBitmap_BGRA,
                    image.bits(),
                    image.bytesPerLine());
                if (!bitmap) {
                    return true;
                }
                FPDFBitmap_FillRect(
                    bitmap,
                    0,
                    0,
                    pixelSize.width(),
                    pixelSize.height(),
                    pdfiumColor(background));

                bool renderDocumentReusable = true;
                {
                    ScopedPageObjectSuppression suppression;
                    ScopedPageObjectMoves objectMoves;
                    PDFWidgetAnnotationSuppression widgetSuppression;
                    QVector<PDFWidgetRenderPatch> widgetPatches;
                    if (!draftObjectPaths.isEmpty()) {
                        suppressionApplied = suppression.suppress(
                            previewPage,
                            draftObjectPaths);
                    }
                    if (!draftObjectMoves.isEmpty()) {
                        objectMovesApplied = objectMoves.apply(
                            previewPage,
                            pixelSize,
                            draftObjectMoves);
                    }
                    if (!suppressedFormWidgetIds.isEmpty()) {
                        widgetSuppressionApplied
                            = widgetSuppression.suppress(
                                previewPage,
                                suppressedFormWidgetIds);
                    }

                    if (!isCancelled(cancellation)
                        && (suppressionApplied
                            || suppression.restorationSucceeded())
                        && (objectMovesApplied
                            || objectMoves.restorationSucceeded())
                        && (widgetSuppressionApplied
                            || widgetSuppression
                                .restorationSucceeded())) {
                        FPDF_RenderPageBitmap(
                            bitmap,
                            previewPage,
                            0,
                            0,
                            pixelSize.width(),
                            pixelSize.height(),
                            0,
                            FPDF_ANNOT);
                        if (widgetSuppressionApplied) {
                            widgetSuppressionApplied
                                = capturePDFWidgetRenderPatches(
                                    previewPage,
                                    pixelSize,
                                    widgetSuppression.pageRects(),
                                    image,
                                    widgetPatches);
                        }
                        const bool widgetRestoreSucceeded
                            = widgetSuppression.restore();
                        if (formHandle) {
                            FPDF_FFLDraw(
                                formHandle,
                                bitmap,
                                formPage,
                                0,
                                0,
                                pixelSize.width(),
                                pixelSize.height(),
                                0,
                                FPDF_ANNOT);
                        }
                        if (widgetSuppressionApplied) {
                            widgetSuppressionApplied
                                = restorePDFWidgetRenderPatches(
                                    image,
                                    widgetPatches);
                        }
                        renderDocumentReusable
                            = widgetRestoreSucceeded;
                        renderPerformed = true;
                    }
                    const bool objectRestoreSucceeded = objectMoves.restore();
                    const bool suppressionRestoreSucceeded
                        = suppression.restore();
                    const bool widgetRestoreSucceeded
                        = widgetSuppression.restore();
                    renderDocumentReusable = objectRestoreSucceeded
                        && suppressionRestoreSucceeded
                        && widgetRestoreSucceeded
                        && renderDocumentReusable;
                }

                FPDFBitmap_Destroy(bitmap);
                return renderDocumentReusable;
            });
    }

    if (!renderPerformed || isCancelled(cancellation)) {
        return {};
    }

    image.setDevicePixelRatio(devicePixelRatio);
    return PageRenderResult{
        image,
        suppressionApplied
            && objectMovesApplied
            && widgetSuppressionApplied};
}

} // namespace

PDFPageItem::PDFPageItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAntialiasing(false);
    m_draftRenderTimer.setInterval(250);
    m_draftRenderTimer.setSingleShot(true);
    connect(&m_draftRenderTimer, &QTimer::timeout, this, &PDFPageItem::scheduleRender);
}

PDFPageItem::~PDFPageItem()
{
    cancelPendingRender();
}

PDFDocument *PDFPageItem::document() const
{
    return m_document;
}

PDFEditController *PDFPageItem::editController() const
{
    return m_editController;
}

PDFFormController *PDFPageItem::formController() const
{
    return m_formController;
}

PDFFormDesignerController *PDFPageItem::formDesignerController() const
{
    return m_formDesignerController;
}

bool PDFPageItem::formDesignMode() const
{
    return m_formDesignMode;
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

bool PDFPageItem::draftTextureCurrent() const
{
    return m_draftTextureCurrent;
}

bool PDFPageItem::draftTextureReady() const
{
    return m_draftTextureReady;
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

void PDFPageItem::setEditController(PDFEditController *editController)
{
    if (m_editController == editController) {
        return;
    }

    if (m_editController) {
        disconnect(m_editController, nullptr, this, nullptr);
    }

    m_editController = editController;
    if (m_editController) {
        connect(m_editController,
                &PDFEditController::draftRenderStateChanged,
                this,
                &PDFPageItem::handleDraftRenderStateChanged);
        connect(m_editController,
                &PDFEditController::documentChanged,
                this,
                &PDFPageItem::invalidateCache);
        connect(m_editController, &QObject::destroyed, this, [this]() {
            m_editController = nullptr;
            invalidateCache();
            emit editControllerChanged();
        });
    }

    invalidateCache();
    emit editControllerChanged();
}

void PDFPageItem::setFormController(PDFFormController *formController)
{
    if (m_formController == formController) {
        return;
    }

    if (m_formController) {
        disconnect(m_formController, nullptr, this, nullptr);
    }

    m_formController = formController;
    if (m_formController) {
        connect(
            m_formController,
            &PDFFormController::formRenderStateChanged,
            this,
            &PDFPageItem::handleFormRenderStateChanged);
        connect(
            m_formController,
            &PDFFormController::documentChanged,
            this,
            &PDFPageItem::invalidateCache);
        connect(m_formController, &QObject::destroyed, this, [this]() {
            m_formController = nullptr;
            invalidateCache();
            emit formControllerChanged();
        });
    }

    invalidateCache();
    emit formControllerChanged();
}

void PDFPageItem::setFormDesignerController(
    PDFFormDesignerController *formDesignerController)
{
    if (m_formDesignerController == formDesignerController) {
        return;
    }

    if (m_formDesignerController) {
        disconnect(m_formDesignerController, nullptr, this, nullptr);
    }

    m_formDesignerController = formDesignerController;
    if (m_formDesignerController) {
        connect(
            m_formDesignerController,
            &PDFFormDesignerController::textFieldsChanged,
            this,
            [this](int page) {
                if (page < 0 || page == m_page) {
                    refreshSuppressedFormWidgetIds();
                }
            });
        connect(
            m_formDesignerController,
            &QObject::destroyed,
            this,
            [this]() {
                m_formDesignerController = nullptr;
                refreshSuppressedFormWidgetIds();
                emit formDesignerControllerChanged();
            });
    }

    refreshSuppressedFormWidgetIds();
    emit formDesignerControllerChanged();
}

void PDFPageItem::setFormDesignMode(bool formDesignMode)
{
    if (m_formDesignMode == formDesignMode) {
        return;
    }

    m_formDesignMode = formDesignMode;
    refreshSuppressedFormWidgetIds();
    emit formDesignModeChanged();
}

void PDFPageItem::setPage(int page)
{
    if (m_page == page) {
        return;
    }

    m_page = page;
    refreshSuppressedFormWidgetIds();
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
        invalidateDraftCache(true);
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
        m_pendingTextureRequestId = 0;
        m_pendingTextureDraftRevision = 0;
        m_pendingTextureFormRevision = 0;
        m_pendingSuppressionApplied = false;
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
        const quint64 textureRequestId = m_pendingTextureRequestId;
        const quint64 textureDraftRevision = m_pendingTextureDraftRevision;
        const quint64 textureFormRevision = m_pendingTextureFormRevision;
        const bool suppressionApplied = m_pendingSuppressionApplied;
        QQuickWindow *quickWindow = window();
        QSGTexture *texture = quickWindow
            ? quickWindow->createTextureFromImage(m_pendingImage, QQuickWindow::TextureIsOpaque)
            : nullptr;
        m_pendingImage = QImage();
        m_textureDirty = false;
        m_pendingTextureRequestId = 0;
        m_pendingTextureDraftRevision = 0;
        m_pendingTextureFormRevision = 0;
        m_pendingSuppressionApplied = false;

        if (!texture) {
            delete node;
            m_cacheDirty = true;
            return nullptr;
        }
        node->setTexture(texture);

        const QPointer<PDFPageItem> item(this);
        QMetaObject::invokeMethod(this, [item,
                                         textureRequestId,
                                         textureDraftRevision,
                                         textureFormRevision,
                                         suppressionApplied]() {
            if (item) {
                item->markTextureReady(
                    textureRequestId,
                    textureDraftRevision,
                    textureFormRevision,
                    suppressionApplied);
            }
        }, Qt::QueuedConnection);
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
    m_draftRenderTimer.stop();
    cancelPendingRender();
    ++m_requestSerial;
    m_pendingImage = QImage();
    m_textureDirty = false;
    m_clearTexture = true;
    m_pendingTextureRequestId = 0;
    m_pendingTextureDraftRevision = 0;
    m_pendingTextureFormRevision = 0;
    m_pendingSuppressionApplied = false;
    m_cachedPixelSize = QSize();
    m_cachedPage = -1;
    m_cachedZoom = 0.0;
    m_cachedDevicePixelRatio = 0.0;
    m_cachedRevision = -1;
    m_cachedDocument = nullptr;
    m_cachedDraftRevision = 0;
    m_cachedEditController = nullptr;
    m_cachedFormRevision = 0;
    m_cachedFormController = nullptr;
    m_renderPending = false;
    m_requestedPixelSize = QSize();
    m_requestedPage = -1;
    m_requestedZoom = 0.0;
    m_requestedDevicePixelRatio = 0.0;
    m_requestedRevision = -1;
    m_requestedDocument = nullptr;
    m_requestedDraftRevision = 0;
    m_requestedEditController = nullptr;
    m_requestedFormRevision = 0;
    m_requestedFormController = nullptr;
    m_cacheDirty = true;
    setDraftTextureCurrent(false);
    setDraftTextureReady(false);
    update();
    scheduleRender();
}

void PDFPageItem::invalidateDraftCache(bool immediate)
{
    cancelPendingRender();
    ++m_requestSerial;
    m_pendingImage = QImage();
    m_textureDirty = false;
    m_pendingTextureRequestId = 0;
    m_pendingTextureDraftRevision = 0;
    m_pendingTextureFormRevision = 0;
    m_pendingSuppressionApplied = false;
    m_renderPending = false;
    m_requestedPixelSize = QSize();
    m_requestedPage = -1;
    m_requestedZoom = 0.0;
    m_requestedDevicePixelRatio = 0.0;
    m_requestedRevision = -1;
    m_requestedDocument = nullptr;
    m_requestedDraftRevision = 0;
    m_requestedEditController = nullptr;
    m_requestedFormRevision = 0;
    m_requestedFormController = nullptr;
    m_cacheDirty = true;
    setDraftTextureCurrent(false);
    setDraftTextureReady(false);

    if (immediate) {
        m_draftRenderTimer.stop();
        scheduleRender();
    } else {
        m_draftRenderTimer.start();
    }
}

void PDFPageItem::cancelPendingRender()
{
    PDFFormWorker::cancelRender(this);
    if (m_renderCancellation) {
        m_renderCancellation->store(true, std::memory_order_relaxed);
        m_renderCancellation.reset();
    }
}

void PDFPageItem::handleDraftRenderStateChanged(int page,
                                                quint64 revision,
                                                bool immediate)
{
    if (page != m_page || revision != currentDraftRevision()) {
        return;
    }

    if (!immediate) {
        invalidateDraftCache(false);
        return;
    }

    m_draftRenderTimer.stop();
    const bool matchingRequest = m_renderPending
        && m_requestedDraftRevision == revision
        && m_requestedDocument == m_document.data()
        && m_requestedEditController == m_editController.data();
    const bool matchingPendingTexture = m_textureDirty
        && m_pendingTextureDraftRevision == revision;
    const bool matchingTexture = m_draftTextureCurrent
        && !m_cacheDirty
        && m_cachedDraftRevision == revision
        && m_cachedDocument == m_document.data()
        && m_cachedEditController == m_editController.data();
    if (matchingRequest || matchingPendingTexture || matchingTexture) {
        return;
    }

    invalidateDraftCache(true);
}

void PDFPageItem::handleFormRenderStateChanged(int page, quint64 revision)
{
    if ((page >= 0 && page != m_page)
        || revision != currentFormRevision()) {
        return;
    }

    const bool matchingRequest = m_renderPending
        && m_requestedFormRevision == revision
        && m_requestedDocument == m_document.data()
        && m_requestedFormController == m_formController.data();
    const bool matchingPendingTexture = m_textureDirty
        && m_pendingTextureFormRevision == revision;
    const bool matchingTexture = !m_cacheDirty
        && m_cachedFormRevision == revision
        && m_cachedDocument == m_document.data()
        && m_cachedFormController == m_formController.data();
    if (matchingRequest || matchingPendingTexture || matchingTexture) {
        return;
    }

    invalidateDraftCache(true);
}

void PDFPageItem::refreshSuppressedFormWidgetIds()
{
    QStringList widgetIds;
    if (m_formDesignMode && m_formDesignerController && m_page >= 0) {
        widgetIds = m_formDesignerController
                        ->sourceTextFieldIdsForPage(m_page);
        std::sort(widgetIds.begin(), widgetIds.end());
        widgetIds.removeDuplicates();
    }

    if (m_suppressedFormWidgetIds == widgetIds) {
        return;
    }
    m_suppressedFormWidgetIds = std::move(widgetIds);
    invalidateCache();
}

quint64 PDFPageItem::currentDraftRevision() const
{
    if (!m_editController || m_editController->document() != m_document.data()) {
        return 0;
    }
    return m_editController->draftRevisionForPage(m_page);
}

quint64 PDFPageItem::currentFormRevision() const
{
    if (!m_formController
        || m_formController->document() != m_document.data()) {
        return 0;
    }
    return m_formController->formRevisionForPage(m_page);
}

void PDFPageItem::setDraftTextureCurrent(bool current)
{
    if (m_draftTextureCurrent == current) {
        return;
    }
    m_draftTextureCurrent = current;
    emit draftTextureCurrentChanged();
}

void PDFPageItem::setDraftTextureReady(bool ready)
{
    if (m_draftTextureReady == ready) {
        return;
    }
    m_draftTextureReady = ready;
    emit draftTextureReadyChanged();
}

void PDFPageItem::markTextureReady(quint64 requestId,
                                   quint64 draftRevision,
                                   quint64 formRevision,
                                   bool suppressionApplied)
{
    QQuickWindow *quickWindow = window();
    const qreal devicePixelRatio = quickWindow ? quickWindow->devicePixelRatio() : 0.0;
    const QSize pixelSize(
        qMax(1, qRound(width() * devicePixelRatio)),
        qMax(1, qRound(height() * devicePixelRatio)));
    if (requestId != m_requestSerial
        || !quickWindow
        || !m_document
        || m_document->renderRevision() != m_cachedRevision
        || m_cachedPage != m_page
        || !qFuzzyCompare(m_cachedZoom, m_zoom)
        || !qFuzzyCompare(m_cachedDevicePixelRatio, devicePixelRatio)
        || m_cachedPixelSize != pixelSize
        || draftRevision != currentDraftRevision()
        || formRevision != currentFormRevision()
        || m_cachedDraftRevision != draftRevision
        || m_cachedFormRevision != formRevision
        || m_cachedDocument != m_document.data()
        || m_cachedEditController != m_editController.data()
        || m_cachedFormController != m_formController.data()) {
        return;
    }

    setDraftTextureCurrent(true);
    setDraftTextureReady(suppressionApplied);
}

void PDFPageItem::scheduleRender()
{
    if (!isComponentComplete() || m_renderScheduled) {
        return;
    }

    m_renderScheduled = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_renderScheduled = false;
        if (m_draftRenderTimer.isActive()) {
            return;
        }
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
    const quint64 draftRevision = currentDraftRevision();
    const quint64 formRevision = currentFormRevision();
    const bool cacheMatches = !m_cacheDirty
        && m_cachedPixelSize == pixelSize
        && m_cachedPage == m_page
        && qFuzzyCompare(m_cachedZoom, m_zoom)
        && qFuzzyCompare(m_cachedDevicePixelRatio, devicePixelRatio)
        && m_cachedRevision == revision
        && m_cachedDocument == m_document.data()
        && m_cachedDraftRevision == draftRevision
        && m_cachedEditController == m_editController.data()
        && m_cachedFormRevision == formRevision
        && m_cachedFormController == m_formController.data();

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
    PDFEditController *editController = m_editController.data();
    PDFFormController *formController = m_formController.data();
    const quint64 draftRevision = currentDraftRevision();
    const quint64 formRevision = currentFormRevision();
    if (m_renderPending
        && m_requestedPixelSize == pixelSize
        && m_requestedPage == m_page
        && qFuzzyCompare(m_requestedZoom, m_zoom)
        && qFuzzyCompare(m_requestedDevicePixelRatio, devicePixelRatio)
        && m_requestedRevision == revision
        && m_requestedDocument == m_document.data()
        && m_requestedDraftRevision == draftRevision
        && m_requestedEditController == editController
        && m_requestedFormRevision == formRevision
        && m_requestedFormController == formController) {
        return;
    }

    cancelPendingRender();

    const quint64 requestId = ++m_requestSerial;
    PDFDocument *document = m_document.data();
    const int page = m_page;
    const int sourcePage = document->sourcePageIndex(page);
    const qreal zoom = m_zoom;
    const QColor background = m_pageColor;
    const bool blankPage = document->isTransientBlankPage(page);
    PDFFormWorker::DocumentKey formDocument;
    if (formController && formController->document() == document) {
        formDocument = formController->documentKey();
    } else {
        formDocument.filePath = filePath;
        formDocument.revision = revision;
    }
    const QVector<QVector<int>> draftObjectPaths = editController
            && editController->document() == document
        ? editController->draftedTextObjectPathsForPage(page)
        : QVector<QVector<int>>();
    const QVector<PDFEditController::DraftObjectMove> draftObjectMoves = editController
            && editController->document() == document
        ? editController->draftedObjectMovesForPage(page)
        : QVector<PDFEditController::DraftObjectMove>();
    const bool allDraftObjectsSuppressible = !editController
        || editController->document() != document
        || editController->allDraftedTextObjectsSuppressibleForPage(page);
    const QStringList suppressedFormWidgetIds
        = m_suppressedFormWidgetIds;
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
    m_requestedDraftRevision = draftRevision;
    m_requestedEditController = editController;
    m_requestedFormRevision = formRevision;
    m_requestedFormController = formController;

    PDFFormWorker::submitRender(this, formDocument.sessionId, [item,
                                       formDocument,
                                       document,
                                       editController,
                                       formController,
                                       revision,
                                       page,
                                       sourcePage,
                                       zoom,
                                       devicePixelRatio,
                                       pixelSize,
                                       background,
                                       blankPage,
                                       draftRevision,
                                       formRevision,
                                       draftObjectPaths,
                                       draftObjectMoves,
                                       suppressedFormWidgetIds,
                                       allDraftObjectsSuppressible,
                                       requestId,
                                       cancellation]() {
        PageRenderResult result = renderPageFromFile(
            formDocument,
            sourcePage,
            pixelSize,
            background,
            devicePixelRatio,
            blankPage,
            draftObjectPaths,
            draftObjectMoves,
            suppressedFormWidgetIds,
            cancellation);
        result.suppressionApplied = result.suppressionApplied
            && allDraftObjectsSuppressible;
        if (isCancelled(cancellation)) {
            return;
        }

        QObject *app = QCoreApplication::instance();
        if (!app) {
            return;
        }

        QMetaObject::invokeMethod(app, [item,
                                        document,
                                        editController,
                                        formController,
                                        revision,
                                        page,
                                        zoom,
                                        devicePixelRatio,
                                        pixelSize,
                                        draftRevision,
                                        formRevision,
                                        result = std::move(result),
                                        requestId,
                                        cancellation]() {
            if (!item || isCancelled(cancellation)) {
                return;
            }

            item->handleRenderResult(
                requestId,
                document,
                editController,
                formController,
                revision,
                page,
                zoom,
                devicePixelRatio,
                pixelSize,
                draftRevision,
                formRevision,
                result.suppressionApplied,
                result.image);
        }, Qt::QueuedConnection);
    });
}

void PDFPageItem::handleRenderResult(quint64 requestId,
                                     PDFDocument *document,
                                     PDFEditController *editController,
                                     PDFFormController *formController,
                                     int revision,
                                     int page,
                                     qreal zoom,
                                     qreal devicePixelRatio,
                                     const QSize &pixelSize,
                                     quint64 draftRevision,
                                     quint64 formRevision,
                                     bool suppressionApplied,
                                     const QImage &image)
{
    QQuickWindow *quickWindow = window();
    const qreal currentDevicePixelRatio = quickWindow
        ? quickWindow->devicePixelRatio()
        : 0.0;
    const QSize currentPixelSize(
        qMax(1, qRound(width() * currentDevicePixelRatio)),
        qMax(1, qRound(height() * currentDevicePixelRatio)));
    if (requestId != m_requestSerial
        || !quickWindow
        || m_document.data() != document
        || !m_document
        || m_document->renderRevision() != revision
        || m_editController.data() != editController
        || m_formController.data() != formController
        || page != m_page
        || !qFuzzyCompare(zoom, m_zoom)
        || !qFuzzyCompare(devicePixelRatio, currentDevicePixelRatio)
        || pixelSize != m_requestedPixelSize
        || pixelSize != currentPixelSize
        || draftRevision != m_requestedDraftRevision
        || draftRevision != currentDraftRevision()
        || formRevision != m_requestedFormRevision
        || formRevision != currentFormRevision()) {
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
    m_cachedDraftRevision = draftRevision;
    m_cachedEditController = editController;
    m_cachedFormRevision = formRevision;
    m_cachedFormController = formController;
    m_cacheDirty = false;
    m_textureDirty = true;
    m_clearTexture = false;
    m_pendingTextureRequestId = requestId;
    m_pendingTextureDraftRevision = draftRevision;
    m_pendingTextureFormRevision = formRevision;
    m_pendingSuppressionApplied = suppressionApplied;

    update();
}
