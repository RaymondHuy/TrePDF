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

constexpr qreal kMinimumTextMarkupDimensionRatio = 0.0005;

constexpr qreal kStickyNoteSizePoints = 24.0;

constexpr qreal kFreeTextDefaultWidthPoints = 160.0;

constexpr qreal kFreeTextDefaultHeightPoints = 64.0;

constexpr qreal kFreeTextMinimumWidthPoints = 36.0;

constexpr qreal kFreeTextMinimumHeightPoints = 24.0;

constexpr qreal kStampDefaultWidthPoints = 160.0;

constexpr qreal kStampDefaultHeightPoints = 50.0;

constexpr qreal kStampMinimumWidthPoints = 48.0;

constexpr qreal kStampMinimumHeightPoints = 24.0;

constexpr qreal kDrawingCommentBorderWidthPoints = 1.5;

constexpr qreal kDrawingCommentVertexMergeDistancePoints = 0.5;

constexpr qsizetype kMaximumRawInkPoints = 4096;

constexpr qsizetype kMaximumSimplifiedInkPoints = 1024;

constexpr qreal kInkSimplificationTolerancePoints = 0.75;

constexpr qsizetype kMaximumCommentCharacters = 700;

constexpr qreal kTextMarkupMinimumLineOverlapRatio = 0.25;

constexpr qreal kTextMarkupLineCenterToleranceRatio = 0.55;

constexpr qreal kTextMarkupMaximumGapHeightRatio = 1.25;

constexpr qreal kSquigglyBottomPaddingPoints = 3.5;

constexpr qsizetype kMaximumPendingTextMarkupCreations = 32;

QVector<QPointF> simplifyInkStrokeRdp(const QVector<QPointF> &points,
                                      const QSizeF &pageSize,
                                      qreal tolerancePoints)
{
    if (points.size() <= 2) {
        return points;
    }

    QVector<QPointF> pagePoints;
    pagePoints.reserve(points.size());
    for (const QPointF &point : points) {
        pagePoints.append(QPointF(
            point.x() * pageSize.width(),
            point.y() * pageSize.height()));
    }

    QVector<quint8> retained(points.size(), 0);
    retained[0] = 1;
    retained[points.size() - 1] = 1;
    QVector<std::pair<qsizetype, qsizetype>> pendingRanges;
    pendingRanges.append({0, points.size() - 1});
    while (!pendingRanges.isEmpty()) {
        const auto [startIndex, endIndex] = pendingRanges.takeLast();
        if (endIndex - startIndex <= 1) {
            continue;
        }

        qsizetype furthestIndex = -1;
        qreal furthestDistance = tolerancePoints;
        for (qsizetype index = startIndex + 1;
             index < endIndex;
             ++index) {
            const qreal distance = distanceToSegment(
                pagePoints.at(index),
                pagePoints.at(startIndex),
                pagePoints.at(endIndex));
            if (distance > furthestDistance) {
                furthestDistance = distance;
                furthestIndex = index;
            }
        }
        if (furthestIndex < 0) {
            continue;
        }

        retained[furthestIndex] = 1;
        pendingRanges.append({startIndex, furthestIndex});
        pendingRanges.append({furthestIndex, endIndex});
    }

    QVector<QPointF> simplified;
    simplified.reserve(points.size());
    for (qsizetype index = 0; index < points.size(); ++index) {
        if (retained.at(index) != 0) {
            simplified.append(points.at(index));
        }
    }
    return simplified;
}

QVector<QPointF> simplifyInkStroke(const QVector<QPointF> &points,
                                   const QSizeF &pageSize)
{
    QVector<QPointF> simplified = simplifyInkStrokeRdp(
        points,
        pageSize,
        kInkSimplificationTolerancePoints);
    if (simplified.size() <= kMaximumSimplifiedInkPoints) {
        return simplified;
    }

    qreal lowerTolerance = kInkSimplificationTolerancePoints;
    qreal upperTolerance = std::hypot(
        pageSize.width(),
        pageSize.height());
    QVector<QPointF> capped = simplifyInkStrokeRdp(
        points,
        pageSize,
        upperTolerance);
    for (int iteration = 0; iteration < 20; ++iteration) {
        const qreal tolerance = (lowerTolerance + upperTolerance) / 2.0;
        QVector<QPointF> candidate = simplifyInkStrokeRdp(
            points,
            pageSize,
            tolerance);
        if (candidate.size() > kMaximumSimplifiedInkPoints) {
            lowerTolerance = tolerance;
        } else {
            upperTolerance = tolerance;
            capped = std::move(candidate);
        }
    }
    if (capped.size() <= kMaximumSimplifiedInkPoints) {
        return capped;
    }

    // This fallback should only be reachable for pathological floating-point
    // page geometry. Preserve both endpoints while enforcing the hard cap.
    QVector<QPointF> sampled;
    sampled.reserve(kMaximumSimplifiedInkPoints);
    for (qsizetype index = 0;
         index < kMaximumSimplifiedInkPoints;
         ++index) {
        const qsizetype sourceIndex = index * (points.size() - 1)
            / (kMaximumSimplifiedInkPoints - 1);
        sampled.append(points.at(sourceIndex));
    }
    return sampled;
}

bool supportsCommentState(const QString &kind)
{
    return isGeometryCommentCandidateKind(kind);
}

PDFCommentAnnotation::DrawingType drawingTypeForTool(
    PDFEditController::Tool tool)
{
    switch (tool) {
    case PDFEditController::CircleTool:
        return PDFCommentAnnotation::DrawingType::Circle;
    case PDFEditController::LineTool:
        return PDFCommentAnnotation::DrawingType::Line;
    case PDFEditController::PolygonTool:
        return PDFCommentAnnotation::DrawingType::Polygon;
    case PDFEditController::PolyLineTool:
        return PDFCommentAnnotation::DrawingType::PolyLine;
    case PDFEditController::InkTool:
        return PDFCommentAnnotation::DrawingType::Ink;
    default:
        return PDFCommentAnnotation::DrawingType::Square;
    }
}

QString drawingLabel(PDFCommentAnnotation::DrawingType type)
{
    switch (type) {
    case PDFCommentAnnotation::DrawingType::Circle:
        //% "Oval annotation"
        return qtTrId("OvalAnnotation");
    case PDFCommentAnnotation::DrawingType::Line:
        //% "Line annotation"
        return qtTrId("LineAnnotation");
    case PDFCommentAnnotation::DrawingType::Polygon:
        //% "Polygon annotation"
        return qtTrId("PolygonAnnotation");
    case PDFCommentAnnotation::DrawingType::PolyLine:
        //% "Polyline annotation"
        return qtTrId("PolylineAnnotation");
    case PDFCommentAnnotation::DrawingType::Ink:
        //% "Ink annotation"
        return qtTrId("InkAnnotation");
    default:
        //% "Rectangle annotation"
        return qtTrId("RectangleAnnotation");
    }
}

QVector<QRectF> mergedTextMarkupRects(QVector<QRectF> glyphRects,
                                      const QSizeF &pageSize)
{
    glyphRects.erase(
        std::remove_if(
            glyphRects.begin(),
            glyphRects.end(),
            [](const QRectF &rect) {
                return rect.isEmpty()
                    || !std::isfinite(rect.x())
                    || !std::isfinite(rect.y())
                    || !std::isfinite(rect.width())
                    || !std::isfinite(rect.height());
            }),
        glyphRects.end());
    if (glyphRects.isEmpty()) {
        return {};
    }

    std::sort(
        glyphRects.begin(),
        glyphRects.end(),
        [](const QRectF &left, const QRectF &right) {
            if (!qFuzzyCompare(left.center().y() + 1.0,
                               right.center().y() + 1.0)) {
                return left.center().y() < right.center().y();
            }
            return left.left() < right.left();
        });

    struct GlyphLine {
        QRectF bounds;
        QVector<QRectF> glyphRects;
    };
    QVector<GlyphLine> lines;
    for (const QRectF &glyphRect : std::as_const(glyphRects)) {
        GlyphLine *bestLine = nullptr;
        qreal bestCenterDistance = -1.0;
        for (GlyphLine &line : lines) {
            const qreal overlap = qMax<qreal>(
                0.0,
                qMin(line.bounds.bottom(), glyphRect.bottom())
                    - qMax(line.bounds.top(), glyphRect.top()));
            const qreal minimumHeight = qMin(
                line.bounds.height(),
                glyphRect.height());
            const qreal centerDistance = qAbs(
                line.bounds.center().y() - glyphRect.center().y());
            const qreal centerTolerance
                = qMax(line.bounds.height(), glyphRect.height())
                * kTextMarkupLineCenterToleranceRatio;
            if (overlap < minimumHeight
                    * kTextMarkupMinimumLineOverlapRatio
                && centerDistance > centerTolerance) {
                continue;
            }
            if (bestCenterDistance < 0.0
                || centerDistance < bestCenterDistance) {
                bestLine = &line;
                bestCenterDistance = centerDistance;
            }
        }

        if (!bestLine) {
            lines.append(GlyphLine{glyphRect, {glyphRect}});
            continue;
        }
        bestLine->bounds = bestLine->bounds.united(glyphRect);
        bestLine->glyphRects.append(glyphRect);
    }

    std::sort(
        lines.begin(),
        lines.end(),
        [](const GlyphLine &left, const GlyphLine &right) {
            if (!qFuzzyCompare(left.bounds.top() + 1.0,
                               right.bounds.top() + 1.0)) {
                return left.bounds.top() < right.bounds.top();
            }
            return left.bounds.left() < right.bounds.left();
        });

    QVector<QRectF> result;
    for (GlyphLine &line : lines) {
        std::sort(
            line.glyphRects.begin(),
            line.glyphRects.end(),
            [](const QRectF &left, const QRectF &right) {
                return left.left() < right.left();
            });

        QRectF run;
        for (const QRectF &glyphRect : std::as_const(line.glyphRects)) {
            if (run.isEmpty()) {
                run = glyphRect;
                continue;
            }
            // QRectF axes are normalized independently; compare the gap and
            // glyph height only after restoring both to PDF page points.
            const qreal horizontalGapPoints
                = (glyphRect.left() - run.right()) * pageSize.width();
            const qreal maximumGapPoints
                = (std::max)(run.height(), glyphRect.height())
                * pageSize.height()
                * kTextMarkupMaximumGapHeightRatio;
            if (horizontalGapPoints <= maximumGapPoints) {
                run = run.united(glyphRect);
                continue;
            }
            result.append(run);
            run = glyphRect;
        }
        if (!run.isEmpty()) {
            result.append(run);
        }
    }
    return result;
}

} // namespace

