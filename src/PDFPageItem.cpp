#include "PDFPageItem.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormWorker.h"
#include "PDFWidgetAnnotationSuppression.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQuickWindow>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>

namespace {

constexpr auto kZoomRenderSettleInterval = std::chrono::milliseconds(140);
constexpr qint64 kRetainedTileBytes = 32 * 1024 * 1024;
// Leave capacity for a replacement preview even when all detail is visible.
constexpr qint64 kReplacementHeadroomBytes = 16 * 1024 * 1024;

// GUI-thread metadata only. Reservations themselves are thread-safe and remain
// charged until both pending images and their scene-graph textures are gone.
QSet<PDFPageItem *> &rasterItems()
{
    static QSet<PDFPageItem *> items;
    return items;
}

quint64 nextRasterUse()
{
    static quint64 sequence = 0;
    return ++sequence;
}

// Identity transform nodes retain inherited item transforms during incremental
// child updates on both the software and RHI scene-graph backends.
class RasterTextureNode final : public QSGTransformNode
{
public:
    RasterTextureNode(quint64 rasterId,
                      QSGTexture *texture,
                      PDFRasterBudget::Lease lease)
        : id(rasterId), reservation(std::move(lease))
    {
        textureNode = new QSGSimpleTextureNode;
        textureNode->setOwnsTexture(true);
        textureNode->setFiltering(QSGTexture::Linear);
        textureNode->setTexture(texture);
        appendChildNode(textureNode);
    }

    ~RasterTextureNode() override
    {
        // Destroy the owned texture before the reservation member is released.
        removeChildNode(textureNode);
        delete textureNode;
    }

    quint64 id;
    QSGSimpleTextureNode *textureNode = nullptr;
    PDFRasterBudget::Lease reservation;
};

using CancellationFlag = std::shared_ptr<std::atomic_bool>;
using PDFAnnotationUtils::annotationUsesReplyRelation;
using PDFAnnotationUtils::isMarkupAnnotation;

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

int popupAnnotationIndexForComment(FPDF_PAGE page,
                                   FPDF_ANNOTATION comment,
                                   int commentIndex,
                                   int annotationCount,
                                   bool &valid)
{
    assertPDFiumProcessLockHeld();
    valid = false;

    int forwardPopupIndex = -1;
    FPDF_ANNOTATION forwardPopup = FPDFAnnot_GetLinkedAnnot(
        comment,
        "Popup");
    if (forwardPopup) {
        forwardPopupIndex = FPDFPage_GetAnnotIndex(page, forwardPopup);
        const bool forwardPopupValid
            = FPDFAnnot_GetSubtype(forwardPopup) == FPDF_ANNOT_POPUP
            && forwardPopupIndex >= 0
            && forwardPopupIndex < annotationCount;
        FPDFPage_CloseAnnot(forwardPopup);
        if (!forwardPopupValid) {
            return -1;
        }
    }

    int reversePopupIndex = -1;
    for (int annotationIndex = 0;
         annotationIndex < annotationCount;
         ++annotationIndex) {
        FPDF_ANNOTATION popup = FPDFPage_GetAnnot(page, annotationIndex);
        if (!popup) {
            continue;
        }
        if (FPDFAnnot_GetSubtype(popup) != FPDF_ANNOT_POPUP) {
            FPDFPage_CloseAnnot(popup);
            continue;
        }

        FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(popup, "Parent");
        const int parentIndex = parent
            ? FPDFPage_GetAnnotIndex(page, parent)
            : -1;
        if (parent) {
            FPDFPage_CloseAnnot(parent);
        }
        FPDFPage_CloseAnnot(popup);
        if (parentIndex != commentIndex) {
            continue;
        }
        if (reversePopupIndex >= 0) {
            return -1;
        }
        reversePopupIndex = annotationIndex;
    }

    if (forwardPopupIndex >= 0
        && reversePopupIndex != forwardPopupIndex) {
        return -1;
    }
    valid = true;
    return forwardPopupIndex >= 0
        ? forwardPopupIndex
        : reversePopupIndex;
}

bool hiddenCommentAnnotationIndices(
    FPDF_PAGE page,
    const QVector<PDFEditController::DraftAnnotationDeletion> &deletions,
    QVector<int> &annotationIndices)
{
    assertPDFiumProcessLockHeld();
    annotationIndices.clear();
    if (!page) {
        return deletions.isEmpty();
    }

    const int annotationCount = FPDFPage_GetAnnotCount(page);
    QSet<int> commentIndices;
    for (const PDFEditController::DraftAnnotationDeletion &deletion
         : deletions) {
        if (deletion.annotationIndex < 0
            || deletion.annotationIndex >= annotationCount
            || commentIndices.contains(deletion.annotationIndex)) {
            annotationIndices.clear();
            return false;
        }

        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            page,
            deletion.annotationIndex);
        if (!annotation
            || FPDFAnnot_GetSubtype(annotation)
                != deletion.annotationSubtype) {
            if (annotation) {
                FPDFPage_CloseAnnot(annotation);
            }
            annotationIndices.clear();
            return false;
        }
        commentIndices.insert(deletion.annotationIndex);
        FPDFPage_CloseAnnot(annotation);
    }

