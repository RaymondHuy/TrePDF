#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMutexLocker>
#include <QPolygonF>
#include <QQuickTextDocument>
#include <QRunnable>
#include <QSet>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

using namespace PDFEditControllerPrivate;

namespace {

constexpr int kUnderlineRed = 198;

constexpr int kUnderlineGreen = 40;

constexpr int kUnderlineBlue = 40;

constexpr int kUnderlineAlpha = 192;

constexpr int kSquigglyRed = 198;

constexpr int kSquigglyGreen = 40;

constexpr int kSquigglyBlue = 40;

constexpr int kSquigglyAlpha = 192;

constexpr int kStrikeoutRed = 198;

constexpr int kStrikeoutGreen = 40;

constexpr int kStrikeoutBlue = 40;

constexpr int kStrikeoutAlpha = 192;

constexpr int kDrawingCommentRed = 198;

constexpr int kDrawingCommentGreen = 40;

constexpr int kDrawingCommentBlue = 40;

constexpr int kDrawingCommentAlpha = 255;

bool polygonContainsPoint(const QPolygonF &polygon,
                          const QPointF &point,
                          qreal tolerance)
{
    if (polygon.containsPoint(point, Qt::OddEvenFill)) {
        return true;
    }

    for (qsizetype index = 0; index < polygon.size(); ++index) {
        const QPointF &start = polygon.at(index);
        const QPointF &end = polygon.at((index + 1) % polygon.size());
        if (distanceToSegment(point, start, end) <= tolerance) {
            return true;
        }
    }
    return false;
}

bool shapeContainsPoint(PDFEditController::ShapeType shapeType,
                        const QRectF &normalizedFrame,
                        const QPointF &startRatio,
                        const QPointF &endRatio,
                        qreal strokeWidthRatio,
                        const QSizeF &pageSize,
                        const QPointF &normalizedPoint)
{
    if (normalizedFrame.isEmpty() || pageSize.isEmpty()) {
        return false;
    }

    const QRectF frame(
        normalizedFrame.x() * pageSize.width(),
        normalizedFrame.y() * pageSize.height(),
        normalizedFrame.width() * pageSize.width(),
        normalizedFrame.height() * pageSize.height());
    const QPointF point(
        normalizedPoint.x() * pageSize.width(),
        normalizedPoint.y() * pageSize.height());
    const qreal strokeWidth = shapeStrokeWidthPoints(
        strokeWidthRatio,
        pageSize.height());
    const qreal inset = std::min({
        strokeWidth / 2.0,
        frame.width() / 4.0,
        frame.height() / 4.0});
    const QRectF drawingRect = frame.adjusted(
        inset,
        inset,
        -inset,
        -inset);
    if (drawingRect.isEmpty()) {
        return false;
    }

    const qreal tolerance = shapeHitTolerancePoints(
        strokeWidthRatio,
        pageSize.height());
    const auto lineRatioPoint = [&frame](const QPointF &ratio) {
        return QPointF(
            frame.x()
                + qBound<qreal>(0.0, ratio.x(), 1.0)
                    * frame.width(),
            frame.y()
                + qBound<qreal>(0.0, ratio.y(), 1.0)
                    * frame.height());
    };

    // Closed shapes remain interactive throughout their interior, regardless
    // of fill or opacity. Keep this in sync with EditOverlay's move mask.
    switch (shapeType) {
    case PDFEditController::RectangleShape: {
        QPolygonF polygon;
        polygon << drawingRect.topLeft()
                << drawingRect.topRight()
                << drawingRect.bottomRight()
                << drawingRect.bottomLeft();
        return polygonContainsPoint(
            polygon,
            point,
            tolerance);
    }
    case PDFEditController::EllipseShape: {
        const qreal radiusX = drawingRect.width() / 2.0;
        const qreal radiusY = drawingRect.height() / 2.0;
        const qreal deltaX = point.x() - drawingRect.center().x();
        const qreal deltaY = point.y() - drawingRect.center().y();
        const qreal outerX = deltaX / (radiusX + tolerance);
        const qreal outerY = deltaY / (radiusY + tolerance);
        return outerX * outerX + outerY * outerY <= 1.0;
    }
    case PDFEditController::LineShape:
        return distanceToSegment(
                   point,
                   lineRatioPoint(startRatio),
                   lineRatioPoint(endRatio))
            <= tolerance;
    case PDFEditController::ArrowShape: {
        const QPointF start = lineRatioPoint(startRatio);
        const QPointF end = lineRatioPoint(endRatio);
        if (distanceToSegment(point, start, end) <= tolerance) {
            return true;
        }

        const QPointF direction = end - start;
        const qreal length = std::hypot(direction.x(), direction.y());
        if (length <= 0.0) {
            return false;
        }
        const QPointF unit = direction / length;
        const QPointF normal(-unit.y(), unit.x());
        const qreal headLength = qMin(
            qMax<qreal>(6.0, strokeWidth * 4.0),
            length * 0.35);
        const qreal halfWidth = headLength * 0.55;
        const QPointF headBase = end - unit * headLength;
        return distanceToSegment(
                   point,
                   headBase + normal * halfWidth,
                   end)
                <= tolerance
            || distanceToSegment(
                   point,
                   end,
                   headBase - normal * halfWidth)
                <= tolerance;
    }
    case PDFEditController::TriangleShape: {
        QPolygonF polygon;
        polygon << QPointF(drawingRect.center().x(), drawingRect.top())
                << drawingRect.bottomRight()
                << drawingRect.bottomLeft();
        return polygonContainsPoint(
            polygon,
            point,
            tolerance);
    }
    case PDFEditController::DiamondShape: {
        QPolygonF polygon;
        polygon << QPointF(drawingRect.center().x(), drawingRect.top())
                << QPointF(drawingRect.right(), drawingRect.center().y())
                << QPointF(drawingRect.center().x(), drawingRect.bottom())
                << QPointF(drawingRect.left(), drawingRect.center().y());
        return polygonContainsPoint(
            polygon,
            point,
            tolerance);
    }
    }
    return false;
}

} // namespace