std::optional<PDFEditController::CommentStatus>
PDFEditController::commentStatusForPdfName(const QString &state)
{
    const QString normalized = state.trimmed();
    if (normalized.compare(QLatin1String("None"), Qt::CaseInsensitive) == 0) {
        return NoCommentStatus;
    }
    if (normalized.compare(
            QLatin1String("Accepted"),
            Qt::CaseInsensitive) == 0) {
        return AcceptedCommentStatus;
    }
    if (normalized.compare(
            QLatin1String("Rejected"),
            Qt::CaseInsensitive) == 0) {
        return RejectedCommentStatus;
    }
    if (normalized.compare(
            QLatin1String("Cancelled"),
            Qt::CaseInsensitive) == 0) {
        return CancelledCommentStatus;
    }
    if (normalized.compare(
            QLatin1String("Completed"),
            Qt::CaseInsensitive) == 0) {
        return CompletedCommentStatus;
    }
    if (normalized.compare(
            QLatin1String("Deferred"),
            Qt::CaseInsensitive) == 0) {
        return DeferredCommentStatus;
    }
    if (normalized.compare(
            QLatin1String("Future"),
            Qt::CaseInsensitive) == 0) {
        return FutureCommentStatus;
    }
    return std::nullopt;
}

QColor PDFEditController::highlightColor() const
{
    return m_highlightColor;
}

void PDFEditController::setHighlightColor(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    if (m_highlightColor == normalizedColor) {
        return;
    }

    m_highlightColor = normalizedColor;
    emit highlightColorChanged();
}

QColor PDFEditController::underlineColor() const
{
    return m_underlineColor;
}

void PDFEditController::setUnderlineColor(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    if (m_underlineColor == normalizedColor) {
        return;
    }

    m_underlineColor = normalizedColor;
    emit underlineColorChanged();
}

QColor PDFEditController::squigglyColor() const
{
    return m_squigglyColor;
}

void PDFEditController::setSquigglyColor(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    if (m_squigglyColor == normalizedColor) {
        return;
    }

    m_squigglyColor = normalizedColor;
    emit squigglyColorChanged();
}

QColor PDFEditController::strikeoutColor() const
{
    return m_strikeoutColor;
}

void PDFEditController::setStrikeoutColor(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    if (m_strikeoutColor == normalizedColor) {
        return;
    }

    m_strikeoutColor = normalizedColor;
    emit strikeoutColorChanged();
}

QColor PDFEditController::drawingCommentColor() const
{
    return m_drawingCommentColor;
}

void PDFEditController::setDrawingCommentColor(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    if (m_drawingCommentColor == normalizedColor) {
        return;
    }

    m_drawingCommentColor = normalizedColor;
    emit drawingCommentColorChanged();
}

QString PDFEditController::stampName() const
{
    return m_stampName;
}

void PDFEditController::setStampName(const QString &name)
{
    if (!stampPreset(name) || m_stampName == name) {
        return;
    }

    m_stampName = name;
    emit stampNameChanged();
}

QString PDFEditController::stampText() const
{
    return stampTextForName(m_stampName);
}

QColor PDFEditController::stampColor() const
{
    return stampColorForName(m_stampName);
}

bool PDFEditController::commentStateEditable(
    const Candidate &candidate) const
{
    const int protectedFlags = FPDF_ANNOT_FLAG_READONLY
        | FPDF_ANNOT_FLAG_LOCKED
        | kAnnotationFlagLockedContents;
    return isCommentCandidateKind(candidate.kind)
        && supportsCommentState(candidate.kind)
        && !candidate.commentStateAnnotation
        && (!candidate.isReplyAnnotation
            || (!candidate.replyToObjectId.isEmpty()
                && commentReplyIsVisible(candidate)))
        && documentAllowsAnnotations()
        && (candidate.commentProperties.flags & protectedFlags) == 0
        && !candidate.commentTextReadOnly
        && (candidate.added || candidate.sourceAnnotationIndex >= 0);
}

QString PDFEditController::canonicalCommentStateAuthor(
    const QString &requestedAuthor) const
{
    const QString requested = requestedAuthor.trimmed();
    if (requested.isEmpty()) {
        return {};
    }

    QHash<QString, CommentStateAuthorEvidence> evidenceByAuthor;
    QSet<QString> seenSourceStates;
    const auto includeCandidates = [&](const QVector<Candidate> &candidates) {
        for (const Candidate &candidate : candidates) {
            if (!candidate.commentStateAnnotation
                || candidate.sourceAnnotationIndex < 0) {
                continue;
            }
            const QString sourceKey = QString::number(candidate.page)
                + QLatin1Char('|')
                + QString::number(candidate.sourceAnnotationIndex);
            if (seenSourceStates.contains(sourceKey)) {
                continue;
            }
            seenSourceStates.insert(sourceKey);
            const QDateTime timestamp
                = candidate.commentModifiedDate.isValid()
                ? candidate.commentModifiedDate
                : candidate.commentCreationDate;
            const qint64 pageSequence
                = static_cast<qint64>(qMax(0, candidate.page))
                    * (static_cast<qint64>(1) << 32)
                + candidate.sourceAnnotationIndex;
            includeCommentStateAuthorEvidence(
                evidenceByAuthor,
                requested,
                candidate.commentAuthor,
                timestamp,
                pageSequence);
        }
    };
    for (auto page = m_indexedCommentCandidates.cbegin();
         page != m_indexedCommentCandidates.cend();
         ++page) {
        includeCandidates(page.value());
    }
    for (auto page = m_pageCandidates.cbegin();
         page != m_pageCandidates.cend();
         ++page) {
        includeCandidates(page.value());
    }
    return preferredCommentStateAuthor(evidenceByAuthor, requested);
}

bool PDFEditController::drawingSupportsFill(const QString &kind)
{
    return kind == QLatin1String("square")
        || kind == QLatin1String("circle")
        || kind == QLatin1String("polygon");
}

bool PDFEditController::drawingPropertiesEditable(
    const Candidate &candidate) const
{
    if (isShapeCandidateKind(candidate.kind)) {
        return candidate.editable && documentAllowsEditing();
    }
    return isDrawingCommentCandidateKind(candidate.kind)
        && candidate.editable
        && candidate.drawingPropertiesEditable
        && documentAllowsAnnotations();
}

bool PDFEditController::selectedDrawingPropertiesEditable() const
{
    return m_hasSelection
        && drawingPropertiesEditable(m_selectedCandidate);
}

bool PDFEditController::selectedDrawingSupportsFill() const
{
    return m_hasSelection
        && (drawingSupportsFill(m_selectedCandidate.kind)
            || (isShapeCandidateKind(m_selectedCandidate.kind)
                && m_selectedCandidate.shapeType != LineShape
                && m_selectedCandidate.shapeType != ArrowShape));
}

QColor PDFEditController::selectedDrawingStrokeColor() const
{
    return m_hasSelection
            && (isDrawingCommentCandidateKind(m_selectedCandidate.kind)
                || isShapeCandidateKind(m_selectedCandidate.kind))
        ? selectedDraftForDisplay().commentProperties.strokeColor
        : QColor();
}

QColor PDFEditController::selectedDrawingInteriorColor() const
{
    return selectedDrawingSupportsFill()
        ? selectedDraftForDisplay().commentProperties.interiorColor
        : QColor();
}

bool PDFEditController::selectedDrawingHasInterior() const
{
    return selectedDrawingInteriorColor().isValid();
}

qreal PDFEditController::selectedDrawingOpacity() const
{
    return m_hasSelection
            && (isDrawingCommentCandidateKind(m_selectedCandidate.kind)
                || isShapeCandidateKind(m_selectedCandidate.kind))
        ? selectedDraftForDisplay().commentProperties.opacity
        : 1.0;
}

QVariantList PDFEditController::commentItems() const
{
    return buildCommentItems(true);
}

QVariantList PDFEditController::commentPanelItems() const
{
    return buildCommentItems(false);
}