    const QSet<int> deletedRoots = commentIndices;
    for (int annotationIndex = 0;
         annotationIndex < annotationCount;
         ++annotationIndex) {
        if (commentIndices.contains(annotationIndex)) {
            continue;
        }
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, annotationIndex);
        if (!annotation) {
            continue;
        }
        const bool replyRelation = annotationUsesReplyRelation(annotation);
        bool markupReplyChain = isMarkupAnnotation(annotation);
        FPDF_ANNOTATION parent = replyRelation
            ? FPDFAnnot_GetLinkedAnnot(annotation, "IRT")
            : nullptr;
        if (parent && markupReplyChain) {
            // Replies stay available to the comments panel and save pipeline,
            // but their standalone annotation appearances are not part of the
            // page viewer.
            commentIndices.insert(annotationIndex);
        }
        FPDFPage_CloseAnnot(annotation);
        if (!replyRelation) {
            continue;
        }

        QSet<int> visited{annotationIndex};
        while (parent) {
            const int parentIndex = FPDFPage_GetAnnotIndex(page, parent);
            if (parentIndex < 0 || visited.contains(parentIndex)) {
                FPDFPage_CloseAnnot(parent);
                break;
            }
            visited.insert(parentIndex);
            if (deletedRoots.contains(parentIndex)) {
                if (!markupReplyChain) {
                    FPDFPage_CloseAnnot(parent);
                    annotationIndices.clear();
                    return false;
                }
                commentIndices.insert(annotationIndex);
                FPDFPage_CloseAnnot(parent);
                break;
            }
            if (!annotationUsesReplyRelation(parent)) {
                FPDFPage_CloseAnnot(parent);
                break;
            }
            markupReplyChain = markupReplyChain
                && isMarkupAnnotation(parent);
            FPDF_ANNOTATION nextParent = FPDFAnnot_GetLinkedAnnot(
                parent,
                "IRT");
            FPDFPage_CloseAnnot(parent);
            parent = nextParent;
        }
    }

    QSet<int> hiddenIndices = commentIndices;
    for (int commentIndex : std::as_const(commentIndices)) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, commentIndex);
        if (!annotation) {
            annotationIndices.clear();
            return false;
        }
        bool popupValid = false;
        const int popupIndex = popupAnnotationIndexForComment(
            page,
            annotation,
            commentIndex,
            annotationCount,
            popupValid);
        FPDFPage_CloseAnnot(annotation);
        if (!popupValid) {
            annotationIndices.clear();
            return false;
        }
        if (popupIndex >= 0) {
            hiddenIndices.insert(popupIndex);
        }
    }
    annotationIndices = hiddenIndices.values();
    std::sort(annotationIndices.begin(), annotationIndices.end());
    return true;
}

class ScopedAnnotationSuppression
{
public:
    ~ScopedAnnotationSuppression()
    {
        restore();
    }

    bool suppress(
        FPDF_PAGE page,
        const QVector<PDFEditController::DraftAnnotationDeletion> &deletions)
    {
        assertPDFiumProcessLockHeld();
        m_page = page;
        QVector<int> annotationIndices;
        if (!hiddenCommentAnnotationIndices(
                page,
                deletions,
                annotationIndices)) {
            return false;
        }
        m_hasTargetAnnotations = !annotationIndices.isEmpty();
        m_states.reserve(annotationIndices.size());
        for (int annotationIndex : std::as_const(annotationIndices)) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                page,
                annotationIndex);
            if (!annotation) {
                restore();
                return false;
            }
            const int flags = FPDFAnnot_GetFlags(annotation);
            m_states.append(State{annotationIndex, flags});
            const bool hidden = flags & FPDF_ANNOT_FLAG_HIDDEN;
            const bool updated = hidden
                || FPDFAnnot_SetFlags(
                    annotation,
                    flags | FPDF_ANNOT_FLAG_HIDDEN);
            FPDFPage_CloseAnnot(annotation);
            if (!updated) {
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
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                m_page,
                state->annotationIndex);
            if (!annotation) {
                m_restoreSucceeded = false;
                continue;
            }
            if (!FPDFAnnot_SetFlags(annotation, state->flags)) {
                m_restoreSucceeded = false;
            }
            FPDFPage_CloseAnnot(annotation);
        }
        m_states.clear();
        return m_restoreSucceeded;
    }

    bool restorationSucceeded() const
    {
        return m_restoreSucceeded;
    }

    bool hasTargetAnnotations() const
    {
        return m_hasTargetAnnotations;
    }

private:
    struct State {
        int annotationIndex = -1;
        int flags = 0;
    };

    FPDF_PAGE m_page = nullptr;
    QVector<State> m_states;
    bool m_restored = false;
    bool m_restoreSucceeded = true;
    bool m_hasTargetAnnotations = false;
};

struct PreviewAnnotationRemovalResult {
    bool removedAll = false;
    bool documentReusable = true;
};