PDFEditController::PDFEditController(QObject *parent)
    : QObject(parent)
    , m_highlightColor(
          kHighlightRed,
          kHighlightGreen,
          kHighlightBlue,
          kHighlightAlpha)
    , m_underlineColor(
          kUnderlineRed,
          kUnderlineGreen,
          kUnderlineBlue,
          kUnderlineAlpha)
    , m_squigglyColor(
          kSquigglyRed,
          kSquigglyGreen,
          kSquigglyBlue,
          kSquigglyAlpha)
    , m_strikeoutColor(
          kStrikeoutRed,
          kStrikeoutGreen,
          kStrikeoutBlue,
          kStrikeoutAlpha)
    , m_drawingCommentColor(
          kDrawingCommentRed,
          kDrawingCommentGreen,
          kDrawingCommentBlue,
          kDrawingCommentAlpha)
{
    connect(
        this,
        &PDFEditController::selectionChanged,
        this,
        &PDFEditController::textFormatStateChanged);
    connect(
        this,
        &PDFEditController::selectionChanged,
        this,
        &PDFEditController::selectedGeometryChanged);
}

PDFEditController::~PDFEditController()
{
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    resetCommentIndex();
    clearRegisteredFonts();
}

PDFDocument *PDFEditController::document() const
{
    return m_document;
}

void PDFEditController::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }

    m_document = document;
    if (m_document) {
        connect(m_document, &PDFDocument::sourceChanged, this, &PDFEditController::resetSession);
        connect(m_document, &PDFDocument::loadedChanged, this, &PDFEditController::resetSession);
        connect(m_document, &QObject::destroyed, this, [this]() {
            m_document = nullptr;
            resetSession();
            emit documentChanged();
        });
    }

    resetSession();
    emit documentChanged();
}

PDFFormController *PDFEditController::formController() const
{
    return m_formController;
}