QVariantList PDFEditController::buildCommentItems(
    bool includeCandidateDetails) const
{
    QVector<const Candidate *> comments;
    for (auto page = m_indexedCommentCandidates.cbegin();
         page != m_indexedCommentCandidates.cend();
         ++page) {
        for (const Candidate &candidate : page.value()) {
            if (isCommentCandidateKind(candidate.kind)
                && !candidate.commentStateAnnotation) {
                comments.append(&candidate);
            }
        }
    }
    for (const Candidate &candidate : m_addedCandidates) {
        if (isCommentCandidateKind(candidate.kind)
            && !candidate.commentStateAnnotation) {
            comments.append(&candidate);
        }
    }

    const auto geometryLess = [](const Candidate *left,
                                 const Candidate *right) {
            if (left->page != right->page) {
                return left->page < right->page;
            }
            if (!qFuzzyCompare(
                    left->geometry.sourceRect.top() + 1.0,
                    right->geometry.sourceRect.top() + 1.0)) {
                return left->geometry.sourceRect.top()
                    < right->geometry.sourceRect.top();
            }
            if (!qFuzzyCompare(
                    left->geometry.sourceRect.left() + 1.0,
                    right->geometry.sourceRect.left() + 1.0)) {
                return left->geometry.sourceRect.left()
                    < right->geometry.sourceRect.left();
            }
            if (left->addedOrder != right->addedOrder) {
                return left->addedOrder < right->addedOrder;
            }
            return left->objectId < right->objectId;
        };
    const auto replyLess = [&geometryLess](const Candidate *left,
                                           const Candidate *right) {
        if (left->commentCreationDate.isValid()
            && right->commentCreationDate.isValid()
            && left->commentCreationDate != right->commentCreationDate) {
            return left->commentCreationDate < right->commentCreationDate;
        }
        if (left->sourceAnnotationIndex != right->sourceAnnotationIndex
            && left->sourceAnnotationIndex >= 0
            && right->sourceAnnotationIndex >= 0) {
            return left->sourceAnnotationIndex < right->sourceAnnotationIndex;
        }
        return geometryLess(left, right);
    };

    QVector<const Candidate *> roots;
    QHash<QString, QVector<const Candidate *>> repliesByRoot;
    for (const Candidate *candidate : std::as_const(comments)) {
        const auto draft = m_drafts.constFind(
            draftKey(candidate->page, candidate->objectId));
        if (draft != m_drafts.cend()
            && draftRepresentsDeletion(
                candidate->kind,
                draft->text,
                draft->deleted)) {
            continue;
        }
        if (candidate->replyToObjectId.isEmpty()) {
            roots.append(candidate);
        } else if (commentReplyIsVisible(*candidate)) {
            repliesByRoot[draftKey(
                candidate->page,
                candidate->replyToObjectId)]
                .append(candidate);
        }
    }
    std::stable_sort(roots.begin(), roots.end(), geometryLess);
    for (auto replies = repliesByRoot.begin();
         replies != repliesByRoot.end();
         ++replies) {
        std::stable_sort(replies->begin(), replies->end(), replyLess);
    }

    QVariantList result;
    result.reserve(comments.size());
    QSet<QString> appendedReplies;
    const auto appendComment = [this,
                                &result,
                                includeCandidateDetails](
                                   const Candidate *candidate,
                                   bool isReply,
                                   int replyCount) {
        const auto draft = m_drafts.constFind(
            draftKey(candidate->page, candidate->objectId));
        const Draft displayDraft = draft == m_drafts.cend()
            ? initialDraftForCandidate(*candidate)
            : *draft;
        const Candidate &displayCandidate = displayDraft.candidate;
        QVariantMap map;
        if (includeCandidateDetails) {
            map = candidateMap(displayDraft);
        } else {
            map.insert(
                QStringLiteral("objectId"),
                displayCandidate.objectId);
            map.insert(QStringLiteral("kind"), displayCandidate.kind);
            map.insert(QStringLiteral("label"), displayCandidate.label);
            map.insert(QStringLiteral("text"), displayDraft.text);
            map.insert(
                QStringLiteral("commentAuthor"),
                displayCandidate.commentAuthor);
            map.insert(
                QStringLiteral("commentCreationDate"),
                displayCandidate.commentCreationDate);
            map.insert(
                QStringLiteral("commentModifiedDate"),
                displayCandidate.commentModifiedDate);
            map.insert(
                QStringLiteral("commentDate"),
                displayCandidate.commentModifiedDate.isValid()
                    ? displayCandidate.commentModifiedDate
                    : displayCandidate.commentCreationDate);
            map.insert(
                QStringLiteral("deletable"),
                candidateDeletable(displayCandidate));
            map.insert(QStringLiteral("added"), displayCandidate.added);
        }
        map.insert(QStringLiteral("page"), candidate->page);
        map.insert(
            QStringLiteral("commentStatus"),
            static_cast<int>(displayDraft.commentStatus));
        map.insert(
            QStringLiteral("commentMarked"),
            displayDraft.commentMarked);
        const bool stateEditable = commentStateEditable(displayCandidate);
        map.insert(
            QStringLiteral("commentStateEditable"),
            stateEditable);
        QString commentStateEditReason;
        if (!stateEditable) {
            if (!documentAllowsAnnotations()) {
                //% "This PDF does not permit changing annotations."
                commentStateEditReason = qtTrId(
                    "PdfAnnotationsNotPermitted");
            } else {
                //% "This comment is read-only."
                commentStateEditReason = qtTrId(
                    "ReadOnlyCommentCannotBeEdited");
            }
        }
        map.insert(
            QStringLiteral("commentStateEditReason"),
            commentStateEditReason);
        map.insert(
            QStringLiteral("commentTextEditable"),
            documentAllowsAnnotations()
                && !candidate->commentTextReadOnly);
        QString commentTextEditReason;
        if (candidate->commentTextReadOnly) {
            //% "This comment is read-only."
            commentTextEditReason = qtTrId(
                "ReadOnlyCommentCannotBeEdited");
        } else if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            commentTextEditReason = qtTrId(
                "PdfAnnotationsNotPermitted");
        }
        map.insert(
            QStringLiteral("commentTextEditReason"),
            commentTextEditReason);
        const QColor markerColor
            = isDrawingCommentCandidateKind(displayCandidate.kind)
            ? displayDraft.commentProperties.strokeColor
            : candidate->highlightColor.isValid()
                ? candidate->highlightColor
                : candidate->kind == QLatin1String("stickyNote")
                ? QColor(kHighlightRed, kHighlightGreen, kHighlightBlue)
                : QColor(kHighlightRed,
                         kHighlightGreen,
                         kHighlightBlue,
                         kHighlightAlpha);
        map.insert(QStringLiteral("color"), markerColor);
        map.insert(QStringLiteral("isReply"), isReply);
        map.insert(
            QStringLiteral("parentObjectId"),
            isReply ? candidate->replyToObjectId : QString());
        map.insert(QStringLiteral("replyCount"), replyCount);
        result.append(map);
    };
    for (const Candidate *root : std::as_const(roots)) {
        const QString rootKey = draftKey(root->page, root->objectId);
        const QVector<const Candidate *> replies = repliesByRoot.value(rootKey);
        appendComment(root, false, replies.size());
        for (const Candidate *reply : replies) {
            appendComment(reply, true, 0);
            appendedReplies.insert(draftKey(reply->page, reply->objectId));
        }
    }

    // Malformed or cross-page source reply relationships are still readable,
    // but are presented as top-level comments rather than creating a deeper
    // or dangling thread in the panel.
    QVector<const Candidate *> orphanReplies;
    for (auto replies = repliesByRoot.cbegin();
         replies != repliesByRoot.cend();
         ++replies) {
        for (const Candidate *reply : replies.value()) {
            if (!appendedReplies.contains(
                    draftKey(reply->page, reply->objectId))) {
                orphanReplies.append(reply);
            }
        }
    }
    std::stable_sort(orphanReplies.begin(), orphanReplies.end(), geometryLess);
    for (const Candidate *reply : std::as_const(orphanReplies)) {
        appendComment(reply, false, 0);
    }
    return result;
}

PDFEditController::CommentIndexState PDFEditController::commentIndexState() const
{
    return m_commentIndexState;
}

int PDFEditController::maximumCommentCharacters() const
{
    return static_cast<int>(kMaximumCommentCharacters);
}

QVector<QRectF> PDFEditController::resolvedTextMarkupRectsForSelection(
    int page,
    const QRectF &selectionRect,
    Tool tool,
    bool *geometryReady)
{
    if (geometryReady) {
        *geometryReady = false;
    }
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()
        || !std::isfinite(selectionRect.x())
        || !std::isfinite(selectionRect.y())
        || !std::isfinite(selectionRect.width())
        || !std::isfinite(selectionRect.height())) {
        return {};
    }

    const QRectF boundedSelection = selectionRect.normalized().intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    if (boundedSelection.isEmpty()) {
        return {};
    }

    ensurePageCandidates(page);
    const QVector<Candidate> *candidates = cachedPageCandidates(page);
    if (!candidates) {
        return {};
    }
    if (geometryReady) {
        *geometryReady = true;
    }

    QVector<QRectF> selectedGlyphRects;
    for (const Candidate &candidate : *candidates) {
        if (candidate.kind != QLatin1String("text")
            || candidate.textGlyphRects.isEmpty()) {
            continue;
        }

        QPointF draftTranslation;
        const auto draft = m_drafts.constFind(
            draftKey(candidate.page, candidate.objectId));
        if (draft != m_drafts.cend()) {
            if (draftRepresentsDeletion(
                    candidate.kind,
                    draft->text,
                    draft->deleted)) {
                continue;
            }

            const Draft initialDraft = initialDraftForCandidate(candidate);
            if (!sameDraftContent(*draft, initialDraft)) {
                if (!isSourceTextMoveOnly(*draft)) {
                    // Per-glyph geometry belongs to the source text. Once the
                    // text is reflowed, it is no longer safe to reuse it.
                    continue;
                }
                draftTranslation = draft->frameRect.topLeft()
                    - initialDraft.frameRect.topLeft();
            }
        }

        const QRectF candidateBounds = candidate.geometry.sourceRect
            .translated(draftTranslation);
        if (!candidateBounds.intersects(boundedSelection)) {
            continue;
        }

        for (QRectF glyphRect : candidate.textGlyphRects) {
            glyphRect.translate(draftTranslation);
            glyphRect = glyphRect.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
            if (!glyphRect.isEmpty() && glyphRect.intersects(boundedSelection)) {
                selectedGlyphRects.append(glyphRect);
            }
        }
    }
    const QSizeF pageSize = m_document->pageSize(page);
    if (!std::isfinite(pageSize.width())
        || !std::isfinite(pageSize.height())
        || pageSize.width() <= 0.0
        || pageSize.height() <= 0.0) {
        return {};
    }
    QVector<QRectF> markupRects = mergedTextMarkupRects(
        std::move(selectedGlyphRects),
        pageSize);

    if (tool == SquigglyTool) {
        // Keep the top and width aligned to the glyphs while extending the
        // bottom by enough padding for the wave band plus a visible gap.
        const qreal gapRatio
            = kSquigglyBottomPaddingPoints / pageSize.height();
        for (QRectF &markupRect : markupRects) {
            markupRect.setBottom(qMin<qreal>(
                1.0,
                markupRect.bottom() + gapRatio));
        }
    }
    return markupRects;
}