PreviewAnnotationRemovalResult removePreviewAnnotations(
    FPDF_PAGE page,
    const QVector<PDFEditController::DraftAnnotationDeletion> &deletions)
{
    assertPDFiumProcessLockHeld();
    if (!page) {
        return {deletions.isEmpty(), true};
    }

    QVector<int> annotationIndices;
    if (!hiddenCommentAnnotationIndices(page, deletions, annotationIndices)) {
        return {};
    }

    std::sort(
        annotationIndices.begin(),
        annotationIndices.end(),
        std::greater<int>());
    PreviewAnnotationRemovalResult result;
    for (int annotationIndex : std::as_const(annotationIndices)) {
        if (!FPDFPage_RemoveAnnot(page, annotationIndex)) {
            return result;
        }
        result.documentReusable = false;
    }
    result.removedAll = true;
    return result;
}

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
                : move.kind == PDFEditController::DraftObjectMove::Kind::Path
                    ? FPDF_PAGEOBJ_PATH
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
    const QRect &renderRect,
    const QColor &background,
    qreal devicePixelRatio,
    bool blankPage,
    const QVector<QVector<int>> &draftObjectPaths,
    const QVector<PDFEditController::DraftObjectMove> &draftObjectMoves,
    const QVector<PDFEditController::DraftAnnotationDeletion>
        &deletedCommentAnnotations,
    const QStringList &suppressedFormWidgetIds,
    const CancellationFlag &cancellation)
{
    if (!formDocument.isValid()
        || (!blankPage && sourcePage < 0)
        || pixelSize.isEmpty()
        || renderRect.isEmpty()
        || !QRect(QPoint(), pixelSize).contains(renderRect)
        || isCancelled(cancellation)) {
        return {};
    }

    QImage image(renderRect.size(), QImage::Format_RGB32);
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
    bool annotationSuppressionApplied = false;
    bool widgetSuppressionApplied = suppressedFormWidgetIds.isEmpty();
    bool renderPerformed = false;
    if (isCancelled(cancellation)) {
        return {};
    }
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
                    image.width(),
                    image.height(),
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
                    image.width(),
                    image.height(),
                    pdfiumColor(background));

                bool renderDocumentReusable = true;
                {
                    ScopedPageObjectSuppression suppression;
                    ScopedPageObjectMoves objectMoves;
                    ScopedAnnotationSuppression annotationSuppression;
                    PDFWidgetAnnotationSuppression widgetSuppression;
                    QVector<PDFWidgetRenderPatch> widgetPatches;
                    bool omitSourceAnnotations = false;
                    bool omitFormAnnotations = false;
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
                    annotationSuppressionApplied
                        = annotationSuppression.suppress(
                            previewPage,
                            deletedCommentAnnotations);
                    omitFormAnnotations
                        = annotationSuppression.hasTargetAnnotations();
                    if (!annotationSuppressionApplied
                        && annotationSuppression.restorationSucceeded()) {
                        const PreviewAnnotationRemovalResult removal
                            = removePreviewAnnotations(
                                previewPage,
                                deletedCommentAnnotations);
                        annotationSuppressionApplied = removal.removedAll;
                        renderDocumentReusable = removal.documentReusable;
                    }
                    if (!annotationSuppressionApplied) {
                        // A deleted annotation or reply must never leak back
                        // into the viewer. If both targeted strategies fail,
                        // omit the page annotation pass and let FPDF_FFLDraw
                        // render form widgets separately.
                        omitSourceAnnotations = true;
                        omitFormAnnotations = true;
                        annotationSuppressionApplied = true;
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
                        && (annotationSuppressionApplied
                            || annotationSuppression
                                .restorationSucceeded())
                        && (widgetSuppressionApplied
                            || widgetSuppression
                                .restorationSucceeded())) {
                        FPDF_RenderPageBitmap(
                            bitmap,
                            previewPage,
                            -renderRect.x(),
                            -renderRect.y(),
                            pixelSize.width(),
                            pixelSize.height(),
                            0,
                            omitSourceAnnotations ? 0 : FPDF_ANNOT);
                        if (widgetSuppressionApplied) {
                            widgetSuppressionApplied
                                = capturePDFWidgetRenderPatches(
                                    previewPage,
                                    pixelSize,
                                    widgetSuppression.pageRects(),
                                    image,
                                    widgetPatches,
                                    renderRect.topLeft());
                        }
                        const bool widgetRestoreSucceeded
                            = widgetSuppression.restore();
                        if (formHandle) {
                            FPDF_FFLDraw(
                                formHandle,
                                bitmap,
                                formPage,
                                -renderRect.x(),
                                -renderRect.y(),
                                pixelSize.width(),
                                pixelSize.height(),
                                0,
                                omitFormAnnotations ? 0 : FPDF_ANNOT);
                        }
                        if (widgetSuppressionApplied) {
                            widgetSuppressionApplied
                                = restorePDFWidgetRenderPatches(
                                    image,
                                    widgetPatches);
                        }
                        renderDocumentReusable
                            = widgetRestoreSucceeded
                            && renderDocumentReusable;
                        renderPerformed = true;
                    }
                    const bool objectRestoreSucceeded = objectMoves.restore();
                    const bool suppressionRestoreSucceeded
                        = suppression.restore();
                    const bool annotationRestoreSucceeded
                        = annotationSuppression.restore();
                    const bool widgetRestoreSucceeded
                        = widgetSuppression.restore();
                    renderDocumentReusable = objectRestoreSucceeded
                        && suppressionRestoreSucceeded
                        && annotationRestoreSucceeded
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
            && annotationSuppressionApplied
            && widgetSuppressionApplied};
}

} // namespace