void PDFEditController::setFormController(
    PDFFormController *formController)
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
            &PDFFormController::historyStateChanged,
            this,
            &PDFEditController::historyStateChanged);
        connect(m_formController, &QObject::destroyed, this, [this]() {
            m_formController = nullptr;
            emit historyStateChanged();
        });
    }
    emit historyStateChanged();
}

PDFFormDesignerController *PDFEditController::formDesignerController() const
{
    return m_formDesignerController;
}

void PDFEditController::setFormDesignerController(
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
            &PDFFormDesignerController::draftStateChanged,
            this,
            &PDFEditController::historyStateChanged);
        connect(
            m_formDesignerController,
            &PDFFormDesignerController::textFieldsChanged,
            this,
            &PDFEditController::notifyManagedWidgetVisibilityChanged);
        connect(
            m_formDesignerController,
            &QObject::destroyed,
            this,
            [this]() {
                m_formDesignerController = nullptr;
                notifyManagedWidgetVisibilityChanged(-1);
                emit historyStateChanged();
            });
    }
    notifyManagedWidgetVisibilityChanged(-1);
    emit historyStateChanged();
}

bool PDFEditController::isDesignerManagedSourceWidget(
    const Candidate &candidate) const
{
    // The annotation scan keeps the saved source rectangle until Save. The
    // form designer owns both rendering and hit-testing at the draft rectangle,
    // so exposing the same Widget here would create a stale hover target.
    return candidate.kind == QLatin1String("widget")
        && m_formDesignerController
        && m_formDesignerController->managesSourceWidget(
            candidate.page,
            candidate.sourceAnnotationIndex,
            candidate.sourceWidgetId);
}

void PDFEditController::notifyManagedWidgetVisibilityChanged(int page)
{
    if (m_hasSelection
        && (page < 0 || m_selectedCandidate.page == page)
        && isDesignerManagedSourceWidget(m_selectedCandidate)) {
        clearSelection();
    }

    if (page >= 0) {
        emit pageCandidatesReady(page);
        return;
    }

    const QList<int> cachedPages = m_pageCandidates.keys();
    for (int cachedPage : cachedPages) {
        emit pageCandidatesReady(cachedPage);
    }
}

PDFEditController::Tool PDFEditController::tool() const
{
    return m_tool;
}

int PDFEditController::pendingDrawingCommentPage() const
{
    return m_pendingDrawingCommentPage;
}

void PDFEditController::setPendingDrawingCommentPage(int page)
{
    if (m_pendingDrawingCommentPage == page) {
        return;
    }
    m_pendingDrawingCommentPage = page;
    emit pendingDrawingCommentPageChanged();
}

bool PDFEditController::claimDrawingCommentInteraction(int page)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || !isDrawingCommentTool(m_tool)
        || page < 0
        || page >= m_document->pageCount()) {
        return false;
    }

    setPendingDrawingCommentPage(page);
    return true;
}

void PDFEditController::releaseDrawingCommentInteraction(int page)
{
    if (page == m_pendingDrawingCommentPage) {
        setPendingDrawingCommentPage(-1);
    }
}

void PDFEditController::setTool(Tool tool)
{
    if (m_tool == tool) {
        return;
    }

    finishSelectedTextEdit();
    setPendingDrawingCommentPage(-1);
    m_tool = tool;
    if ((m_tool == CommentSelectTool
         && m_hasSelection
         && !isCommentCandidateKind(m_selectedCandidate.kind))
        || m_tool == BrowseTool
        || m_tool == StickyNoteTool
        || m_tool == FreeTextTool
        || m_tool == StampTool
        || isDrawingCommentTool(m_tool)
        || isTextMarkupTool(m_tool)) {
        clearSelection();
    }
    emit toolChanged();
}

PDFEditController::ShapeType PDFEditController::shapeType() const
{
    return m_shapeType;
}

void PDFEditController::setShapeType(ShapeType shapeType)
{
    if (!isSupportedShapeType(shapeType) || m_shapeType == shapeType) {
        return;
    }

    m_shapeType = shapeType;
    emit shapeTypeChanged();
}