QVariantList PDFEditController::textMarkupRectsForSelection(
    int page,
    const QRectF &selectionRect)
{
    const Tool markupTool = isTextMarkupTool(m_tool)
        ? m_tool
        : HighlightTool;
    const QVector<QRectF> rects = resolvedTextMarkupRectsForSelection(
        page,
        selectionRect,
        markupTool,
        nullptr);

    QVariantList result;
    result.reserve(rects.size());
    for (const QRectF &rect : rects) {
        QVariantMap map;
        map.insert(QStringLiteral("x"), rect.x());
        map.insert(QStringLiteral("y"), rect.y());
        map.insert(QStringLiteral("width"), rect.width());
        map.insert(QStringLiteral("height"), rect.height());
        result.append(std::move(map));
    }
    return result;
}

QVariantMap PDFEditController::addHighlight(int page,
                                            const QRectF &selectionRect)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return {};
    }
    if (!std::isfinite(selectionRect.x())
        || !std::isfinite(selectionRect.y())
        || !std::isfinite(selectionRect.width())
        || !std::isfinite(selectionRect.height())) {
        return {};
    }

    const QRectF normalizedRect = selectionRect.normalized().intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    const Tool markupTool = isTextMarkupTool(m_tool)
        ? m_tool
        : HighlightTool;
    if (normalizedRect.width() < kMinimumTextMarkupDimensionRatio
        || normalizedRect.height() <= 0.0) {
        return {};
    }

    QColor markupColor = m_highlightColor;
    QString markupKind = QStringLiteral("highlight");
    if (markupTool == UnderlineTool) {
        markupColor = m_underlineColor;
        markupKind = QStringLiteral("underline");
    } else if (markupTool == SquigglyTool) {
        markupColor = m_squigglyColor;
        markupKind = QStringLiteral("squiggly");
    } else if (markupTool == StrikeoutTool) {
        markupColor = m_strikeoutColor;
        markupKind = QStringLiteral("strikeout");
    }

    bool geometryReady = false;
    const QVector<QRectF> annotationRects
        = resolvedTextMarkupRectsForSelection(
            page,
            normalizedRect,
            markupTool,
            &geometryReady);
    if (!geometryReady) {
        if (m_pendingTextMarkupCreations.size()
            >= kMaximumPendingTextMarkupCreations) {
            //% "Text information is still loading. Please try again."
            emit statusMessage(qtTrId("TextGeometryStillLoading"));
            return {};
        }

        PendingTextMarkupCreation pendingCreation;
        pendingCreation.filePath = m_document->renderFilePath();
        pendingCreation.selectionRect = normalizedRect;
        pendingCreation.pageSize = m_document->pageSize(page);
        pendingCreation.color = markupColor;
        pendingCreation.tool = markupTool;
        pendingCreation.revision = m_document->renderRevision();
        pendingCreation.page = page;
        pendingCreation.sourcePage = m_document->sourcePageIndex(page);
        m_pendingTextMarkupCreations.append(std::move(pendingCreation));
        requestPageCandidateScan(page);
        emit historyStateChanged();

        QVariantMap pendingResult;
        pendingResult.insert(QStringLiteral("kind"), markupKind);
        pendingResult.insert(QStringLiteral("page"), page);
        pendingResult.insert(QStringLiteral("pending"), true);
        return pendingResult;
    }
    if (annotationRects.isEmpty()) {
        //% "No text was found in the selected area."
        emit statusMessage(qtTrId("NoTextInSelectedArea"));
        return {};
    }
    return addTextMarkupDraft(
        page,
        annotationRects,
        markupTool,
        markupColor);
}

QVariantMap PDFEditController::addTextMarkupDraft(
    int page,
    const QVector<QRectF> &annotationRects,
    Tool tool,
    const QColor &color)
{
    if (annotationRects.isEmpty()) {
        return {};
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    const QVariantMap draft = materializeTextMarkupDraft(
        page,
        annotationRects,
        tool,
        color);
    if (draft.isEmpty()) {
        return {};
    }

    setSelection(nullptr);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return draft;
}

QVariantMap PDFEditController::materializeTextMarkupDraft(
    int page,
    const QVector<QRectF> &annotationRects,
    Tool tool,
    const QColor &color)
{
    if (annotationRects.isEmpty()) {
        return {};
    }

    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    switch (tool) {
    case UnderlineTool:
        candidate.kind = QStringLiteral("underline");
        //% "Underline"
        candidate.label = qtTrId("Underline");
        break;
    case SquigglyTool:
        candidate.kind = QStringLiteral("squiggly");
        //% "Squiggly"
        candidate.label = qtTrId("Squiggly");
        break;
    case StrikeoutTool:
        candidate.kind = QStringLiteral("strikeout");
        //% "Strikeout"
        candidate.label = qtTrId("Strikeout");
        break;
    default:
        candidate.kind = QStringLiteral("highlight");
        //% "Highlight"
        candidate.label = qtTrId("Highlight");
        break;
    }
    candidate.objectId = candidate.kind + QLatin1Char(':')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.commentThreadId = candidate.objectId;
    candidate.editable = true;
    candidate.added = true;
    candidate.commentAuthor = currentCommentAuthor();
    candidate.commentCreationDate = QDateTime::currentDateTime();
    candidate.highlightRects = annotationRects;
    candidate.textMarkupQuads.reserve(annotationRects.size());
    for (const QRectF &rect : annotationRects) {
        const QRectF bounds = rect.normalized();
        candidate.textMarkupQuads.append(std::array<QPointF, 4>{
            bounds.topLeft(),
            bounds.topRight(),
            bounds.bottomLeft(),
            bounds.bottomRight()});
    }
    candidate.highlightColor = color;
    QRectF annotationBounds;
    for (const QRectF &rect : std::as_const(annotationRects)) {
        annotationBounds = annotationBounds.isEmpty()
            ? rect
            : annotationBounds.united(rect);
    }
    setInitialGeometry(candidate, annotationBounds);

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    return candidateMap(draft);
}

bool PDFEditController::pendingTextMarkupCreationIsCurrent(
    const PendingTextMarkupCreation &creation) const
{
    return m_document
        && m_document->isLoaded()
        && m_document->renderRevision() == creation.revision
        && m_document->renderFilePath() == creation.filePath
        && creation.page >= 0
        && creation.page < m_document->pageCount()
        && m_document->sourcePageIndex(creation.page) == creation.sourcePage
        && m_document->pageSize(creation.page) == creation.pageSize;
}

int PDFEditController::pendingTextMarkupScanPage() const
{
    for (const PendingTextMarkupCreation &creation
         : m_pendingTextMarkupCreations) {
        if (pendingTextMarkupCreationIsCurrent(creation)
            && !m_pageCandidates.contains(creation.page)) {
            return creation.page;
        }
    }
    return -1;
}

void PDFEditController::completePendingTextMarkupCreations(int page)
{
    if (m_pendingTextMarkupCreations.isEmpty()) {
        return;
    }

    QVector<PendingTextMarkupCreation> readyCreations;
    QVector<PendingTextMarkupCreation> remainingCreations;
    readyCreations.reserve(m_pendingTextMarkupCreations.size());
    remainingCreations.reserve(m_pendingTextMarkupCreations.size());
    for (PendingTextMarkupCreation &creation
         : m_pendingTextMarkupCreations) {
        if (creation.page == page) {
            readyCreations.append(std::move(creation));
        } else {
            remainingCreations.append(std::move(creation));
        }
    }
    m_pendingTextMarkupCreations = std::move(remainingCreations);
    if (readyCreations.isEmpty()) {
        return;
    }

    bool createdDraft = false;
    bool pendingStateChanged = false;
    for (const PendingTextMarkupCreation &creation : readyCreations) {
        if (!pendingTextMarkupCreationIsCurrent(creation)) {
            pendingStateChanged = true;
            continue;
        }
        if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
            pendingStateChanged = true;
            continue;
        }

        bool geometryReady = false;
        const QVector<QRectF> annotationRects
            = resolvedTextMarkupRectsForSelection(
                creation.page,
                creation.selectionRect,
                creation.tool,
                &geometryReady);
        if (!geometryReady) {
            m_pendingTextMarkupCreations.append(creation);
            requestPageCandidateScan(creation.page);
            continue;
        }
        pendingStateChanged = true;
        if (annotationRects.isEmpty()) {
            //% "No text was found in the selected area."
            emit statusMessage(qtTrId("NoTextInSelectedArea"));
            continue;
        }

        const QVariantMap draft = materializeTextMarkupDraft(
            creation.page,
            annotationRects,
            creation.tool,
            creation.color);
        if (!draft.isEmpty()) {
            markDraftStateChanged(creation.page, true);
            emit historyStateChanged();
            createdDraft = true;
        }
    }

    if (pendingStateChanged && !createdDraft) {
        emit historyStateChanged();
    }
}