PDFPageItem::PDFPageItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    rasterItems().insert(this);
    static const bool budgetCallbackInstalled = []() {
        PDFRasterBudget::setReleaseCallback([]() {
            if (QObject *app = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(app, []() {
                    PDFPageItem::retryBudgetBlockedRenders();
                }, Qt::QueuedConnection);
            }
        });
        return true;
    }();
    Q_UNUSED(budgetCallbackInstalled);
    setFlag(ItemHasContents, true);
    setAntialiasing(false);
    m_draftRenderTimer.setInterval(250);
    m_draftRenderTimer.setSingleShot(true);
    connect(&m_draftRenderTimer, &QTimer::timeout, this, &PDFPageItem::scheduleRender);
    m_zoomRenderTimer.setInterval(kZoomRenderSettleInterval);
    m_zoomRenderTimer.setSingleShot(true);
    connect(&m_zoomRenderTimer,
            &QTimer::timeout,
            this,
            &PDFPageItem::scheduleRender);
}

PDFPageItem::~PDFPageItem()
{
    rasterItems().remove(this);
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

QRectF PDFPageItem::visiblePageRect() const
{
    return m_hasExplicitVisibleRect ? m_visiblePageRect : boundingRect();
}

QRectF PDFPageItem::normalizedVisibleRect() const
{
    if (width() <= 0 || height() <= 0) {
        return {};
    }
    const QRectF rect = visiblePageRect().intersected(boundingRect());
    if (rect.isEmpty()) {
        return {};
    }
    return QRectF(rect.x() / width(), rect.y() / height(),
                  rect.width() / width(), rect.height() / height());
}

void PDFPageItem::setVisiblePageRect(const QRectF &rect)
{
    if (m_hasExplicitVisibleRect && m_visiblePageRect == rect) {
        return;
    }
    m_hasExplicitVisibleRect = true;
    m_visiblePageRect = rect;
    m_budgetBlocked = false;
    trimRasterCache();
    // Movement within the same tile coverage needs no PDFium work. Replan only
    // when a newly visible tile is neither resident nor in the current request.
    if (window()) {
        const auto pixelSize = PDFRasterPlan::rasterSize(
            size(), window()->devicePixelRatio());
        const auto plan = PDFRasterPlan::makePlan(
            pixelSize, normalizedVisibleRect(), m_cachedTiled, m_forceTiledForBudget);
        const bool residentCurrent = cachedRasterMatchesCurrentState(
            pixelSize, window()->devicePixelRatio()) && m_cachedTiled == plan.tiled;
        bool covered = pendingRenderMatchesCurrentState(
            pixelSize, window()->devicePixelRatio())
            || residentCurrent;
        for (const auto &tile : plan.tiles) {
            const bool queued = std::any_of(
                m_renderJobs.cbegin(), m_renderJobs.cend(),
                [&tile](const RasterJob &job) {
                    return !job.base && job.rect == tile.rect;
                });
            if (!(residentCurrent && hasRasterTile(tile.rect)) && !queued) {
                covered = false;
                break;
            }
        }
        if (normalizedVisibleRect().isEmpty() || !covered) {
            cancelPendingRender();
            ++m_requestSerial;
        }
    }
    emit visiblePageRectChanged();
    scheduleRender();
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

QStringList PDFPageItem::displayedSuppressedCommentObjectIds() const
{
    return m_displayedSuppressedCommentObjectIds;
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
                    const bool suppressionIdsChanged
                        = refreshSuppressedFormWidgetIds();
                    if (!suppressionIdsChanged
                        && m_formDesignMode
                        && !m_suppressedFormWidgetIds.isEmpty()) {
                        // A source-backed field move keeps the same widget ID,
                        // but the cached page may still contain its appearance
                        // at the source rectangle. Render the suppressed page
                        // again so only the designer overlay remains visible.
                        invalidateDraftCache(true);
                    }
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
    updateImplicitPageSize();
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
        invalidateRasterCache();
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
    QSGNode *node = oldNode;

    if (m_clearTexture || width() <= 0.0 || height() <= 0.0) {
        delete node;
        m_rasters.clear();
        m_textureDirty = false;
        m_cachedRequestId = 0;
        m_cacheDirty = true;
        m_pendingTextureRequestId = 0;
        m_pendingTextureDraftRevision = 0;
        m_pendingTextureFormRevision = 0;
        m_pendingSuppressionApplied = false;
        m_clearTexture = false;
        queueTextureLossRecovery();
        return nullptr;
    }

    if (!node && !m_textureDirty) {
        m_rasters.clear();
        m_cachedRequestId = 0;
        m_cacheDirty = true;
        queueTextureLossRecovery();
        return nullptr;
    }

    if (!node) {
        node = new QSGTransformNode;
    }

    // Synchronize desired IDs first. Only this thread deletes textures and thus
    // releases their budget leases; GUI eviction removes metadata, not textures.
    for (QSGNode *child = node->firstChild(); child;) {
        QSGNode *next = child->nextSibling();
        const auto *rasterNode = static_cast<RasterTextureNode *>(child);
        const bool retained = std::any_of(
            m_rasters.cbegin(), m_rasters.cend(),
            [rasterNode](const RasterEntry &entry) { return entry.id == rasterNode->id; });
        if (!retained) {
            node->removeChildNode(child);
            delete child;
        }
        child = next;
    }
    for (RasterEntry &entry : m_rasters) {
        RasterTextureNode *rasterNode = nullptr;
        for (QSGNode *child = node->firstChild(); child; child = child->nextSibling()) {
            auto *candidate = static_cast<RasterTextureNode *>(child);
            if (candidate->id == entry.id) {
                rasterNode = candidate;
                break;
            }
        }
        if (!rasterNode && !entry.image.isNull()) {
            QSGTexture *texture = window()->createTextureFromImage(
                entry.image, QQuickWindow::TextureIsOpaque);
            entry.image = QImage();
            if (texture) {
                rasterNode = new RasterTextureNode(entry.id, texture, entry.reservation);
                node->appendChildNode(rasterNode);
            }
        }
        if (!rasterNode) {
            delete node;
            m_rasters.clear();
            m_textureDirty = false;
            m_cachedRequestId = 0;
            m_cacheDirty = true;
            queueTextureLossRecovery();
            return nullptr;
        }
        const RasterJob &job = entry.job;
        const qreal sx = width() / job.rasterSize.width();
        const qreal sy = height() / job.rasterSize.height();
        rasterNode->textureNode->setRect(QRectF(
            job.rect.x() * sx, job.rect.y() * sy,
            job.rect.width() * sx, job.rect.height() * sy));
        // Crop the two-pixel gutter only at sampling time, avoiding seams when
        // the accepted tiles are GPU-scaled during a zoom gesture.
        rasterNode->textureNode->setSourceRect(QRectF(
            job.rect.translated(-job.renderRect.topLeft())));
    }

    if (m_textureDirty) {
        const quint64 textureRequestId = m_pendingTextureRequestId;
        const quint64 textureDraftRevision = m_pendingTextureDraftRevision;
        const quint64 textureFormRevision = m_pendingTextureFormRevision;
        const bool suppressionApplied = m_pendingSuppressionApplied;
        m_textureDirty = false;
        m_pendingTextureRequestId = 0;
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

    return node;
}

void PDFPageItem::refreshMetrics()
{
    updateImplicitPageSize();
    invalidateCache();
}

void PDFPageItem::updateImplicitPageSize()
{
    QSizeF pageSize;
    if (m_document && m_document->isLoaded()) {
        pageSize = m_document->pageSize(m_page);
    }

    setImplicitWidth(qMax<qreal>(0.0, pageSize.width() * m_zoom));
    setImplicitHeight(qMax<qreal>(0.0, pageSize.height() * m_zoom));
}

void PDFPageItem::invalidateCache()
{
    m_draftRenderTimer.stop();
    m_zoomRenderTimer.stop();
    cancelPendingRender();
    ++m_requestSerial;
    m_rasters.clear();
    m_cachedTiled = false;
    m_forceTiledForBudget = false;
    m_budgetBlocked = false;
    m_textureDirty = false;
    m_clearTexture = true;
    m_pendingTextureRequestId = 0;
    m_pendingTextureDraftRevision = 0;
    m_pendingTextureFormRevision = 0;
    m_pendingSuppressionApplied = false;
    setDisplayedSuppressedCommentObjectIds({});
    m_cachedRequestId = 0;
    m_cachedPixelSize = QSize();
    m_cachedPage = -1;
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
    m_budgetBlocked = false;
    m_budgetRetryBytes = 0;
    m_zoomRenderTimer.stop();
    cancelPendingRender();
    ++m_requestSerial;
    // Keep an already accepted image. Its suppression snapshot has already
    // been published to QML and will be uploaded together with the matching
    // overlay state on the next scene-graph sync. A newer render may replace
    // it before then, but dropping it here would leave QML describing a
    // texture that can no longer be displayed.
    m_renderPending = false;
    m_requestedPixelSize = QSize();
    m_requestedPage = -1;
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

void PDFPageItem::invalidateRasterCache()
{
    m_draftRenderTimer.stop();
    QQuickWindow *quickWindow = window();
    const qreal devicePixelRatio = quickWindow
        ? quickWindow->devicePixelRatio()
        : 0.0;
    const QSize pixelSize = PDFRasterPlan::rasterSize(size(), devicePixelRatio);
    m_budgetBlocked = false;
    if (!pixelSize.isEmpty()
        && pendingRenderMatchesCurrentState(pixelSize, devicePixelRatio)) {
        m_zoomRenderTimer.stop();
        update();
        return;
    }

    cancelPendingRender();
    ++m_requestSerial;

    // A visual resize does not change page contents. Keep the accepted texture
    // and its suppression snapshot on screen while the scene graph scales it,
    // then replace it with a sharp raster after zoom input settles.
    m_renderPending = false;
    m_requestedPixelSize = QSize();
    m_requestedPage = -1;
    m_requestedDevicePixelRatio = 0.0;
    m_requestedRevision = -1;
    m_requestedDocument = nullptr;
    m_requestedDraftRevision = 0;
    m_requestedEditController = nullptr;
    m_requestedFormRevision = 0;
    m_requestedFormController = nullptr;
    if (!pixelSize.isEmpty()
        && cachedRasterMatchesCurrentState(pixelSize, devicePixelRatio)) {
        m_zoomRenderTimer.stop();
        scheduleRender(); // Resume missing tiles if zoom returned to this raster.
    } else {
        m_zoomRenderTimer.start();
    }
    update();
}

void PDFPageItem::cancelPendingRender()
{
    PDFFormWorker::cancelRender(this);
    if (m_renderCancellation) {
        m_renderCancellation->store(true, std::memory_order_relaxed);
        m_renderCancellation.reset();
    }
    m_renderPending = false;
    m_renderJobs.clear();
    m_renderWork = {};
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

bool PDFPageItem::refreshSuppressedFormWidgetIds()
{
    QStringList widgetIds;
    if (m_formDesignMode && m_formDesignerController && m_page >= 0) {
        widgetIds = m_formDesignerController->sourceFieldIdsForPage(m_page);
        std::sort(widgetIds.begin(), widgetIds.end());
        widgetIds.removeDuplicates();
    }

    if (m_suppressedFormWidgetIds == widgetIds) {
        return false;
    }
    m_suppressedFormWidgetIds = std::move(widgetIds);
    invalidateCache();
    return true;
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

bool PDFPageItem::cachedRasterMatchesCurrentState(
    const QSize &pixelSize,
    qreal devicePixelRatio) const
{
    return !m_cacheDirty
        && m_cachedRequestId != 0
        && m_cachedPixelSize == pixelSize
        && m_cachedPage == m_page
        && qFuzzyCompare(m_cachedDevicePixelRatio, devicePixelRatio)
        && m_document
        && m_cachedRevision == m_document->renderRevision()
        && m_cachedDocument == m_document.data()
        && m_cachedDraftRevision == currentDraftRevision()
        && m_cachedEditController == m_editController.data()
        && m_cachedFormRevision == currentFormRevision()
        && m_cachedFormController == m_formController.data();
}

bool PDFPageItem::pendingRenderMatchesCurrentState(
    const QSize &pixelSize,
    qreal devicePixelRatio) const
{
    return m_renderPending
        && m_requestedPixelSize == pixelSize
        && m_requestedPage == m_page
        && qFuzzyCompare(m_requestedDevicePixelRatio, devicePixelRatio)
        && m_document
        && m_requestedRevision == m_document->renderRevision()
        && m_requestedDocument == m_document.data()
        && m_requestedDraftRevision == currentDraftRevision()
        && m_requestedEditController == m_editController.data()
        && m_requestedFormRevision == currentFormRevision()
        && m_requestedFormController == m_formController.data();
}

void PDFPageItem::queueTextureLossRecovery()
{
    const QPointer<PDFPageItem> item(this);
    QMetaObject::invokeMethod(this, [item]() {
        if (!item
            || item->m_cachedRequestId != 0
            || !item->m_cacheDirty) {
            return;
        }

        item->setDraftTextureCurrent(false);
        item->setDraftTextureReady(false);
        item->cancelPendingRender();
        ++item->m_requestSerial;
        item->m_budgetBlocked = false;
        item->scheduleRender();
    }, Qt::QueuedConnection);
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

void PDFPageItem::setDisplayedSuppressedCommentObjectIds(
    QStringList objectIds)
{
    if (m_displayedSuppressedCommentObjectIds == objectIds) {
        return;
    }
    m_displayedSuppressedCommentObjectIds = std::move(objectIds);
    emit displayedSuppressedCommentObjectIdsChanged();
}

void PDFPageItem::markTextureReady(quint64 requestId,
                                   quint64 draftRevision,
                                   quint64 formRevision,
                                   bool suppressionApplied)
{
    if (m_cacheDirty
        || requestId != m_cachedRequestId
        || !m_document
        || m_document->renderRevision() != m_cachedRevision
        || m_cachedPage != m_page
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
        if (m_draftRenderTimer.isActive()
            || m_zoomRenderTimer.isActive()) {
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
    const QSize pixelSize = PDFRasterPlan::rasterSize(size(), devicePixelRatio);
    if (pixelSize.isEmpty() || normalizedVisibleRect().isEmpty() || m_budgetBlocked) {
        return;
    }
    requestRender(pixelSize, devicePixelRatio);
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

    if (pendingRenderMatchesCurrentState(pixelSize, devicePixelRatio)) {
        return;
    }

    const auto plan = PDFRasterPlan::makePlan(
        pixelSize, normalizedVisibleRect(), m_cachedTiled, m_forceTiledForBudget);
    const bool reuse = cachedRasterMatchesCurrentState(pixelSize, devicePixelRatio)
        && m_cachedTiled == plan.tiled;
    QVector<RasterJob> jobs;
    if (!reuse) {
        const QSize baseSize = plan.tiled ? plan.previewSize : pixelSize;
        const QRect baseRect(QPoint(), baseSize);
        jobs.append({baseSize, baseRect, baseRect, true});
    }
    if (plan.tiled) {
        for (const auto &tile : plan.tiles) {
            if (!reuse || !hasRasterTile(tile.rect)) {
                jobs.append({pixelSize, tile.rect, tile.renderRect, false});
            }
        }
    }
    if (jobs.isEmpty()) {
        return;
    }
    cancelPendingRender();
    m_renderJobs = std::move(jobs);
    m_requestedTiled = plan.tiled;
    m_budgetBlocked = false;

    const int revision = m_document->renderRevision();
    PDFEditController *editController = m_editController.data();
    PDFFormController *formController = m_formController.data();
    const quint64 draftRevision = currentDraftRevision();
    const quint64 formRevision = currentFormRevision();
    const quint64 requestId = ++m_requestSerial;
    PDFDocument *document = m_document.data();
    const int page = m_page;
    const int sourcePage = document->sourcePageIndex(page);
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
    const QVector<PDFEditController::DraftAnnotationDeletion>
        deletedCommentAnnotations = editController
            && editController->document() == document
        ? editController->deletedSourceCommentAnnotationsForPage(page)
        : QVector<PDFEditController::DraftAnnotationDeletion>();
    QStringList suppressedCommentObjectIds;
    suppressedCommentObjectIds.reserve(deletedCommentAnnotations.size());
    for (const PDFEditController::DraftAnnotationDeletion &deletion
         : deletedCommentAnnotations) {
        suppressedCommentObjectIds.append(
            QStringLiteral("annotation:")
            + QString::number(deletion.annotationIndex));
    }
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
    m_requestedDevicePixelRatio = devicePixelRatio;
    m_requestedRevision = revision;
    m_requestedDocument = document;
    m_requestedDraftRevision = draftRevision;
    m_requestedEditController = editController;
    m_requestedFormRevision = formRevision;
    m_requestedFormController = formController;

    // A continuation is dispatched by GUI acknowledgement of each result.
    // The queue can run form commands and other pages between individual tiles.
    m_renderWork = [item,
                    formDocument,
                    document,
                    editController,
                    formController,
                    revision,
                    page,
                    sourcePage,
                    devicePixelRatio,
                    pixelSize,
                    background,
                    blankPage,
                    draftRevision,
                    formRevision,
                    draftObjectPaths,
                    draftObjectMoves,
                    deletedCommentAnnotations,
                    suppressedCommentObjectIds,
                    suppressedFormWidgetIds,
                    allDraftObjectsSuppressible,
                    requestId,
                    cancellation](const RasterJob &job) {
        PDFFormWorker::submitRender(item.data(), formDocument.sessionId,
            [=]() {
                if (isCancelled(cancellation)) {
                    return;
                }
                const qint64 imageBytes = PDFRasterPlan::imageBytes(job.renderRect.size());
                const qint64 headroom = !job.base || imageBytes > PDFRasterPlan::previewLimitBytes
                    ? kReplacementHeadroomBytes : 0;
                auto reservation = PDFRasterBudget::limitBytes - PDFRasterBudget::usedBytes()
                        >= 2 * imageBytes + headroom
                    ? PDFRasterBudget::tryReserve(imageBytes) : PDFRasterBudget::Lease();
                const bool budgetDenied = !reservation;
                PageRenderResult result;
                if (reservation && !isCancelled(cancellation)) {
                    result = renderPageFromFile(
                        formDocument,
                        sourcePage,
                        job.rasterSize,
                        job.renderRect,
                        background,
                        devicePixelRatio,
                        blankPage,
                        draftObjectPaths,
                        draftObjectMoves,
                        deletedCommentAnnotations,
                        suppressedFormWidgetIds,
                        cancellation);
                }
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
                                                devicePixelRatio,
                                                pixelSize,
                                                draftRevision,
                                                formRevision,
                                                job,
                                                result = std::move(result),
                                                reservation = std::move(reservation),
                                                budgetDenied,
                                                suppressedCommentObjectIds,
                                                requestId,
                                                cancellation]() mutable {
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
                        devicePixelRatio,
                        pixelSize,
                        draftRevision,
                        formRevision,
                        result.suppressionApplied,
                        suppressedCommentObjectIds,
                        job,
                        result.image,
                        std::move(reservation),
                        budgetDenied);
                }, Qt::QueuedConnection);
        });
    };
    dispatchNextRaster();
}

void PDFPageItem::dispatchNextRaster()
{
    if (m_renderJobs.isEmpty() || !m_renderWork) {
        m_renderPending = false;
        m_renderWork = {};
        m_renderCancellation.reset();
        return;
    }
    m_renderWork(m_renderJobs.constFirst());
}

bool PDFPageItem::hasRasterTile(const QRect &rect) const
{
    return std::any_of(m_rasters.cbegin(), m_rasters.cend(),
                       [&rect](const RasterEntry &entry) {
                           return !entry.job.base && entry.job.rect == rect;
                       });
}

void PDFPageItem::trimRasterCache()
{
    const QRectF visible = normalizedVisibleRect();
    qint64 unusedBytes = 0;
    QVector<quint64> unused;
    for (RasterEntry &entry : m_rasters) {
        const auto &job = entry.job;
        const QRectF normalized(job.rect.x() / qreal(job.rasterSize.width()),
                                job.rect.y() / qreal(job.rasterSize.height()),
                                job.rect.width() / qreal(job.rasterSize.width()),
                                job.rect.height() / qreal(job.rasterSize.height()));
        if (normalized.intersects(visible)) {
            entry.lastUsed = nextRasterUse();
        } else if (!job.base) {
            unusedBytes += entry.reservation->bytes();
            unused.append(entry.id);
        }
    }
    std::sort(unused.begin(), unused.end(), [this](quint64 a, quint64 b) {
        auto used = [this](quint64 id) {
            for (const auto &entry : m_rasters) {
                if (entry.id == id) {
                    return entry.lastUsed;
                }
            }
            return quint64{0};
        };
        return used(a) < used(b);
    });
    for (quint64 id : unused) {
        if (unusedBytes <= kRetainedTileBytes) {
            break;
        }
        for (qsizetype i = 0; i < m_rasters.size(); ++i) {
            if (m_rasters.at(i).id == id) {
                unusedBytes -= m_rasters.at(i).reservation->bytes();
                m_rasters.removeAt(i);
                update();
                break;
            }
        }
    }
}

void PDFPageItem::evictUnusedRasters(qint64 bytesNeeded)
{
    struct Candidate {
        PDFPageItem *item;
        quint64 id;
        quint64 lastUsed;
    };
    QVector<Candidate> candidates;
    for (PDFPageItem *item : rasterItems()) {
        const QRectF visible = item->normalizedVisibleRect();
        const QSize target = item->window()
            ? PDFRasterPlan::rasterSize(item->size(), item->window()->devicePixelRatio())
            : QSize();
        for (const RasterEntry &entry : item->m_rasters) {
            const auto &job = entry.job;
            const QRectF normalized(job.rect.x() / qreal(job.rasterSize.width()),
                                    job.rect.y() / qreal(job.rasterSize.height()),
                                    job.rect.width() / qreal(job.rasterSize.width()),
                                    job.rect.height() / qreal(job.rasterSize.height()));
            // Keep visible fallback coverage; old-resolution detail tiles may
            // be reclaimed while a replacement generation is being rendered.
            if (!normalized.intersects(visible)
                || (!job.base && (item->m_cacheDirty || target != item->m_cachedPixelSize))) {
                candidates.append({item, entry.id, entry.lastUsed});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.lastUsed < b.lastUsed; });
    qint64 scheduledRelease = 0;
    const qint64 shortage = bytesNeeded
        - (PDFRasterBudget::limitBytes - PDFRasterBudget::usedBytes());
    for (const Candidate &candidate : candidates) {
        if (scheduledRelease >= shortage) {
            break;
        }
        auto *item = candidate.item;
        for (qsizetype i = 0; i < item->m_rasters.size(); ++i) {
            if (item->m_rasters.at(i).id != candidate.id) {
                continue;
            }
            if (item->m_rasters.at(i).job.base) {
                for (const auto &entry : item->m_rasters) {
                    scheduledRelease += entry.reservation->bytes();
                }
                item->invalidateCache();
            } else {
                scheduledRelease += item->m_rasters.at(i).reservation->bytes();
                item->m_rasters.removeAt(i);
                item->update();
            }
            break;
        }
    }
}

void PDFPageItem::retryBudgetBlockedRenders()
{
    const qint64 available = PDFRasterBudget::limitBytes - PDFRasterBudget::usedBytes();
    for (PDFPageItem *item : rasterItems()) {
        if (item->m_budgetBlocked && item->m_budgetRetryBytes <= available
            && !item->normalizedVisibleRect().isEmpty()) {
            item->m_budgetBlocked = false;
            item->scheduleRender();
        }
    }
}

void PDFPageItem::handleRenderResult(quint64 requestId,
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
                                     bool budgetDenied)
{
    QQuickWindow *quickWindow = window();
    const qreal currentDevicePixelRatio = quickWindow
        ? quickWindow->devicePixelRatio()
        : 0.0;
    const QSize currentPixelSize = PDFRasterPlan::rasterSize(
        size(), currentDevicePixelRatio);
    if (requestId != m_requestSerial
        || !quickWindow
        || m_document.data() != document
        || !m_document
        || m_document->renderRevision() != revision
        || m_editController.data() != editController
        || m_formController.data() != formController
        || page != m_page
        || !qFuzzyCompare(devicePixelRatio, currentDevicePixelRatio)
        || pixelSize != m_requestedPixelSize
        || pixelSize != currentPixelSize
        || draftRevision != m_requestedDraftRevision
        || draftRevision != currentDraftRevision()
        || formRevision != m_requestedFormRevision
        || formRevision != currentFormRevision()) {
        return;
    }

    if (image.isNull()) {
        cancelPendingRender();
        if (budgetDenied) {
            if (job.base && !m_requestedTiled) {
                // A large full-page replacement can need more headroom than
                // the remaining capacity. Switch this item to native-resolution
                // tiles, accepting a small new-revision preview first.
                m_forceTiledForBudget = true;
                scheduleRender();
                return;
            }
            m_budgetBlocked = true;
            m_budgetRetryBytes = 2 * PDFRasterPlan::imageBytes(job.renderRect.size());
            if (!job.base) {
                m_budgetRetryBytes += kReplacementHeadroomBytes;
            }
            evictUnusedRasters(m_budgetRetryBytes);
            retryBudgetBlockedRenders();
        }
        return;
    }

    // A preview covers the WHOLE page at the new content revision. Swap it and
    // remove old tiles together before refining; never mix suppression states.
    if (job.base) {
        m_rasters.clear();
        m_cachedTiled = m_requestedTiled;
        m_cachedSuppressionApplied = suppressionApplied;
        setDisplayedSuppressedCommentObjectIds(suppressedCommentObjectIds);
    } else if (suppressionApplied != m_cachedSuppressionApplied) {
        // A partial suppression failure must not produce a mixed page mosaic.
        cancelPendingRender();
        return;
    }

    // Publish the suppression snapshot before requesting a scene-graph sync.
    // QML and the new texture are then synchronized into the same rendered
    // frame; the previously displayed frame remains internally consistent.
    m_rasters.append({++m_nextRasterId, job, image,
                      std::move(reservation), nextRasterUse()});
    m_cachedRequestId = requestId;
    m_cachedPixelSize = pixelSize;
    m_cachedPage = page;
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

    if (!m_renderJobs.isEmpty()) {
        m_renderJobs.removeFirst();
    }
    trimRasterCache();
    update();
    dispatchNextRaster();
}