bool PDFEditController::hasDrafts() const
{
    return !m_drafts.isEmpty()
        || !m_pendingTextMarkupCreations.isEmpty()
        || !m_pageInsertions.isEmpty()
        || !m_deletedSourcePages.isEmpty()
        || (m_formController && m_formController->hasDrafts())
        || (m_formDesignerController
            && m_formDesignerController->hasDrafts());
}

bool PDFEditController::canUndo() const
{
    return !m_undoStack.isEmpty()
        || (m_formController && m_formController->canUndo());
}

bool PDFEditController::canRedo() const
{
    return !m_redoStack.isEmpty()
        || (m_formController && m_formController->canRedo());
}

bool PDFEditController::saving() const
{
    return m_saving;
}

bool PDFEditController::textEditActive() const
{
    return m_textEditActive;
}

int PDFEditController::selectedPage() const
{
    return m_hasSelection ? m_selectedCandidate.page : -1;
}

QString PDFEditController::selectedObjectId() const
{
    return m_hasSelection ? m_selectedCandidate.objectId : QString();
}

QString PDFEditController::selectedKind() const
{
    return m_hasSelection ? m_selectedCandidate.kind : QString();
}

bool PDFEditController::selectedCommentReply() const
{
    return m_hasSelection
        && m_selectedCandidate.isReplyAnnotation;
}

QRectF PDFEditController::selectedSourceRect() const
{
    return m_hasSelection ? m_selectedCandidate.geometry.sourceRect : QRectF();
}

QRectF PDFEditController::selectedFrameRect() const
{
    return m_hasSelection ? selectedDraftForDisplay().frameRect : QRectF();
}

QRectF PDFEditController::selectedContentBounds() const
{
    return m_hasSelection ? selectedDraftForDisplay().contentBounds : QRectF();
}

QVariantList PDFEditController::selectedSourceRects() const
{
    return m_hasSelection ? candidateSourceRects(m_selectedCandidate) : QVariantList();
}

bool PDFEditController::selectedHasBaseline() const
{
    return m_hasSelection && m_selectedCandidate.hasBaseline;
}

qreal PDFEditController::selectedOriginX() const
{
    return selectedHasBaseline()
        ? layoutOriginForDraft(selectedDraftForDisplay()).x()
        : selectedFrameRect().x();
}

qreal PDFEditController::selectedBaselineY() const
{
    return selectedHasBaseline()
        ? layoutOriginForDraft(selectedDraftForDisplay()).y()
        : selectedFrameRect().y();
}

bool PDFEditController::selectedEditable() const
{
    return m_hasSelection && m_selectedCandidate.editable;
}

bool PDFEditController::candidateDeletable(const Candidate &candidate) const
{
    if (!isCommentCandidateKind(candidate.kind)) {
        return candidate.editable;
    }
    return documentAllowsAnnotations()
        && !candidate.commentTextReadOnly
        && (candidate.commentProperties.flags & FPDF_ANNOT_FLAG_LOCKED) == 0
        && !candidate.commentThreadDeletionProtected
        && (candidate.added || candidate.sourceAnnotationIndex >= 0);
}

bool PDFEditController::selectedDeletable() const
{
    return m_hasSelection && candidateDeletable(m_selectedCandidate);
}

QString PDFEditController::selectedReason() const
{
    return m_hasSelection ? m_selectedCandidate.reason : QString();
}