QVariantMap PDFEditController::addStickyNote(int page,
                                             const QPointF &position)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()
        || !std::isfinite(position.x())
        || !std::isfinite(position.y())) {
        return {};
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }
    const qreal noteWidth = qMin<qreal>(1.0, kStickyNoteSizePoints / pageSize.width());
    const qreal noteHeight = qMin<qreal>(1.0, kStickyNoteSizePoints / pageSize.height());
    const qreal left = qBound<qreal>(0.0, position.x(), 1.0 - noteWidth);
    const qreal top = qBound<qreal>(0.0, position.y(), 1.0 - noteHeight);
    const QRectF noteRect(left, top, noteWidth, noteHeight);

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("stickyNote:")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.commentThreadId = candidate.objectId;
    candidate.kind = QStringLiteral("stickyNote");
    //% "Sticky Note"
    candidate.label = qtTrId("StickyNote");
    candidate.editable = true;
    candidate.added = true;
    candidate.commentAuthor = currentCommentAuthor();
    candidate.commentCreationDate = QDateTime::currentDateTime();
    candidate.highlightColor = QColor(
        kHighlightRed,
        kHighlightGreen,
        kHighlightBlue);
    setInitialGeometry(candidate, noteRect);

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(CommentSelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QVariantMap PDFEditController::addFreeText(int page,
                                           const QRectF &selectionRect)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()
        || !std::isfinite(selectionRect.x())
        || !std::isfinite(selectionRect.y())
        || !std::isfinite(selectionRect.width())
        || !std::isfinite(selectionRect.height())) {
        return {};
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }

    const QRectF normalizedSelection = selectionRect.normalized();
    const qreal selectionWidthPoints
        = normalizedSelection.width() * pageSize.width();
    const qreal selectionHeightPoints
        = normalizedSelection.height() * pageSize.height();
    QRectF freeTextRect;
    if (selectionWidthPoints < kMinimumShapeDragPoints
        || selectionHeightPoints < kMinimumShapeDragPoints) {
        const qreal width = qMin<qreal>(
            1.0,
            kFreeTextDefaultWidthPoints / pageSize.width());
        const qreal height = qMin<qreal>(
            1.0,
            kFreeTextDefaultHeightPoints / pageSize.height());
        const qreal left = qBound<qreal>(
            0.0,
            normalizedSelection.left(),
            1.0 - width);
        const qreal top = qBound<qreal>(
            0.0,
            normalizedSelection.top(),
            1.0 - height);
        freeTextRect = QRectF(left, top, width, height);
    } else {
        const qreal width = qMin<qreal>(
            1.0,
            qMax(
                normalizedSelection.width(),
                kFreeTextMinimumWidthPoints / pageSize.width()));
        const qreal height = qMin<qreal>(
            1.0,
            qMax(
                normalizedSelection.height(),
                kFreeTextMinimumHeightPoints / pageSize.height()));
        const qreal left = qBound<qreal>(
            0.0,
            normalizedSelection.left(),
            1.0 - width);
        const qreal top = qBound<qreal>(
            0.0,
            normalizedSelection.top(),
            1.0 - height);
        freeTextRect = QRectF(left, top, width, height);
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("freeText:")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.commentThreadId = candidate.objectId;
    candidate.kind = QStringLiteral("freeText");
    //% "Free text annotation"
    candidate.label = qtTrId("FreeTextAnnotation");
    candidate.fontFamily = QStringLiteral("Helvetica");
    candidate.fontSizeRatio = qMin<qreal>(
        1.0,
        kFreeTextFontSizePoints / pageSize.height());
    candidate.displayFontSizeRatio = candidate.fontSizeRatio;
    candidate.color = QColor(32, 33, 36);
    candidate.highlightColor = Qt::black;
    candidate.editable = true;
    candidate.added = true;
    candidate.commentAuthor = currentCommentAuthor();
    candidate.commentCreationDate = QDateTime::currentDateTime();
    setInitialGeometry(candidate, freeTextRect);

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(CommentSelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QVariantMap PDFEditController::addStamp(int page,
                                        const QRectF &selectionRect)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()
        || !std::isfinite(selectionRect.x())
        || !std::isfinite(selectionRect.y())
        || !std::isfinite(selectionRect.width())
        || !std::isfinite(selectionRect.height())
        || !stampPreset(m_stampName)) {
        return {};
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }

    const QRectF normalizedSelection = selectionRect.normalized();
    const qreal selectionWidthPoints
        = normalizedSelection.width() * pageSize.width();
    const qreal selectionHeightPoints
        = normalizedSelection.height() * pageSize.height();
    QRectF stampRect;
    if (selectionWidthPoints < kMinimumShapeDragPoints
        || selectionHeightPoints < kMinimumShapeDragPoints) {
        const qreal width = qMin<qreal>(
            1.0,
            kStampDefaultWidthPoints / pageSize.width());
        const qreal height = qMin<qreal>(
            1.0,
            kStampDefaultHeightPoints / pageSize.height());
        stampRect = QRectF(
            qBound<qreal>(
                0.0,
                normalizedSelection.left(),
                1.0 - width),
            qBound<qreal>(
                0.0,
                normalizedSelection.top(),
                1.0 - height),
            width,
            height);
    } else {
        const qreal width = qMin<qreal>(
            1.0,
            qMax(
                normalizedSelection.width(),
                kStampMinimumWidthPoints / pageSize.width()));
        const qreal height = qMin<qreal>(
            1.0,
            qMax(
                normalizedSelection.height(),
                kStampMinimumHeightPoints / pageSize.height()));
        stampRect = QRectF(
            qBound<qreal>(
                0.0,
                normalizedSelection.left(),
                1.0 - width),
            qBound<qreal>(
                0.0,
                normalizedSelection.top(),
                1.0 - height),
            width,
            height);
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("stamp:")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.commentThreadId = candidate.objectId;
    candidate.kind = QStringLiteral("stamp");
    //% "Stamp annotation"
    candidate.label = qtTrId("StampAnnotation");
    candidate.commentProperties.name = m_stampName;
    candidate.stampText = stampText();
    candidate.text = candidate.stampText;
    candidate.highlightColor = stampColor();
    candidate.editable = true;
    candidate.added = true;
    candidate.commentAuthor = currentCommentAuthor();
    candidate.commentCreationDate = QDateTime::currentDateTime();
    setInitialGeometry(candidate, stampRect);

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(CommentSelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QVariantMap PDFEditController::addDrawingComment(
    int page,
    const QVariantList &vertices)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()
        || !isDrawingCommentTool(m_tool)
        || vertices.size()
            > (m_tool == InkTool
                   ? kMaximumRawInkPoints
                   : kMaximumDrawingCommentVertices)) {
        return {};
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }

    QVector<QPointF> normalizedVertices;
    normalizedVertices.reserve(vertices.size());
    for (const QVariant &value : vertices) {
        if (!value.canConvert<QPointF>()) {
            return {};
        }
        QPointF point = value.toPointF();
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            return {};
        }
        point.setX(std::clamp(point.x(), 0.0, 1.0));
        point.setY(std::clamp(point.y(), 0.0, 1.0));
        if (!normalizedVertices.isEmpty()) {
            const QPointF previous = normalizedVertices.constLast();
            const qreal distance = std::hypot(
                (point.x() - previous.x()) * pageSize.width(),
                (point.y() - previous.y()) * pageSize.height());
            if (distance < kDrawingCommentVertexMergeDistancePoints) {
                continue;
            }
        }
        normalizedVertices.append(point);
    }

    const PDFCommentAnnotation::DrawingType type = drawingTypeForTool(m_tool);
    qreal inkPathLengthPoints = 0.0;
    if (type == PDFCommentAnnotation::DrawingType::Ink) {
        normalizedVertices = simplifyInkStroke(
            normalizedVertices,
            pageSize);
        QVector<QPointF> distinctVertices;
        distinctVertices.reserve(normalizedVertices.size());
        for (const QPointF &point : std::as_const(normalizedVertices)) {
            if (!distinctVertices.isEmpty()) {
                const QPointF previous = distinctVertices.constLast();
                const qreal distance = std::hypot(
                    (point.x() - previous.x()) * pageSize.width(),
                    (point.y() - previous.y()) * pageSize.height());
                if (distance <= 0.0) {
                    continue;
                }
            }
            distinctVertices.append(point);
        }
        normalizedVertices = std::move(distinctVertices);
        for (qsizetype index = 1;
             index < normalizedVertices.size();
             ++index) {
            const QPointF previous = normalizedVertices.at(index - 1);
            const QPointF point = normalizedVertices.at(index);
            inkPathLengthPoints += std::hypot(
                (point.x() - previous.x()) * pageSize.width(),
                (point.y() - previous.y()) * pageSize.height());
        }
    }
    if (type == PDFCommentAnnotation::DrawingType::Polygon
        && normalizedVertices.size() > 2) {
        const QPointF first = normalizedVertices.constFirst();
        const QPointF last = normalizedVertices.constLast();
        const qreal closingDistance = std::hypot(
            (first.x() - last.x()) * pageSize.width(),
            (first.y() - last.y()) * pageSize.height());
        if (closingDistance < kDrawingCommentVertexMergeDistancePoints) {
            normalizedVertices.removeLast();
        }
    }

    if ((type == PDFCommentAnnotation::DrawingType::Polygon
         && normalizedVertices.size() < 3)
        || (type == PDFCommentAnnotation::DrawingType::PolyLine
            && normalizedVertices.size() < 2)
        || (type == PDFCommentAnnotation::DrawingType::Ink
            && normalizedVertices.size() < 2)
        || ((type == PDFCommentAnnotation::DrawingType::Square
             || type == PDFCommentAnnotation::DrawingType::Circle
             || type == PDFCommentAnnotation::DrawingType::Line)
            && normalizedVertices.size() != 2)) {
        return {};
    }

    QRectF geometry;
    if (type == PDFCommentAnnotation::DrawingType::Square
        || type == PDFCommentAnnotation::DrawingType::Circle) {
        geometry = QRectF(
            normalizedVertices.constFirst(),
            normalizedVertices.constLast()).normalized();
        if (geometry.width() * pageSize.width() < kMinimumShapeDragPoints
            || geometry.height() * pageSize.height()
                < kMinimumShapeDragPoints) {
            return {};
        }
        normalizedVertices = {geometry.topLeft(), geometry.bottomRight()};
    } else {
        qreal minimumX = normalizedVertices.constFirst().x();
        qreal maximumX = minimumX;
        qreal minimumY = normalizedVertices.constFirst().y();
        qreal maximumY = minimumY;
        qreal pathLength = 0.0;
        qreal doubledArea = 0.0;
        for (qsizetype index = 0; index < normalizedVertices.size(); ++index) {
            const QPointF point = normalizedVertices.at(index);
            minimumX = (std::min)(minimumX, point.x());
            maximumX = (std::max)(maximumX, point.x());
            minimumY = (std::min)(minimumY, point.y());
            maximumY = (std::max)(maximumY, point.y());
            if (index > 0) {
                const QPointF previous = normalizedVertices.at(index - 1);
                pathLength += std::hypot(
                    (point.x() - previous.x()) * pageSize.width(),
                    (point.y() - previous.y()) * pageSize.height());
            }
            if (type == PDFCommentAnnotation::DrawingType::Polygon) {
                const QPointF next = normalizedVertices.at(
                    (index + 1) % normalizedVertices.size());
                doubledArea += point.x() * pageSize.width()
                        * next.y() * pageSize.height()
                    - next.x() * pageSize.width()
                        * point.y() * pageSize.height();
            }
        }
        if ((type == PDFCommentAnnotation::DrawingType::Ink
                 ? inkPathLengthPoints < kMinimumShapeDragPoints
                 : pathLength < kMinimumShapeDragPoints)
            || (type == PDFCommentAnnotation::DrawingType::Polygon
                && qAbs(doubledArea) < kMinimumShapeDragPoints * 2.0)) {
            return {};
        }
        geometry = QRectF(
            QPointF(minimumX, minimumY),
            QPointF(maximumX, maximumY)).normalized();
        const qreal horizontalPadding
            = kDrawingCommentBorderWidthPoints / pageSize.width();
        const qreal verticalPadding
            = kDrawingCommentBorderWidthPoints / pageSize.height();
        geometry = geometry.adjusted(
            -horizontalPadding,
            -verticalPadding,
            horizontalPadding,
            verticalPadding).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.drawingType = type;
    candidate.kind = drawingKind(type);
    candidate.objectId = candidate.kind + QLatin1Char(':')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.commentThreadId = candidate.objectId;
    candidate.label = drawingLabel(type);
    candidate.editable = true;
    candidate.drawingPropertiesEditable = true;
    candidate.added = true;
    candidate.commentAuthor = currentCommentAuthor();
    candidate.commentCreationDate = QDateTime::currentDateTime();
    if (type == PDFCommentAnnotation::DrawingType::Ink) {
        candidate.inkStrokes.append(normalizedVertices);
    } else {
        candidate.drawingVertices = normalizedVertices;
    }
    candidate.commentProperties.flags = FPDF_ANNOT_FLAG_PRINT;
    candidate.commentProperties.subject = candidate.label;
    candidate.commentProperties.strokeColor = QColor(
        m_drawingCommentColor.red(),
        m_drawingCommentColor.green(),
        m_drawingCommentColor.blue());
    candidate.commentProperties.opacity
        = m_drawingCommentColor.alphaF();
    candidate.commentProperties.borderWidth
        = kDrawingCommentBorderWidthPoints;
    candidate.highlightColor = m_drawingCommentColor;
    setInitialGeometry(candidate, geometry);

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(CommentSelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QVariantMap PDFEditController::addCommentReply(
    int page,
    const QString &parentObjectId,
    const QString &text)
{
    const QString normalizedText = normalizedCommentText(text);
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()
        || parentObjectId.isEmpty()
        || normalizedText.trimmed().isEmpty()) {
        return {};
    }
    if (normalizedText.size() > kMaximumCommentCharacters) {
        //% "Comments and replies can contain at most %1 characters."
        emit statusMessage(
            qtTrId("CommentTextTooLong")
                .arg(kMaximumCommentCharacters));
        return {};
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return {};
    }

    Candidate parentCandidate;
    const Candidate *pageCandidate = findCandidate(page, parentObjectId);
    if (pageCandidate
        && isCommentCandidateKind(pageCandidate->kind)
        && !pageCandidate->commentStateAnnotation) {
        parentCandidate = *pageCandidate;
    } else {
        const auto indexedPage = m_indexedCommentCandidates.constFind(page);
        if (indexedPage == m_indexedCommentCandidates.cend()) {
            return {};
        }
        const auto indexedCandidate = std::find_if(
            indexedPage->cbegin(),
            indexedPage->cend(),
            [&parentObjectId](const Candidate &candidate) {
                return candidate.objectId == parentObjectId
                    && isCommentCandidateKind(candidate.kind)
                    && !candidate.commentStateAnnotation;
            });
        if (indexedCandidate == indexedPage->cend()) {
            return {};
        }
        parentCandidate = *indexedCandidate;
    }
    if (!parentCandidate.replyToObjectId.isEmpty()) {
        return {};
    }
    if (parentCandidate.commentTextReadOnly) {
        //% "This comment is read-only."
        emit statusMessage(qtTrId("ReadOnlyCommentCannotBeEdited"));
        return {};
    }
    const auto parentDraft = m_drafts.constFind(
        draftKey(page, parentObjectId));
    if (parentDraft != m_drafts.cend()
        && draftRepresentsDeletion(
            parentCandidate.kind,
            parentDraft->text,
            parentDraft->deleted)) {
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }
    const qreal noteWidth = qMin<qreal>(
        1.0,
        kStickyNoteSizePoints / pageSize.width());
    const qreal noteHeight = qMin<qreal>(
        1.0,
        kStickyNoteSizePoints / pageSize.height());
    int existingReplyCount = 0;
    QString parentThreadId = parentCandidate.commentThreadId;
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page == page
            && candidate.replyToObjectId == parentObjectId) {
            ++existingReplyCount;
            if (parentThreadId.isEmpty()
                && !candidate.replyToThreadId.isEmpty()) {
                parentThreadId = candidate.replyToThreadId;
            }
        }
    }
    const auto indexedPage = m_indexedCommentCandidates.constFind(page);
    if (indexedPage != m_indexedCommentCandidates.cend()) {
        for (const Candidate &candidate : indexedPage.value()) {
            if (candidate.replyToObjectId == parentObjectId) {
                ++existingReplyCount;
            }
        }
    }
    if (parentThreadId.isEmpty()) {
        parentThreadId = QStringLiteral("trepdf-thread:")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const qreal offsetPoints = qMin<qreal>(
        18.0,
        4.0 * (existingReplyCount + 1));
    const QRectF parentRect = parentCandidate.geometry.sourceRect.normalized();
    const qreal left = qBound<qreal>(
        0.0,
        parentRect.left() + offsetPoints / pageSize.width(),
        1.0 - noteWidth);
    const qreal top = qBound<qreal>(
        0.0,
        parentRect.top() + offsetPoints / pageSize.height(),
        1.0 - noteHeight);

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("commentReply:")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.kind = QStringLiteral("stickyNote");
    //% "Reply"
    candidate.label = qtTrId("Reply");
    candidate.text = normalizedText;
    candidate.editable = true;
    candidate.added = true;
    candidate.commentAuthor = currentCommentAuthor();
    candidate.commentCreationDate = QDateTime::currentDateTime();
    candidate.replyToObjectId = parentObjectId;
    candidate.replyToThreadId = parentThreadId;
    candidate.isReplyAnnotation = true;
    candidate.replyToSourceAnnotationIndex
        = parentCandidate.added
        ? -1
        : parentCandidate.sourceAnnotationIndex;
    candidate.replyDirectParentSourceAnnotationIndex
        = candidate.replyToSourceAnnotationIndex;
    candidate.replyToParentAdded = parentCandidate.added;
    candidate.highlightColor = parentCandidate.highlightColor.isValid()
        ? parentCandidate.highlightColor
        : QColor(kHighlightRed, kHighlightGreen, kHighlightBlue);
    setInitialGeometry(candidate, QRectF(left, top, noteWidth, noteHeight));

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(CommentSelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    QVariantMap map = candidateMap(draft);
    map.insert(QStringLiteral("page"), page);
    return map;
}

bool PDFEditController::selectComment(int page, const QString &objectId)
{
    const Candidate *candidate = findCandidate(page, objectId);
    if (candidate
        && isCommentCandidateKind(candidate->kind)
        && !candidate->commentStateAnnotation) {
        return selectCandidate(page, objectId);
    }

    const auto indexedPage = m_indexedCommentCandidates.constFind(page);
    if (indexedPage == m_indexedCommentCandidates.cend()) {
        return false;
    }
    for (const Candidate &indexedCandidate : indexedPage.value()) {
        if (indexedCandidate.objectId != objectId) {
            continue;
        }
        if (indexedCandidate.commentStateAnnotation) {
            return false;
        }
        setSelection(&indexedCandidate);
        if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        } else if (!indexedCandidate.reason.isEmpty()) {
            emit statusMessage(indexedCandidate.reason);
        }
        return true;
    }
    return false;
}

bool PDFEditController::removeComment(int page, const QString &objectId)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()
        || objectId.isEmpty()) {
        return false;
    }

    Candidate candidate;
    const Candidate *pageCandidate = findCandidate(page, objectId);
    if (pageCandidate
        && isCommentCandidateKind(pageCandidate->kind)
        && !pageCandidate->commentStateAnnotation) {
        candidate = *pageCandidate;
    } else {
        const auto indexedPage = m_indexedCommentCandidates.constFind(page);
        if (indexedPage == m_indexedCommentCandidates.cend()) {
            return false;
        }
        const auto indexedCandidate = std::find_if(
            indexedPage->cbegin(),
            indexedPage->cend(),
            [&objectId](const Candidate &current) {
                return current.objectId == objectId
                    && isCommentCandidateKind(current.kind)
                    && !current.commentStateAnnotation;
            });
        if (indexedCandidate == indexedPage->cend()) {
            return false;
        }
        candidate = *indexedCandidate;
    }
    return removeCommentCandidate(candidate);
}

