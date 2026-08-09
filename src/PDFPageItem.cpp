#include "PDFPageItem.h"
#include "PDFFormWorker.h"
#include "PDFWidgetAnnotationSuppression.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
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

using CancellationFlag = std::shared_ptr<std::atomic_bool>;
constexpr unsigned long kMaximumAnnotationStringBytes
    = 4UL * 1024UL * 1024UL;

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

QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key)
{
    const unsigned long byteLength = annotation
        ? FPDFAnnot_GetStringValue(annotation, key, nullptr, 0)
        : 0;
    if (byteLength < sizeof(FPDF_WCHAR)
        || byteLength > kMaximumAnnotationStringBytes) {
        return {};
    }
    QVector<FPDF_WCHAR> buffer(static_cast<qsizetype>(
        (byteLength + sizeof(FPDF_WCHAR) - 1) / sizeof(FPDF_WCHAR)));
    const unsigned long actualLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
        buffer.data(),
        byteLength);
    if (actualLength < sizeof(FPDF_WCHAR)) {
        return {};
    }
    qsizetype characterCount = static_cast<qsizetype>(
        actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

bool isMarkupAnnotationSubtype(int subtype)
{
    switch (subtype) {
    case FPDF_ANNOT_TEXT:
    case FPDF_ANNOT_FREETEXT:
    case FPDF_ANNOT_LINE:
    case FPDF_ANNOT_SQUARE:
    case FPDF_ANNOT_CIRCLE:
    case FPDF_ANNOT_POLYGON:
    case FPDF_ANNOT_POLYLINE:
    case FPDF_ANNOT_HIGHLIGHT:
    case FPDF_ANNOT_UNDERLINE:
    case FPDF_ANNOT_SQUIGGLY:
    case FPDF_ANNOT_STRIKEOUT:
    case FPDF_ANNOT_STAMP:
    case FPDF_ANNOT_CARET:
    case FPDF_ANNOT_INK:
    case FPDF_ANNOT_FILEATTACHMENT:
    case FPDF_ANNOT_SOUND:
    case FPDF_ANNOT_REDACT:
        return true;
    default:
        return false;
    }
}

bool annotationUsesReplyRelation(FPDF_ANNOTATION annotation)
{
    const QString relation = annotationString(annotation, "RT").trimmed();
    return relation.isEmpty()
        || relation.compare(QLatin1String("R"), Qt::CaseInsensitive) == 0;
}

bool commentAnnotationIndices(
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
        bool markupReplyChain = isMarkupAnnotationSubtype(
            FPDFAnnot_GetSubtype(annotation));
        FPDF_ANNOTATION parent = replyRelation
            ? FPDFAnnot_GetLinkedAnnot(annotation, "IRT")
            : nullptr;
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
                && isMarkupAnnotationSubtype(
                    FPDFAnnot_GetSubtype(parent));
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
        if (!commentAnnotationIndices(page, deletions, annotationIndices)) {
            return false;
        }
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

private:
    struct State {
        int annotationIndex = -1;
        int flags = 0;
    };

    FPDF_PAGE m_page = nullptr;
    QVector<State> m_states;
    bool m_restored = false;
    bool m_restoreSucceeded = true;
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
    if (!commentAnnotationIndices(page, deletions, annotationIndices)) {
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
    const QVector<PDFEditController::DraftAnnotationDeletion>
        &deletedCommentAnnotations,
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
    bool annotationSuppressionApplied = deletedCommentAnnotations.isEmpty();
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
                    ScopedAnnotationSuppression annotationSuppression;
                    PDFWidgetAnnotationSuppression widgetSuppression;
                    QVector<PDFWidgetRenderPatch> widgetPatches;
                    bool omitSourceAnnotations = false;
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
                    if (!deletedCommentAnnotations.isEmpty()) {
                        annotationSuppressionApplied
                            = annotationSuppression.suppress(
                                previewPage,
                                deletedCommentAnnotations);
                        if (!annotationSuppressionApplied
                            && annotationSuppression.restorationSucceeded()) {
                            const PreviewAnnotationRemovalResult removal
                                = removePreviewAnnotations(
                                    previewPage,
                                    deletedCommentAnnotations);
                            annotationSuppressionApplied = removal.removedAll;
                            renderDocumentReusable
                                = removal.documentReusable;
                        }
                        if (!annotationSuppressionApplied) {
                            // The deleted annotation must never leak back into
                            // the draft preview. If both targeted strategies
                            // fail, omit the page annotation pass and let
                            // FPDF_FFLDraw render form widgets separately.
                            omitSourceAnnotations = true;
                            annotationSuppressionApplied = true;
                        }
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
                            0,
                            0,
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
                                deletedCommentAnnotations.isEmpty()
                                    ? FPDF_ANNOT
                                    : 0);
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
    const QVector<PDFEditController::DraftAnnotationDeletion>
        deletedCommentAnnotations = editController
            && editController->document() == document
        ? editController->deletedSourceCommentAnnotationsForPage(page)
        : QVector<PDFEditController::DraftAnnotationDeletion>();
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
                                       deletedCommentAnnotations,
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
            deletedCommentAnnotations,
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