QVariantMap PDFEditController::candidateAt(int page, qreal x, qreal y)
{
    ensureCacheRevision();
    const QPointF point(x, y);
    const QSizeF pageSize = m_document
        ? m_document->pageSize(page)
        : QSizeF();
    const Candidate *bestCandidate = nullptr;
    qreal bestArea = 0.0;

    const auto considerCandidate = [&](const Candidate &candidate) {
        if (candidate.textMarkupHitTestOnly) {
            return;
        }
        if (isDesignerManagedSourceWidget(candidate)) {
            return;
        }
        if (candidate.isReplyAnnotation) {
            return;
        }
        if (m_tool == CommentSelectTool
            && !isCommentCandidateKind(candidate.kind)) {
            return;
        }
        if (m_tool == EditTextTool
            && isCommentCandidateKind(candidate.kind)) {
            return;
        }
        const auto draft = m_drafts.constFind(
            draftKey(candidate.page, candidate.objectId));
        if (draft != m_drafts.cend()
            && draftRepresentsDeletion(candidate.kind, draft->text, draft->deleted)) {
            return;
        }

        const bool draftGeometryCandidate
            = candidate.kind == QLatin1String("image")
            || candidate.kind == QLatin1String("addedImage")
            || isGeometryCommentCandidateKind(candidate.kind)
            || isShapeCandidateKind(candidate.kind);
        const QRectF candidateRect
            = draftGeometryCandidate && draft != m_drafts.cend()
            ? draft->frameRect
            : candidate.geometry.sourceRect;
        const QRectF hitRect = candidateRect.adjusted(
            -0.0025,
            -0.0025,
            0.0025,
            0.0025);
        if (!hitRect.contains(point)) {
            return;
        }
        if (isShapeCandidateKind(candidate.kind)
            && !shapeContainsPoint(
                candidate.shapeType,
                candidateRect,
                candidate.shapeStartRatio,
                candidate.shapeEndRatio,
                candidate.shapeStrokeWidthRatio,
                pageSize,
                point)) {
            return;
        }

        const qreal area = candidateRect.width() * candidateRect.height();
        const bool isBetter = !bestCandidate
            || (candidate.added && !bestCandidate->added)
            || (candidate.added == bestCandidate->added
                && candidate.editable && !bestCandidate->editable)
            || (candidate.added == bestCandidate->added
                && candidate.editable == bestCandidate->editable
                && area < bestArea);
        if (isBetter) {
            bestCandidate = &candidate;
            bestArea = area;
        }
    };

    // Added items always win the existing hit-test tie-break. Check them first
    // so Add Image -> Select hover never needs to enter PDFium while its draft
    // render is active.
    for (auto candidate = m_addedCandidates.crbegin();
         candidate != m_addedCandidates.crend();
         ++candidate) {
        if (candidate->page == page) {
            considerCandidate(*candidate);
        }
    }

    if (!bestCandidate) {
        // A cache miss never enters PDFium from the QML hover callback. One
        // serial worker scan runs at a time and repeated misses coalesce to the
        // latest requested page.
        requestPageCandidateScan(page);
        const QVector<Candidate> *candidates = cachedPageCandidates(page);
        if (candidates) {
            for (auto candidate = candidates->crbegin();
                 candidate != candidates->crend();
                 ++candidate) {
                considerCandidate(*candidate);
            }
        }
    }

    if (!bestCandidate) {
        return {};
    }

    const auto draft = m_drafts.constFind(draftKey(page, bestCandidate->objectId));
    const Draft displayDraft = draft == m_drafts.cend()
        ? initialDraftForCandidate(*bestCandidate)
        : *draft;
    return candidateMap(displayDraft);
}

bool PDFEditController::selectCandidate(int page, const QString &objectId)
{
    if (m_textEditActive
        && (m_selectedCandidate.page != page || m_selectedCandidate.objectId != objectId)) {
        finishSelectedTextEdit();
    }

    const Candidate *candidate = findCandidate(page, objectId);
    if (!candidate || candidate->commentStateAnnotation) {
        clearSelection();
        return false;
    }
    if (m_tool == CommentSelectTool
        && !isCommentCandidateKind(candidate->kind)) {
        return false;
    }
    const auto draft = m_drafts.constFind(draftKey(page, objectId));
    if (draft != m_drafts.cend()
        && draftRepresentsDeletion(candidate->kind, draft->text, draft->deleted)) {
        clearSelection();
        return false;
    }

    setSelection(candidate);
    if (!candidate->editable && !candidate->reason.isEmpty()) {
        if (isCommentCandidateKind(candidate->kind)
            && !documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        } else {
            emit statusMessage(candidate->reason);
        }
    }
    return true;
}

void PDFEditController::clearSelection()
{
    if (!m_hasSelection) {
        return;
    }
    finishSelectedTextEdit();
    setSelection(nullptr);
}