int PDFEditController::selectRelativeComment(int direction)
{
    const QVariantList comments = commentItems();
    if (comments.isEmpty() || direction == 0) {
        return -1;
    }

    qsizetype currentIndex = -1;
    for (qsizetype index = 0; index < comments.size(); ++index) {
        const QVariantMap item = comments.at(index).toMap();
        if (m_hasSelection
            && item.value(QStringLiteral("page")).toInt()
                == m_selectedCandidate.page
            && item.value(QStringLiteral("objectId")).toString()
                == m_selectedCandidate.objectId) {
            currentIndex = index;
            break;
        }
    }

    const qsizetype step = direction < 0 ? -1 : 1;
    const qsizetype targetIndex = currentIndex < 0
        ? step > 0 ? 0 : comments.size() - 1
        : (currentIndex + step + comments.size()) % comments.size();
    const QVariantMap target = comments.at(targetIndex).toMap();
    const int page = target.value(QStringLiteral("page"), -1).toInt();
    if (page < 0
        || !selectComment(
            page,
            target.value(QStringLiteral("objectId")).toString())) {
        return -1;
    }
    return page;
}

bool PDFEditController::setCommentText(int page,
                                       const QString &objectId,
                                       const QString &text)
{
    if (m_saving || page < 0 || objectId.isEmpty()) {
        return false;
    }

    const Candidate *candidatePointer = findCommentCandidate(page, objectId);
    if (!candidatePointer) {
        return false;
    }
    const Candidate candidate = *candidatePointer;
    if (!isCommentCandidateKind(candidate.kind)
        || candidate.commentTextReadOnly
        || (!candidate.added && candidate.sourceAnnotationIndex < 0)) {
        return false;
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return false;
    }

    const QString key = draftKey(page, objectId);
    const auto existingDraft = m_drafts.constFind(key);
    if (existingDraft != m_drafts.cend()
        && draftRepresentsDeletion(
            candidate.kind,
            existingDraft->text,
            existingDraft->deleted)) {
        return false;
    }
    const bool hadBefore = existingDraft != m_drafts.cend();
    const Draft before = hadBefore
        ? *existingDraft
        : initialDraftForCandidate(candidate);
    const QString normalizedText = normalizedCommentText(text);
    if (before.text == normalizedText) {
        return true;
    }
    if (normalizedText.size() > kMaximumCommentCharacters) {
        //% "Comments and replies can contain at most %1 characters."
        emit statusMessage(
            qtTrId("CommentTextTooLong")
                .arg(kMaximumCommentCharacters));
        return false;
    }

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = objectId;
    command.hadBefore = hadBefore;
    command.before = before;
    command.after = command.before;
    setDraftText(command.after, normalizedText);
    command.after.candidate.commentModifiedDate
        = QDateTime::currentDateTime();
    command.hadAfter = candidate.added
        || !sameDraftContent(
            command.after,
            initialDraftForCandidate(candidate));
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}