void PDFEditController::resetSession()
{
    setPendingDrawingCommentPage(-1);
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_deferredCandidateScanPage = -1;
    m_pendingTextMarkupCreations.clear();
    m_pageCandidates.clear();
    m_pageCandidateLru.clear();
    resetCommentIndex();
    m_addedCandidates.clear();
    m_pageInsertions.clear();
    m_deletedSourcePages.clear();
    if (m_document) {
        m_document->setTransientPageStructure(
            m_deletedSourcePages,
            m_pageInsertions);
    }
    m_drafts.clear();
    m_pageDraftRevisions.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_nextAddedObjectId = 0;
    m_cachedRevision = m_document ? m_document->renderRevision() : -1;
    m_hasSelection = false;
    m_selectedCandidate = Candidate();
    m_selectedText.clear();
    clearRegisteredFonts();
    const bool wasEditing = m_textEditActive;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();

    if (wasEditing) {
        emit textEditActiveChanged();
    }

    if ((!m_document || !m_document->isLoaded()) && m_tool != BrowseTool) {
        m_tool = BrowseTool;
        emit toolChanged();
    }

    emit pageDataChanged(-1);
    emit historyStateChanged();
    emit selectionChanged();
    emit commentsChanged();
}

const PDFEditController::Candidate *PDFEditController::findCandidate(int page,
                                                                     const QString &objectId)
{
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page == page && candidate.objectId == objectId
            && !isDesignerManagedSourceWidget(candidate)) {
            return &candidate;
        }
    }

    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    for (const Candidate &candidate : candidates) {
        if (candidate.objectId == objectId
            && !candidate.textMarkupHitTestOnly
            && !isDesignerManagedSourceWidget(candidate)) {
            return &candidate;
        }
    }
    return nullptr;
}

const PDFEditController::Candidate *PDFEditController::findCommentCandidate(
    int page,
    const QString &objectId)
{
    const Candidate *candidate = findCandidate(page, objectId);
    if (candidate
        && isCommentCandidateKind(candidate->kind)
        && !candidate->commentStateAnnotation) {
        return candidate;
    }

    const auto indexedPage = m_indexedCommentCandidates.constFind(page);
    if (indexedPage == m_indexedCommentCandidates.cend()) {
        return nullptr;
    }
    const auto indexedCandidate = std::find_if(
        indexedPage->cbegin(),
        indexedPage->cend(),
        [&objectId](const Candidate &current) {
            return current.objectId == objectId
                && isCommentCandidateKind(current.kind)
                && !current.commentStateAnnotation;
        });
    return indexedCandidate == indexedPage->cend()
        ? nullptr
        : &*indexedCandidate;
}

bool PDFEditController::documentAllowsEditing() const
{
    // PDFDocument snapshots permissions inside its open/metadata PDFium
    // transaction, so GUI actions never need a nested or standalone API call.
    return m_document
        && m_document->m_document
        && m_document->m_allowsContentEditing;
}

bool PDFEditController::documentAllowsAnnotations() const
{
    return m_document
        && m_document->m_document
        && m_document->m_allowsAnnotations;
}

bool PDFEditController::documentAllowsPageAssembly() const
{
    return m_document
        && m_document->m_document
        && m_document->m_allowsPageAssembly;
}

QString PDFEditController::draftKey(int page, const QString &objectId) const
{
    return QString::number(page) + QLatin1Char('|') + objectId;
}

void PDFEditController::setSelection(const Candidate *candidate)
{
    resetFrameResizeState();
    resetTypingFormat();
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    if (!candidate
        || candidate->commentStateAnnotation
        || (m_tool == CommentSelectTool
            && !isCommentCandidateKind(candidate->kind))) {
        m_hasSelection = false;
        m_selectedCandidate = Candidate();
        m_selectedText.clear();
        emit selectionChanged();
        return;
    }

    m_selectedCandidate = *candidate;
    m_hasSelection = true;
    const auto draft = m_drafts.constFind(draftKey(candidate->page, candidate->objectId));
    m_selectedText = draft == m_drafts.cend() ? candidate->text : draft->text;
    emit selectionChanged();
}