bool PDFEditController::setCommentStatus(int page,
                                         const QString &objectId,
                                         CommentStatus status)
{
    if (m_saving
        || page < 0
        || objectId.isEmpty()
        || status < NoCommentStatus
        || status > FutureCommentStatus) {
        return false;
    }

    const Candidate *candidatePointer = findCommentCandidate(page, objectId);
    if (!candidatePointer) {
        return false;
    }
    const Candidate candidate = *candidatePointer;
    if (!commentStateEditable(candidate)) {
        if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        } else {
            //% "This comment is read-only."
            emit statusMessage(qtTrId("ReadOnlyCommentCannotBeEdited"));
        }
        return false;
    }

    const QString key = draftKey(page, objectId);
    const auto existingDraft = m_drafts.constFind(key);
    if (existingDraft != m_drafts.cend()
        && draftRepresentsDeletion(
            candidate.kind,
            existingDraft->text,
            existingDraft->deleted)) {
        return false;
    }
    const bool hadBefore = existingDraft != m_drafts.cend();
    const Draft before = hadBefore
        ? *existingDraft
        : initialDraftForCandidate(candidate);
    if (before.commentStatus == status) {
        return true;
    }

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = objectId;
    command.hadBefore = hadBefore;
    command.before = before;
    command.after = before;
    command.after.commentStatus = status;
    const QString stateAuthor = canonicalCommentStateAuthor(
        currentCommentAuthor());
    if (!stateAuthor.isEmpty()) {
        command.after.commentStateAuthor = stateAuthor;
    }
    command.after.commentStateModifiedDate = QDateTime::currentDateTime();
    if (command.after.candidate.commentThreadId.isEmpty()
        || (command.after.candidate.isReplyAnnotation
            && command.after.candidate.commentThreadId
                == command.after.candidate.replyToThreadId)) {
        command.after.candidate.commentThreadId
            = QStringLiteral("trepdf-thread:")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    command.hadAfter = candidate.added
        || !sameDraftContent(
            command.after,
            initialDraftForCandidate(candidate));
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}

bool PDFEditController::setCommentMarked(int page,
                                         const QString &objectId,
                                         bool marked)
{
    if (m_saving || page < 0 || objectId.isEmpty()) {
        return false;
    }

    const Candidate *candidatePointer = findCommentCandidate(page, objectId);
    if (!candidatePointer) {
        return false;
    }
    const Candidate candidate = *candidatePointer;
    if (!commentStateEditable(candidate)) {
        if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        } else {
            //% "This comment is read-only."
            emit statusMessage(qtTrId("ReadOnlyCommentCannotBeEdited"));
        }
        return false;
    }

    const QString key = draftKey(page, objectId);
    const auto existingDraft = m_drafts.constFind(key);
    if (existingDraft != m_drafts.cend()
        && draftRepresentsDeletion(
            candidate.kind,
            existingDraft->text,
            existingDraft->deleted)) {
        return false;
    }
    const bool hadBefore = existingDraft != m_drafts.cend();
    const Draft before = hadBefore
        ? *existingDraft
        : initialDraftForCandidate(candidate);
    if (before.commentMarked == marked) {
        return true;
    }

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = objectId;
    command.hadBefore = hadBefore;
    command.before = before;
    command.after = before;
    command.after.commentMarked = marked;
    const QString stateAuthor = canonicalCommentStateAuthor(
        currentCommentAuthor());
    if (!stateAuthor.isEmpty()) {
        command.after.commentStateAuthor = stateAuthor;
    }
    command.after.commentStateModifiedDate = QDateTime::currentDateTime();
    if (command.after.candidate.commentThreadId.isEmpty()
        || (command.after.candidate.isReplyAnnotation
            && command.after.candidate.commentThreadId
                == command.after.candidate.replyToThreadId)) {
        command.after.candidate.commentThreadId
            = QStringLiteral("trepdf-thread:")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    command.hadAfter = candidate.added
        || !sameDraftContent(
            command.after,
            initialDraftForCandidate(candidate));
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}

void PDFEditController::setSelectedCommentText(const QString &text)
{
    if (!m_hasSelection) {
        return;
    }
    setCommentText(
        m_selectedCandidate.page,
        m_selectedCandidate.objectId,
        text);
}

bool PDFEditController::updateSelectedDrawingProperties(
    const std::function<void(
        PDFCommentAnnotation::CommonProperties &)> &update)
{
    if (m_saving
        || !m_hasSelection
        || !drawingPropertiesEditable(m_selectedCandidate)) {
        return false;
    }

    const QString key = draftKey(
        m_selectedCandidate.page,
        m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool hadBefore = existingDraft != m_drafts.cend();
    const Draft before = hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    if (before.deleted) {
        return false;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = hadBefore;
    command.before = before;
    command.after = before;
    update(command.after.commentProperties);
    if (command.after.commentProperties == before.commentProperties) {
        return true;
    }

    if (isDrawingCommentCandidateKind(m_selectedCandidate.kind)) {
        command.after.candidate.commentModifiedDate
            = QDateTime::currentDateTime();
    }
    command.hadAfter = m_selectedCandidate.added
        || !sameDraftContent(
            command.after,
            initialDraftForCandidate(m_selectedCandidate));
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}

bool PDFEditController::setSelectedDrawingStrokeColor(const QColor &color)
{
    if (!color.isValid()) {
        return false;
    }
    QColor normalized = color.toRgb();
    normalized.setAlpha(255);
    return updateSelectedDrawingProperties(
        [&normalized](PDFCommentAnnotation::CommonProperties &properties) {
            properties.strokeColor = normalized;
        });
}

bool PDFEditController::setSelectedDrawingInteriorColor(const QColor &color)
{
    if (!m_hasSelection
        || !selectedDrawingSupportsFill()
        || !color.isValid()) {
        return false;
    }
    QColor normalized = color.toRgb();
    normalized.setAlpha(255);
    return updateSelectedDrawingProperties(
        [&normalized](PDFCommentAnnotation::CommonProperties &properties) {
            properties.interiorColor = normalized;
        });
}

bool PDFEditController::clearSelectedDrawingInteriorColor()
{
    if (!m_hasSelection
        || !selectedDrawingSupportsFill()) {
        return false;
    }
    return updateSelectedDrawingProperties(
        [](PDFCommentAnnotation::CommonProperties &properties) {
            properties.interiorColor = QColor();
        });
}

bool PDFEditController::setSelectedDrawingOpacity(qreal opacity)
{
    if (!std::isfinite(opacity)) {
        return false;
    }
    const qreal normalized = qBound<qreal>(0.0, opacity, 1.0);
    return updateSelectedDrawingProperties(
        [normalized](PDFCommentAnnotation::CommonProperties &properties) {
            properties.opacity = normalized;
        });
}

bool PDFEditController::removeCommentCandidate(const Candidate &candidate)
{
    if (!isCommentCandidateKind(candidate.kind)
        || candidate.page < 0
        || candidate.objectId.isEmpty()) {
        return false;
    }
    if (candidate.commentTextReadOnly) {
        //% "This comment is read-only."
        emit statusMessage(qtTrId("ReadOnlyCommentCannotBeEdited"));
        return false;
    }
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return false;
    }
    if (!candidateDeletable(candidate)) {
        return false;
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    const QString key = draftKey(candidate.page, candidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);

    Command command;
    command.key = key;
    command.page = candidate.page;
    command.objectId = candidate.objectId;
    if (candidate.added) {
        if (existingDraft == m_drafts.cend()) {
            return false;
        }
        command.hadBefore = true;
        command.before = *existingDraft;
        command.hadAfter = false;
        command.after = command.before;
    } else {
        command.hadBefore = existingDraft != m_drafts.cend();
        command.before = command.hadBefore
            ? *existingDraft
            : initialDraftForCandidate(candidate);
        if (command.before.deleted) {
            return false;
        }
        command.hadAfter = true;
        command.after = command.before;
        command.after.deleted = true;
    }

    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}

bool PDFEditController::commentReplyIsVisible(const Candidate &candidate) const
{
    if (candidate.replyToObjectId.isEmpty()) {
        return true;
    }

    const auto parentDraft = m_drafts.constFind(
        draftKey(candidate.page, candidate.replyToObjectId));
    if (parentDraft != m_drafts.cend()
        && draftRepresentsDeletion(
            parentDraft->candidate.kind,
            parentDraft->text,
            parentDraft->deleted)) {
        return false;
    }
    int parentAnnotationIndex
        = candidate.replyDirectParentSourceAnnotationIndex;
    QSet<int> visitedAnnotationIndices;
    const auto sourceParent = [this, &candidate](int annotationIndex)
        -> const Candidate * {
        const auto candidateForIndex = [annotationIndex](
                                           const QVector<Candidate> &items)
            -> const Candidate * {
            const auto match = std::find_if(
                items.cbegin(),
                items.cend(),
                [annotationIndex](const Candidate &current) {
                    return current.sourceAnnotationIndex == annotationIndex
                        && isCommentCandidateKind(current.kind);
                });
            return match == items.cend() ? nullptr : &*match;
        };
        const auto indexedPage = m_indexedCommentCandidates.constFind(
            candidate.page);
        if (indexedPage != m_indexedCommentCandidates.cend()) {
            if (const Candidate *parent = candidateForIndex(*indexedPage)) {
                return parent;
            }
        }
        const auto cachedPage = m_pageCandidates.constFind(candidate.page);
        return cachedPage == m_pageCandidates.cend()
            ? nullptr
            : candidateForIndex(*cachedPage);
    };
    while (parentAnnotationIndex >= 0
           && !visitedAnnotationIndices.contains(parentAnnotationIndex)) {
        visitedAnnotationIndices.insert(parentAnnotationIndex);
        const Candidate *parentCandidate = sourceParent(
            parentAnnotationIndex);
        if (!parentCandidate) {
            break;
        }
        const auto ancestorDraft = m_drafts.constFind(
            draftKey(candidate.page, parentCandidate->objectId));
        if (ancestorDraft != m_drafts.cend()
            && draftRepresentsDeletion(
                ancestorDraft->candidate.kind,
                ancestorDraft->text,
                ancestorDraft->deleted)) {
            return false;
        }
        parentAnnotationIndex
            = parentCandidate->replyDirectParentSourceAnnotationIndex;
    }
    if (!candidate.replyToParentAdded) {
        return true;
    }
    return std::any_of(
        m_addedCandidates.cbegin(),
        m_addedCandidates.cend(),
        [&candidate](const Candidate &parent) {
            return parent.page == candidate.page
                && parent.objectId == candidate.replyToObjectId
                && parent.replyToObjectId.isEmpty();
        });
}

QVector<QRectF> PDFEditController::highlightRectsForDraft(
    const Draft &draft)
{
    QVector<QRectF> rects = draft.candidate.highlightRects;
    if (!isTextMarkupCandidateKind(draft.candidate.kind)
        || rects.isEmpty()) {
        return rects;
    }

    const QRectF initialFrameRect = (
        draft.candidate.initialDraftFrameRect.isEmpty()
            ? draft.candidate.geometry.sourceRect
            : draft.candidate.initialDraftFrameRect).normalized();
    const QRectF currentFrameRect = draft.frameRect.normalized();
    if (currentFrameRect == initialFrameRect) {
        return rects;
    }
    if (!initialFrameRect.isValid()
        || initialFrameRect.width() <= 0.0
        || initialFrameRect.height() <= 0.0
        || !currentFrameRect.isValid()
        || currentFrameRect.width() <= 0.0
        || currentFrameRect.height() <= 0.0) {
        return {};
    }

    const qreal horizontalScale
        = currentFrameRect.width() / initialFrameRect.width();
    const qreal verticalScale
        = currentFrameRect.height() / initialFrameRect.height();
    const auto transformedPoint = [
        &initialFrameRect,
        &currentFrameRect,
        horizontalScale,
        verticalScale](const QPointF &point) {
        return QPointF(
            currentFrameRect.left()
                + (point.x() - initialFrameRect.left()) * horizontalScale,
            currentFrameRect.top()
                + (point.y() - initialFrameRect.top()) * verticalScale);
    };
    for (QRectF &rect : rects) {
        const QRectF bounds = rect.normalized();
        rect = QRectF(
            transformedPoint(bounds.topLeft()),
            transformedPoint(bounds.bottomRight())).normalized();
    }
    return rects;
}

QVector<std::array<QPointF, 4>>
PDFEditController::textMarkupQuadsForDraft(const Draft &draft)
{
    QVector<std::array<QPointF, 4>> quads
        = draft.candidate.textMarkupQuads;
    if (!isTextMarkupCandidateKind(draft.candidate.kind)) {
        return quads;
    }
    if (quads.isEmpty()) {
        quads.reserve(draft.candidate.highlightRects.size());
        for (const QRectF &rect : draft.candidate.highlightRects) {
            const QRectF bounds = rect.normalized();
            quads.append(std::array<QPointF, 4>{
                bounds.topLeft(),
                bounds.topRight(),
                bounds.bottomLeft(),
                bounds.bottomRight()});
        }
    }
    if (quads.isEmpty()) {
        return quads;
    }

    const QRectF initialFrameRect = (
        draft.candidate.initialDraftFrameRect.isEmpty()
            ? draft.candidate.geometry.sourceRect
            : draft.candidate.initialDraftFrameRect).normalized();
    const QRectF currentFrameRect = draft.frameRect.normalized();
    if (currentFrameRect == initialFrameRect) {
        return quads;
    }
    if (!initialFrameRect.isValid()
        || initialFrameRect.width() <= 0.0
        || initialFrameRect.height() <= 0.0
        || !currentFrameRect.isValid()
        || currentFrameRect.width() <= 0.0
        || currentFrameRect.height() <= 0.0) {
        return {};
    }

    const qreal horizontalScale
        = currentFrameRect.width() / initialFrameRect.width();
    const qreal verticalScale
        = currentFrameRect.height() / initialFrameRect.height();
    for (std::array<QPointF, 4> &quad : quads) {
        for (QPointF &point : quad) {
            point = QPointF(
                currentFrameRect.left()
                    + (point.x() - initialFrameRect.left())
                        * horizontalScale,
                currentFrameRect.top()
                    + (point.y() - initialFrameRect.top())
                        * verticalScale);
        }
    }
    return quads;
}

QVector<QPointF> PDFEditController::drawingVerticesForDraft(
    const Draft &draft)
{
    QVector<QPointF> vertices = draft.candidate.drawingVertices;
    if (!isDrawingCommentCandidateKind(draft.candidate.kind)
        || draft.candidate.kind == QLatin1String("ink")
        || vertices.isEmpty()) {
        return vertices;
    }

    const QRectF initialFrameRect = (
        draft.candidate.initialDraftFrameRect.isEmpty()
        ? draft.candidate.geometry.sourceRect
        : draft.candidate.initialDraftFrameRect).normalized();
    const QRectF currentFrameRect = draft.frameRect.normalized();
    if (currentFrameRect == initialFrameRect) {
        return vertices;
    }
    if (!initialFrameRect.isValid()
        || initialFrameRect.width() <= 0.0
        || initialFrameRect.height() <= 0.0
        || !currentFrameRect.isValid()
        || currentFrameRect.width() <= 0.0
        || currentFrameRect.height() <= 0.0) {
        return {};
    }

    const qreal horizontalScale
        = currentFrameRect.width() / initialFrameRect.width();
    const qreal verticalScale
        = currentFrameRect.height() / initialFrameRect.height();
    for (QPointF &point : vertices) {
        point = QPointF(
            currentFrameRect.left()
                + (point.x() - initialFrameRect.left()) * horizontalScale,
            currentFrameRect.top()
                + (point.y() - initialFrameRect.top()) * verticalScale);
    }
    return vertices;
}

QVector<QVector<QPointF>> PDFEditController::inkStrokesForDraft(
    const Draft &draft)
{
    QVector<QVector<QPointF>> strokes = draft.candidate.inkStrokes;
    if (draft.candidate.kind != QLatin1String("ink")
        || strokes.isEmpty()) {
        return strokes;
    }

    const QRectF initialFrameRect = (
        draft.candidate.initialDraftFrameRect.isEmpty()
        ? draft.candidate.geometry.sourceRect
        : draft.candidate.initialDraftFrameRect).normalized();
    const QRectF currentFrameRect = draft.frameRect.normalized();
    if (currentFrameRect == initialFrameRect) {
        return strokes;
    }
    if (!initialFrameRect.isValid()
        || initialFrameRect.width() <= 0.0
        || initialFrameRect.height() <= 0.0
        || !currentFrameRect.isValid()
        || currentFrameRect.width() <= 0.0
        || currentFrameRect.height() <= 0.0) {
        return {};
    }

    const qreal horizontalScale
        = currentFrameRect.width() / initialFrameRect.width();
    const qreal verticalScale
        = currentFrameRect.height() / initialFrameRect.height();

    for (QVector<QPointF> &stroke : strokes) {
        for (QPointF &point : stroke) {
            point = QPointF(
                currentFrameRect.left()
                    + (point.x() - initialFrameRect.left())
                        * horizontalScale,
                currentFrameRect.top()
                    + (point.y() - initialFrameRect.top())
                        * verticalScale);
        }
    }
    return strokes;
}
