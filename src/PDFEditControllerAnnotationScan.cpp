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

constexpr unsigned long kMaximumSourceInkStrokes = 64;

constexpr unsigned long kMaximumSourceInkPointsPerStroke = 4096;

constexpr unsigned long kMaximumSourceInkPoints = 32768;

constexpr size_t kMaximumSourceTextMarkupRects = 50000;

constexpr size_t kMaximumEditableSourceTextMarkupQuads = 4096;

constexpr char kCommentThreadIdKey[] = "TrePDFThreadId";

constexpr char kCommentReplyToKey[] = "TrePDFReplyTo";

PDFCommentAnnotation::DrawingType drawingTypeForSubtype(int subtype)
{
    switch (subtype) {
    case FPDF_ANNOT_CIRCLE:
        return PDFCommentAnnotation::DrawingType::Circle;
    case FPDF_ANNOT_LINE:
        return PDFCommentAnnotation::DrawingType::Line;
    case FPDF_ANNOT_POLYGON:
        return PDFCommentAnnotation::DrawingType::Polygon;
    case FPDF_ANNOT_POLYLINE:
        return PDFCommentAnnotation::DrawingType::PolyLine;
    case FPDF_ANNOT_INK:
        return PDFCommentAnnotation::DrawingType::Ink;
    default:
        return PDFCommentAnnotation::DrawingType::Square;
    }
}

int rootReplyAnnotationIndex(FPDF_PAGE page,
                             FPDF_ANNOTATION annotation,
                             int annotationIndex,
                             int &directParentIndex,
                             QString &threadId)
{
    directParentIndex = -1;
    threadId.clear();
    if (!page
        || !annotation
        || !annotationUsesReplyRelation(annotation)) {
        return -1;
    }

    FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(annotation, "IRT");
    if (!parent) {
        return -1;
    }

    QSet<int> visitedIndices{annotationIndex};
    int rootIndex = -1;
    while (parent) {
        const int parentIndex = FPDFPage_GetAnnotIndex(page, parent);
        const bool parentIsMarkup = isMarkupAnnotation(parent);
        if (parentIndex < 0
            || visitedIndices.contains(parentIndex)
            || !parentIsMarkup) {
            FPDFPage_CloseAnnot(parent);
            return -1;
        }
        visitedIndices.insert(parentIndex);
        if (directParentIndex < 0) {
            directParentIndex = parentIndex;
        }
        rootIndex = parentIndex;
        threadId = annotationString(parent, kCommentThreadIdKey).trimmed();

        FPDF_ANNOTATION nextParent = FPDFAnnot_GetLinkedAnnot(parent, "IRT");
        const bool parentIsReply = nextParent
            && annotationUsesReplyRelation(parent);
        FPDFPage_CloseAnnot(parent);
        if (!parentIsReply) {
            if (nextParent) {
                FPDFPage_CloseAnnot(nextParent);
            }
            if (!parentIsMarkup) {
                return -1;
            }
            break;
        }
        parent = nextParent;
    }
    return rootIndex;
}

bool containsOnlyAsciiDigits(const QString &value)
{
    for (const QChar character : value) {
        if (character < QLatin1Char('0')
            || character > QLatin1Char('9')) {
            return false;
        }
    }
    return !value.isEmpty();
}

QDateTime pdfAnnotationDateTime(const QString &rawValue)
{
    const QString trimmedValue = rawValue.trimmed();
    const QDateTime isoDateTime = QDateTime::fromString(
        trimmedValue,
        Qt::ISODate);
    if (isoDateTime.isValid()) {
        return isoDateTime;
    }

    QString value = trimmedValue;
    if (value.startsWith(QLatin1String("D:"))) {
        value.remove(0, 2);
    }

    qsizetype digitCount = 0;
    while (digitCount < value.size()
           && digitCount < 14
           && value.at(digitCount) >= QLatin1Char('0')
           && value.at(digitCount) <= QLatin1Char('9')) {
        ++digitCount;
    }
    if (digitCount < 4
        || digitCount % 2 != 0
        || (digitCount == 14
            && digitCount < value.size()
            && value.at(digitCount) >= QLatin1Char('0')
            && value.at(digitCount) <= QLatin1Char('9'))) {
        return {};
    }

    const auto component = [&value, digitCount](
                               qsizetype offset,
                               int fallback) {
        if (digitCount < offset + 2) {
            return fallback;
        }
        bool parsed = false;
        const int result = value.mid(offset, 2).toInt(&parsed);
        return parsed ? result : -1;
    };
    bool parsedYear = false;
    const int year = value.left(4).toInt(&parsedYear);
    const int month = component(4, 1);
    const int day = component(6, 1);
    const int hour = component(8, 0);
    const int minute = component(10, 0);
    const int second = component(12, 0);
    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!parsedYear || !date.isValid() || !time.isValid()) {
        return {};
    }

    QString timeZoneSuffix = value.mid(digitCount).trimmed();
    if (timeZoneSuffix.isEmpty()) {
        return QDateTime(date, time);
    }
    if (timeZoneSuffix == QLatin1String("Z")) {
        return QDateTime(date, time, QTimeZone::UTC);
    }
    if (!timeZoneSuffix.startsWith(QLatin1Char('+'))
        && !timeZoneSuffix.startsWith(QLatin1Char('-'))) {
        return {};
    }

    const int direction = timeZoneSuffix.startsWith(QLatin1Char('-'))
        ? -1
        : 1;
    timeZoneSuffix.remove(0, 1);
    timeZoneSuffix.remove(QLatin1Char('\''));
    timeZoneSuffix.remove(QLatin1Char(':'));
    if ((timeZoneSuffix.size() != 2 && timeZoneSuffix.size() != 4)
        || !containsOnlyAsciiDigits(timeZoneSuffix)) {
        return {};
    }
    const int offsetHours = timeZoneSuffix.left(2).toInt();
    const int offsetMinutes = timeZoneSuffix.size() == 4
        ? timeZoneSuffix.mid(2, 2).toInt()
        : 0;
    if (offsetHours > 23 || offsetMinutes > 59) {
        return {};
    }
    const int offsetSeconds = direction
        * (offsetHours * 60 * 60 + offsetMinutes * 60);
    return QDateTime(
        date,
        time,
        QTimeZone::fromSecondsAheadOfUtc(offsetSeconds));
}

struct AnnotationAppearanceStyle {
    QColor strokeColor;
    QColor fillColor;
    qreal strokeWidth = 0.0;
    QVector<qreal> dashPattern;
    bool hasStroke = false;
    bool hasFill = false;
    bool hasStrokeWidth = false;
    bool dashPatternSupported = true;
};

AnnotationAppearanceStyle annotationAppearanceStyle(
    FPDF_ANNOTATION annotation)
{
    AnnotationAppearanceStyle result;
    const int objectCount = FPDFAnnot_GetObjectCount(annotation);
    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT object = FPDFAnnot_GetObject(annotation, index);
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH) {
            continue;
        }

        int fillMode = FPDF_FILLMODE_NONE;
        FPDF_BOOL stroke = false;
        if (!FPDFPath_GetDrawMode(object, &fillMode, &stroke)) {
            continue;
        }

        unsigned int red = 0;
        unsigned int green = 0;
        unsigned int blue = 0;
        unsigned int alpha = 255;
        if (stroke && !result.hasStroke
            && FPDFPageObj_GetStrokeColor(
                object,
                &red,
                &green,
                &blue,
                &alpha)) {
            result.strokeColor = QColor::fromRgb(red, green, blue, alpha);
            result.hasStroke = true;

            float width = 0.0F;
            if (FPDFPageObj_GetStrokeWidth(object, &width)
                && std::isfinite(width)
                && width > 0.0F) {
                result.strokeWidth = width;
                result.hasStrokeWidth = true;
            }

            const int dashCount = FPDFPageObj_GetDashCount(object);
            if (dashCount < 0 || dashCount > 16) {
                result.dashPatternSupported = false;
            } else if (dashCount > 0) {
                QVector<float> dashes(dashCount);
                if (!FPDFPageObj_GetDashArray(
                        object,
                        dashes.data(),
                        static_cast<size_t>(dashCount))) {
                    result.dashPatternSupported = false;
                } else {
                    QVector<qreal> parsedDashes;
                    parsedDashes.reserve(dashCount);
                    for (float dash : std::as_const(dashes)) {
                        if (!std::isfinite(dash) || dash <= 0.0F) {
                            result.dashPatternSupported = false;
                            parsedDashes.clear();
                            break;
                        }
                        parsedDashes.append(dash);
                    }
                    if (result.dashPatternSupported) {
                        result.dashPattern = std::move(parsedDashes);
                    }
                }
            }
        }
        if (fillMode != FPDF_FILLMODE_NONE
            && !result.hasFill
            && FPDFPageObj_GetFillColor(
                object,
                &red,
                &green,
                &blue,
                &alpha)) {
            result.fillColor = QColor::fromRgb(red, green, blue, alpha);
            result.hasFill = true;
        }
    }
    return result;
}

bool hasCanonicalDrawingAppearance(QStringView appearanceText)
{
    const QString appearance = appearanceText.trimmed().toString();
    return appearance.startsWith(QLatin1String("q\n"))
        && appearance.endsWith(QLatin1Char('Q'))
        && appearance.contains(QLatin1String("/GS gs"))
        && appearance.contains(QLatin1String("1 J 1 j"));
}

QColor annotationColor(FPDF_ANNOTATION annotation,
                       FPDF_ANNOTATION_SUBTYPE subtype,
                       const QColor &fallback,
                       const AnnotationAppearanceStyle *appearanceStyle)
{
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int alpha = 255;
    if (FPDFAnnot_GetColor(
            annotation,
            FPDFANNOT_COLORTYPE_Color,
            &red,
            &green,
            &blue,
            &alpha)) {
        return QColor::fromRgb(red, green, blue, alpha);
    }

    const bool highlight = subtype == FPDF_ANNOT_HIGHLIGHT;
    const bool strokedTextMarkup = subtype == FPDF_ANNOT_UNDERLINE
        || subtype == FPDF_ANNOT_SQUIGGLY
        || subtype == FPDF_ANNOT_STRIKEOUT;
    const bool drawing = subtype == FPDF_ANNOT_SQUARE
        || subtype == FPDF_ANNOT_CIRCLE
        || subtype == FPDF_ANNOT_LINE
        || subtype == FPDF_ANNOT_POLYGON
        || subtype == FPDF_ANNOT_POLYLINE
        || subtype == FPDF_ANNOT_INK;
    if (!highlight && !strokedTextMarkup && !drawing) {
        return fallback;
    }

    AnnotationAppearanceStyle discoveredAppearanceStyle;
    if (!appearanceStyle) {
        discoveredAppearanceStyle = annotationAppearanceStyle(annotation);
        appearanceStyle = &discoveredAppearanceStyle;
    }
    if (highlight && appearanceStyle->hasFill) {
        return appearanceStyle->fillColor;
    }
    if (appearanceStyle->hasStroke) {
        return appearanceStyle->strokeColor;
    }
    if (appearanceStyle->hasFill) {
        return appearanceStyle->fillColor;
    }
    return fallback;
}

} // namespace

bool PDFEditController::appendAnnotationCandidates(FPDF_PAGE page,
                                                   const CandidateScanRequest &request,
                                                   QVector<Candidate> &candidates,
    const std::shared_ptr<std::atomic_bool> &cancellation,
    qsizetype &estimatedCommentBytes,
    bool &commentMemoryBudgetExceeded)
{
    assertPDFiumProcessLockHeld();
    const QSizeF &pageSize = request.pageSize;
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    if (annotationCount < 0) {
        return false;
    }
    bool succeeded = true;
    for (int index = 0;
         index < annotationCount && !candidateScanCancelled(cancellation);
         ++index) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
        if (!annotation) {
            succeeded = false;
            continue;
        }

        const int subtype = FPDFAnnot_GetSubtype(annotation);
        const bool commentStateAnnotation
            = isCommentStateAnnotation(annotation);
        const bool textMarkupSubtype
            = subtype == FPDF_ANNOT_HIGHLIGHT
            || subtype == FPDF_ANNOT_UNDERLINE
            || subtype == FPDF_ANNOT_SQUIGGLY
            || subtype == FPDF_ANNOT_STRIKEOUT;
        const bool stickyNoteSubtype = subtype == FPDF_ANNOT_TEXT;
        const bool freeTextSubtype = subtype == FPDF_ANNOT_FREETEXT;
        const bool stampSubtype = subtype == FPDF_ANNOT_STAMP;
        const bool inkSubtype = subtype == FPDF_ANNOT_INK;
        const bool drawingCommentSubtype
            = subtype == FPDF_ANNOT_SQUARE
            || subtype == FPDF_ANNOT_CIRCLE
            || subtype == FPDF_ANNOT_LINE
            || subtype == FPDF_ANNOT_POLYGON
            || subtype == FPDF_ANNOT_POLYLINE
            || inkSubtype;
        const bool supportedCommentSubtype
            = isSupportedCommentAnnotationSubtype(subtype);
        if (isMarkupAnnotation(annotation)) {
            FPDF_ANNOTATION directReplyParent
                = annotationUsesReplyRelation(annotation)
                ? FPDFAnnot_GetLinkedAnnot(annotation, "IRT")
                : nullptr;
            const bool isReplyAnnotation = directReplyParent != nullptr;
            if (directReplyParent) {
                FPDFPage_CloseAnnot(directReplyParent);
            }
            QString rootThreadId;
            int directParentIndex = -1;
            const int rootReplyIndex = rootReplyAnnotationIndex(
                page,
                annotation,
                index,
                directParentIndex,
                rootThreadId);

            QVector<QRectF> annotationRects;
            QVector<std::array<QPointF, 4>> textMarkupQuads;
            bool textMarkupGeometryValid = !textMarkupSubtype;
            if (textMarkupSubtype) {
                const size_t quadCount
                    = FPDFAnnot_CountAttachmentPoints(annotation);
                textMarkupGeometryValid = quadCount > 0
                    && quadCount <= kMaximumEditableSourceTextMarkupQuads;
                if (textMarkupGeometryValid) {
                    textMarkupQuads.reserve(
                        static_cast<qsizetype>(quadCount));
                }
                if (quadCount <= kMaximumSourceTextMarkupRects) {
                    annotationRects.reserve(
                        static_cast<qsizetype>(quadCount));
                    for (size_t quadIndex = 0;
                         quadIndex < quadCount;
                         ++quadIndex) {
                        FS_QUADPOINTSF quad{};
                        if (!FPDFAnnot_GetAttachmentPoints(
                                annotation,
                                quadIndex,
                                &quad)) {
                            textMarkupGeometryValid = false;
                            continue;
                        }
                        const std::array<QPointF, 4> points{
                            QPointF(quad.x1, quad.y1),
                            QPointF(quad.x2, quad.y2),
                            QPointF(quad.x3, quad.y3),
                            QPointF(quad.x4, quad.y4)};
                        qreal minimumX = points.front().x();
                        qreal maximumX = minimumX;
                        qreal minimumY = points.front().y();
                        qreal maximumY = minimumY;
                        for (const QPointF &point : points) {
                            minimumX = qMin(minimumX, point.x());
                            maximumX = qMax(maximumX, point.x());
                            minimumY = qMin(minimumY, point.y());
                            maximumY = qMax(maximumY, point.y());
                        }
                        const QRectF rect = normalizedRect(
                            page,
                            minimumX,
                            minimumY,
                            maximumX,
                            maximumY,
                            kIdentityMatrix,
                            pageSize);
                        if (!rect.isEmpty()) {
                            annotationRects.append(rect);
                        }
                        if (textMarkupGeometryValid) {
                            std::array<QPointF, 4> normalizedQuad;
                            bool normalized = !rect.isEmpty();
                            for (size_t pointIndex = 0;
                                 pointIndex < points.size() && normalized;
                                 ++pointIndex) {
                                normalized = normalizedPagePoint(
                                    page,
                                    pageSize,
                                    points.at(pointIndex),
                                    normalizedQuad.at(pointIndex));
                            }
                            if (normalized) {
                                textMarkupQuads.append(normalizedQuad);
                            } else {
                                textMarkupGeometryValid = false;
                            }
                        }
                    }
                }
                textMarkupGeometryValid = textMarkupGeometryValid
                    && textMarkupQuads.size()
                        == static_cast<qsizetype>(quadCount)
                    && annotationRects.size()
                        == static_cast<qsizetype>(quadCount);
                if (!textMarkupGeometryValid) {
                    textMarkupQuads.clear();
                }
            }

            if (annotationRects.isEmpty()) {
                FS_RECTF bounds{};
                if (FPDFAnnot_GetRect(annotation, &bounds)) {
                    const QRectF rect = normalizedRect(
                        page,
                        bounds.left,
                        bounds.bottom,
                        bounds.right,
                        bounds.top,
                        kIdentityMatrix,
                        pageSize);
                    if (!rect.isEmpty()) {
                        annotationRects.append(rect);
                    }
                }
            }
            if (annotationRects.isEmpty() && rootReplyIndex < 0) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            QRectF geometry;
            if (!annotationRects.isEmpty()) {
                geometry = annotationRects.constFirst();
                for (qsizetype rectIndex = 1;
                     rectIndex < annotationRects.size();
                     ++rectIndex) {
                    geometry = geometry.united(annotationRects.at(rectIndex));
                }
            }

            QVector<QVector<QPointF>> inkStrokes;
            bool inkGeometryValid = !inkSubtype;
            if (inkSubtype) {
                const unsigned long strokeCount
                    = FPDFAnnot_GetInkListCount(annotation);
                if (strokeCount > 0
                    && strokeCount <= kMaximumSourceInkStrokes) {
                    inkGeometryValid = true;
                    unsigned long totalPointCount = 0;
                    inkStrokes.reserve(static_cast<qsizetype>(strokeCount));
                    for (unsigned long strokeIndex = 0;
                         strokeIndex < strokeCount;
                         ++strokeIndex) {
                        if (candidateScanCancelled(cancellation)) {
                            inkGeometryValid = false;
                            break;
                        }
                        const unsigned long pointCount
                            = FPDFAnnot_GetInkListPath(
                                annotation,
                                strokeIndex,
                                nullptr,
                                0);
                        if (pointCount < 2
                            || pointCount
                                > kMaximumSourceInkPointsPerStroke
                            || pointCount
                                > kMaximumSourceInkPoints
                                    - totalPointCount) {
                            inkGeometryValid = false;
                            break;
                        }

                        QVector<FS_POINTF> pageStroke(
                            static_cast<qsizetype>(pointCount));
                        if (FPDFAnnot_GetInkListPath(
                                annotation,
                                strokeIndex,
                                pageStroke.data(),
                                pointCount)
                            != pointCount) {
                            inkGeometryValid = false;
                            break;
                        }

                        QVector<QPointF> normalizedStroke;
                        normalizedStroke.reserve(pageStroke.size());
                        for (const FS_POINTF &pagePoint
                             : std::as_const(pageStroke)) {
                            QPointF normalizedPoint;
                            if (!std::isfinite(pagePoint.x)
                                || !std::isfinite(pagePoint.y)
                                || !normalizedPagePoint(
                                    page,
                                    pageSize,
                                    QPointF(pagePoint.x, pagePoint.y),
                                    normalizedPoint)
                                || !std::isfinite(normalizedPoint.x())
                                || !std::isfinite(normalizedPoint.y())) {
                                inkGeometryValid = false;
                                break;
                            }
                            if (!normalizedStroke.isEmpty()) {
                                const QPointF previous
                                    = normalizedStroke.constLast();
                                if (std::hypot(
                                        (normalizedPoint.x() - previous.x())
                                            * pageSize.width(),
                                        (normalizedPoint.y() - previous.y())
                                            * pageSize.height())
                                    <= 0.0) {
                                    // Foxit may repeat samples in /InkList.
                                    // They do not change the path and must not
                                    // make an otherwise valid Ink read-only.
                                    continue;
                                }
                            }
                            normalizedStroke.append(normalizedPoint);
                        }
                        if (!inkGeometryValid
                            || normalizedStroke.size() < 2) {
                            inkGeometryValid = false;
                            break;
                        }
                        totalPointCount += pointCount;
                        inkStrokes.append(std::move(normalizedStroke));
                    }
                }
                if (!inkGeometryValid) {
                    inkStrokes.clear();
                }
            }

            Candidate candidate;
            candidate.page = request.page;
            candidate.objectId = QStringLiteral("annotation:")
                + QString::number(index);
            candidate.sourceAnnotationIndex = index;
            candidate.text = normalizedCommentText(
                annotationString(annotation, "Contents"));
            candidate.sourceAnnotationName = annotationString(
                annotation,
                "NM");
            candidate.sourceAnnotationHasName
                = FPDFAnnot_HasKey(annotation, "NM");
            candidate.commentAuthor = annotationString(annotation, "T").trimmed();
            candidate.commentCreationDate = pdfAnnotationDateTime(
                annotationString(annotation, "CreationDate"));
            candidate.commentModifiedDate = pdfAnnotationDateTime(
                annotationString(annotation, "M"));
            candidate.commentThreadId = annotationString(
                annotation,
                kCommentThreadIdKey).trimmed();
            candidate.replyToThreadId = annotationString(
                annotation,
                kCommentReplyToKey).trimmed();
            candidate.isReplyAnnotation = isReplyAnnotation;
            candidate.commentStateAnnotation = commentStateAnnotation;
            candidate.commentState = annotationString(
                annotation,
                "State").trimmed();
            candidate.commentStateModel = annotationString(
                annotation,
                "StateModel").trimmed();
            const int annotationFlags = FPDFAnnot_GetFlags(annotation);
            candidate.commentTextReadOnly
                = !supportedCommentSubtype
                || (annotationFlags & FPDF_ANNOT_FLAG_READONLY) != 0
                || (annotationFlags & kAnnotationFlagLockedContents) != 0;
            candidate.commentProperties.flags = annotationFlags;
            candidate.commentProperties.subject = annotationString(
                annotation,
                "Subj");
            candidate.commentProperties.intent = annotationString(
                annotation,
                "IT");
            candidate.commentProperties.name = annotationString(
                annotation,
                "Name");
            if (stampSubtype) {
                candidate.stampText = stampTextForName(
                    candidate.commentProperties.name);
            }
            if (rootReplyIndex >= 0) {
                candidate.replyToObjectId = QStringLiteral("annotation:")
                    + QString::number(rootReplyIndex);
                candidate.replyToThreadId = rootThreadId;
                candidate.replyToSourceAnnotationIndex = rootReplyIndex;
                candidate.replyDirectParentSourceAnnotationIndex
                    = directParentIndex;
            }
            candidate.limited = true;
            setInitialGeometry(candidate, geometry);
            if (textMarkupSubtype) {
                candidate.highlightRects = annotationRects;
                candidate.textMarkupQuads = std::move(textMarkupQuads);
            }

            QColor defaultColor = stickyNoteSubtype
                ? QColor(kHighlightRed, kHighlightGreen, kHighlightBlue)
                : stampSubtype
                    ? stampColorForName(candidate.commentProperties.name)
                : subtype == FPDF_ANNOT_HIGHLIGHT
                    ? QColor(kHighlightRed,
                             kHighlightGreen,
                             kHighlightBlue,
                             kHighlightAlpha)
                    : QColor(Qt::black);
            const AnnotationAppearanceStyle appearanceStyle
                = drawingCommentSubtype
                ? annotationAppearanceStyle(annotation)
                : AnnotationAppearanceStyle{};
            candidate.highlightColor = annotationColor(
                annotation,
                subtype,
                defaultColor,
                drawingCommentSubtype ? &appearanceStyle : nullptr);
            if (drawingCommentSubtype) {
                candidate.drawingType = drawingTypeForSubtype(subtype);
                candidate.commentProperties.strokeColor = QColor(
                    candidate.highlightColor.red(),
                    candidate.highlightColor.green(),
                    candidate.highlightColor.blue());
                candidate.commentProperties.opacity
                    = candidate.highlightColor.alphaF();
                unsigned int red = 0;
                unsigned int green = 0;
                unsigned int blue = 0;
                unsigned int alpha = 0;
                if (FPDFAnnot_GetColor(
                        annotation,
                        FPDFANNOT_COLORTYPE_InteriorColor,
                        &red,
                        &green,
                        &blue,
                        &alpha)) {
                    candidate.commentProperties.interiorColor = QColor(
                        static_cast<int>((std::min)(red, 255U)),
                        static_cast<int>((std::min)(green, 255U)),
                        static_cast<int>((std::min)(blue, 255U)));
                }
                if (appearanceStyle.hasFill) {
                    candidate.commentProperties.interiorColor
                        = appearanceStyle.fillColor.toRgb();
                    candidate.commentProperties.interiorColor.setAlpha(255);
                }
                float horizontalRadius = 0.0F;
                float verticalRadius = 0.0F;
                float borderWidth = 0.0F;
                if (appearanceStyle.hasStrokeWidth) {
                    candidate.commentProperties.borderWidth
                        = appearanceStyle.strokeWidth;
                } else if (FPDFAnnot_GetBorder(
                        annotation,
                        &horizontalRadius,
                        &verticalRadius,
                        &borderWidth)
                    && std::isfinite(borderWidth)
                    && borderWidth >= 0.0F) {
                    candidate.commentProperties.borderWidth = borderWidth;
                }
                if (!appearanceStyle.dashPattern.isEmpty()) {
                    candidate.commentProperties.borderStyle
                        = PDFCommentAnnotation::BorderStyle::Dashed;
                    candidate.commentProperties.dashPattern
                        = appearanceStyle.dashPattern;
                }

                if (inkSubtype && inkGeometryValid) {
                    candidate.inkStrokes = std::move(inkStrokes);
                } else if (subtype == FPDF_ANNOT_LINE) {
                    FS_POINTF start{};
                    FS_POINTF end{};
                    QPointF normalizedStart;
                    QPointF normalizedEnd;
                    if (FPDFAnnot_GetLine(annotation, &start, &end)
                        && normalizedPagePoint(
                            page,
                            pageSize,
                            QPointF(start.x, start.y),
                            normalizedStart)
                        && normalizedPagePoint(
                            page,
                            pageSize,
                            QPointF(end.x, end.y),
                            normalizedEnd)) {
                        candidate.drawingVertices = {
                            normalizedStart,
                            normalizedEnd};
                    }
                } else if (subtype == FPDF_ANNOT_POLYGON
                           || subtype == FPDF_ANNOT_POLYLINE) {
                    const unsigned long vertexCount
                        = FPDFAnnot_GetVertices(annotation, nullptr, 0);
                    if (vertexCount >= 2
                        && vertexCount
                            <= static_cast<unsigned long>(
                                kMaximumDrawingCommentVertices)) {
                        QVector<FS_POINTF> pageVertices(
                            static_cast<qsizetype>(vertexCount));
                        if (FPDFAnnot_GetVertices(
                                annotation,
                                pageVertices.data(),
                                vertexCount)
                            == vertexCount) {
                            candidate.drawingVertices.reserve(
                                pageVertices.size());
                            for (const FS_POINTF &pageVertex
                                 : std::as_const(pageVertices)) {
                                QPointF normalizedVertex;
                                if (!normalizedPagePoint(
                                        page,
                                        pageSize,
                                        QPointF(
                                            pageVertex.x,
                                            pageVertex.y),
                                        normalizedVertex)) {
                                    candidate.drawingVertices.clear();
                                    break;
                                }
                                candidate.drawingVertices.append(
                                    normalizedVertex);
                            }
                        }
                    }
                } else if (!geometry.isEmpty()) {
                    candidate.drawingVertices = {
                        geometry.topLeft(),
                        geometry.bottomRight()};
                }
            }
            const auto finiteNormalizedPoint = [](const QPointF &point) {
                return std::isfinite(point.x())
                    && std::isfinite(point.y())
                    && point.x() >= 0.0
                    && point.x() <= 1.0
                    && point.y() >= 0.0
                    && point.y() <= 1.0;
            };
            bool drawingGeometryValid = !drawingCommentSubtype;
            if (drawingCommentSubtype) {
                const qsizetype minimumVertexCount
                    = subtype == FPDF_ANNOT_POLYGON ? 3 : 2;
                drawingGeometryValid = inkSubtype
                    ? inkGeometryValid
                    : candidate.drawingVertices.size()
                            >= minimumVertexCount
                        && candidate.drawingVertices.size()
                            <= kMaximumDrawingCommentVertices
                        && std::all_of(
                            candidate.drawingVertices.cbegin(),
                            candidate.drawingVertices.cend(),
                            finiteNormalizedPoint);
                if (subtype == FPDF_ANNOT_SQUARE
                    || subtype == FPDF_ANNOT_CIRCLE
                    || subtype == FPDF_ANNOT_LINE) {
                    drawingGeometryValid = drawingGeometryValid
                        && candidate.drawingVertices.size() == 2;
                }
                if (drawingGeometryValid && !inkSubtype) {
                    for (qsizetype vertexIndex = 1;
                         vertexIndex < candidate.drawingVertices.size();
                         ++vertexIndex) {
                        const QPointF previous
                            = candidate.drawingVertices.at(vertexIndex - 1);
                        const QPointF current
                            = candidate.drawingVertices.at(vertexIndex);
                        if (std::hypot(
                                current.x() - previous.x(),
                                current.y() - previous.y()) <= 0.0) {
                            drawingGeometryValid = false;
                            break;
                        }
                    }
                }
                if (drawingGeometryValid
                    && subtype == FPDF_ANNOT_POLYGON) {
                    const QPointF first
                        = candidate.drawingVertices.constFirst();
                    const QPointF last
                        = candidate.drawingVertices.constLast();
                    drawingGeometryValid = std::hypot(
                        first.x() - last.x(),
                        first.y() - last.y()) > 0.0;
                }
            }
            candidate.commentTextReadOnly
                = candidate.commentTextReadOnly || !drawingGeometryValid;
            const bool sourceGeometrySupported
                = drawingCommentSubtype
                    ? drawingGeometryValid
                    : textMarkupSubtype
                        ? textMarkupGeometryValid
                        : stickyNoteSubtype
                            || freeTextSubtype
                            || stampSubtype;
            candidate.editable = request.annotationsAllowed
                && sourceGeometrySupported
                && !isReplyAnnotation
                && !geometry.isEmpty()
                && !candidate.commentTextReadOnly
                && (annotationFlags & FPDF_ANNOT_FLAG_LOCKED) == 0;
            if (freeTextSubtype) {
                candidate.fontFamily = QStringLiteral("Helvetica");
                candidate.fontSizeRatio = qMin<qreal>(
                    1.0,
                    kFreeTextFontSizePoints / pageSize.height());
                candidate.displayFontSizeRatio = candidate.fontSizeRatio;
                candidate.color = QColor(32, 33, 36);
            }

            switch (subtype) {
            case FPDF_ANNOT_HIGHLIGHT:
                candidate.kind = QStringLiteral("highlight");
                //% "Highlight"
                candidate.label = qtTrId("Highlight");
                break;
            case FPDF_ANNOT_UNDERLINE:
                candidate.kind = QStringLiteral("underline");
                //% "Underline"
                candidate.label = qtTrId("Underline");
                break;
            case FPDF_ANNOT_SQUIGGLY:
                candidate.kind = QStringLiteral("squiggly");
                //% "Squiggly"
                candidate.label = qtTrId("Squiggly");
                break;
            case FPDF_ANNOT_STRIKEOUT:
                candidate.kind = QStringLiteral("strikeout");
                //% "Strikeout"
                candidate.label = qtTrId("Strikeout");
                break;
            case FPDF_ANNOT_TEXT:
                candidate.kind = QStringLiteral("stickyNote");
                //% "Sticky Note"
                candidate.label = qtTrId("StickyNote");
                break;
            case FPDF_ANNOT_FREETEXT:
                candidate.kind = QStringLiteral("freeText");
                //% "Free text annotation"
                candidate.label = qtTrId("FreeTextAnnotation");
                break;
            case FPDF_ANNOT_LINE:
                candidate.kind = QStringLiteral("line");
                //% "Line annotation"
                candidate.label = qtTrId("LineAnnotation");
                break;
            case FPDF_ANNOT_SQUARE:
                candidate.kind = QStringLiteral("square");
                //% "Rectangle annotation"
                candidate.label = qtTrId("RectangleAnnotation");
                break;
            case FPDF_ANNOT_CIRCLE:
                candidate.kind = QStringLiteral("circle");
                //% "Oval annotation"
                candidate.label = qtTrId("OvalAnnotation");
                break;
            case FPDF_ANNOT_POLYGON:
                candidate.kind = QStringLiteral("polygon");
                //% "Polygon annotation"
                candidate.label = qtTrId("PolygonAnnotation");
                break;
            case FPDF_ANNOT_POLYLINE:
                candidate.kind = QStringLiteral("polyLine");
                //% "Polyline annotation"
                candidate.label = qtTrId("PolylineAnnotation");
                break;
            case FPDF_ANNOT_STAMP:
                candidate.kind = QStringLiteral("stamp");
                //% "Stamp annotation"
                candidate.label = qtTrId("StampAnnotation");
                break;
            case FPDF_ANNOT_CARET:
                candidate.kind = QStringLiteral("comment");
                //% "Caret annotation"
                candidate.label = qtTrId("CaretAnnotation");
                break;
            case FPDF_ANNOT_INK:
                candidate.kind = inkGeometryValid
                    ? QStringLiteral("ink")
                    : QStringLiteral("comment");
                //% "Ink annotation"
                candidate.label = qtTrId("InkAnnotation");
                break;
            case FPDF_ANNOT_FILEATTACHMENT:
                candidate.kind = QStringLiteral("comment");
                //% "File attachment annotation"
                candidate.label = qtTrId("FileAttachmentAnnotation");
                break;
            case FPDF_ANNOT_SOUND:
                candidate.kind = QStringLiteral("comment");
                //% "Sound annotation"
                candidate.label = qtTrId("SoundAnnotation");
                break;
            case FPDF_ANNOT_REDACT:
                candidate.kind = QStringLiteral("comment");
                //% "Redaction annotation"
                candidate.label = qtTrId("RedactionAnnotation");
                break;
            default:
                candidate.kind = QStringLiteral("comment");
                //% "Projection annotation"
                candidate.label = qtTrId("ProjectionAnnotation");
                break;
            }
            if (drawingCommentSubtype) {
                const QString sourceDrawingAppearance
                    = PDFAnnotationUtils::annotationNormalAppearance(annotation);
                candidate.sourceDrawingAppearanceFingerprint
                    = PDFAnnotationUtils::normalAppearanceFingerprint(
                        sourceDrawingAppearance);
                const QString identityPrefix
                    = drawingKind(candidate.drawingType)
                    + QLatin1Char(':');
                const QString uuidText
                    = candidate.sourceAnnotationName.startsWith(
                          identityPrefix)
                    ? candidate.sourceAnnotationName.mid(
                        identityPrefix.size())
                    : QString();
                const QUuid drawingUuid = QUuid::fromString(uuidText);
                const bool managedIdentity
                    = candidate.sourceAnnotationHasName
                    && !drawingUuid.isNull()
                    && drawingUuid.toString(
                           QUuid::WithoutBraces).compare(
                           uuidText,
                           Qt::CaseInsensitive) == 0
                    && candidate.commentThreadId
                        == candidate.sourceAnnotationName;
                const bool closedShape
                    = subtype == FPDF_ANNOT_SQUARE
                    || subtype == FPDF_ANNOT_CIRCLE
                    || subtype == FPDF_ANNOT_POLYGON;
                const bool interiorSupported = closedShape
                    ? candidate.commentProperties.interiorColor.isValid()
                        == appearanceStyle.hasFill
                    : !candidate.commentProperties.interiorColor.isValid()
                        && !appearanceStyle.hasFill;
                const bool dashPatternSupported
                    = candidate.commentProperties.dashPattern.size() <= 16
                    && std::all_of(
                        candidate.commentProperties.dashPattern.cbegin(),
                        candidate.commentProperties.dashPattern.cend(),
                        [](qreal dash) {
                            return std::isfinite(dash)
                                && dash > 0.0
                                && dash <= 10'000.0;
                        });
                candidate.drawingPropertiesEditable
                    = candidate.editable
                    && managedIdentity
                    && hasCanonicalDrawingAppearance(sourceDrawingAppearance)
                    && appearanceStyle.hasStroke
                    && appearanceStyle.hasStrokeWidth
                    && appearanceStyle.dashPatternSupported
                    && std::isfinite(
                        candidate.commentProperties.opacity)
                    && candidate.commentProperties.opacity >= 0.0
                    && candidate.commentProperties.opacity <= 1.0
                    && std::isfinite(
                        candidate.commentProperties.borderWidth)
                    && candidate.commentProperties.borderWidth > 0.0
                    && candidate.commentProperties.borderWidth <= 1024.0
                    && dashPatternSupported
                    && interiorSupported
                    && !FPDFAnnot_HasKey(annotation, "BE");
            }
            if (!candidate.replyToObjectId.isEmpty()) {
                //% "Reply"
                candidate.label = qtTrId("Reply");
            }
            if (candidate.commentTextReadOnly) {
                //% "This comment is read-only."
                candidate.reason = qtTrId("ReadOnlyCommentCannotBeEdited");
            } else if ((textMarkupSubtype
                        || stickyNoteSubtype
                        || freeTextSubtype
                        || drawingCommentSubtype
                        || stampSubtype)
                       && !request.annotationsAllowed) {
                //% "This PDF does not permit changing annotations."
                candidate.reason = qtTrId("PdfAnnotationsNotPermitted");
            } else if (candidate.editable) {
                candidate.reason.clear();
            } else {
                //% "Edit the comment text in the Comments panel. Moving or resizing existing comments is not available."
                candidate.reason = qtTrId(
                    "ExistingCommentPageAppearanceReadOnly");
            }
            if (request.maximumCommentIndexBytes >= 0) {
                const qsizetype candidateBytes
                    = estimatedCommentCandidateBytes(candidate);
                if (estimatedCommentBytes
                        > request.maximumCommentIndexBytes
                    || candidateBytes
                        > request.maximumCommentIndexBytes
                            - estimatedCommentBytes) {
                    commentMemoryBudgetExceeded = true;
                    FPDFPage_CloseAnnot(annotation);
                    break;
                }
                estimatedCommentBytes = saturatedByteCountAdd(
                    estimatedCommentBytes,
                    candidateBytes);
            }
            candidates.append(std::move(candidate));
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        if (request.maximumCommentIndexBytes >= 0
            || subtype != FPDF_ANNOT_WIDGET) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        FS_RECTF bounds{};
        if (!FPDFAnnot_GetRect(annotation, &bounds)) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        const QRectF rect = normalizedRect(
            page,
            bounds.left,
            bounds.bottom,
            bounds.right,
            bounds.top,
            kIdentityMatrix,
            pageSize);
        if (rect.isEmpty()) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        Candidate candidate;
        candidate.page = request.page;
        candidate.objectId = QStringLiteral("annotation:") + QString::number(index);
        candidate.sourceAnnotationIndex = index;
        setInitialGeometry(candidate, rect);
        candidate.fontFamily = QStringLiteral("Helvetica");
        candidate.fontSizeRatio = qMax<qreal>(0.01, rect.height() * 0.65);
        candidate.editable = false;
        candidate.limited = true;

        candidate.kind = QStringLiteral("widget");
        candidate.sourceWidgetId = annotationString(annotation, "NM");
        //% "Form field"
        candidate.label = qtTrId("FormField");
        candidate.text = annotationString(annotation, "V");

        if (!request.editingAllowed) {
            //% "This PDF does not permit content editing."
            candidate.reason = qtTrId("PdfEditingNotPermitted");
        } else {
            //% "Editing form fields and free-text annotations is not available in this draft editor yet."
            candidate.reason = qtTrId("DraftEditorUnsupportedAnnotations");
        }
        candidates.append(candidate);
        FPDFPage_CloseAnnot(annotation);
    }

    const QString requestedStateAuthor = request.commentAuthor.trimmed();
    if (!requestedStateAuthor.isEmpty()) {
        QHash<int, qsizetype> candidatesBySourceAnnotationIndex;
        QHash<QString, qsizetype> stateTargetsByObjectId;
        QHash<QString, CommentStateAuthorEvidence>
            stateAuthorEvidenceBySpelling;
        for (qsizetype candidateIndex = 0;
             candidateIndex < candidates.size();
             ++candidateIndex) {
            const Candidate &candidate = candidates.at(candidateIndex);
            if (candidate.sourceAnnotationIndex >= 0) {
                candidatesBySourceAnnotationIndex.insert(
                    candidate.sourceAnnotationIndex,
                    candidateIndex);
            }
            if (isCommentCandidateKind(candidate.kind)
                && !candidate.commentStateAnnotation) {
                stateTargetsByObjectId.insert(
                    candidate.objectId,
                    candidateIndex);
            }
            if (candidate.commentStateAnnotation) {
                const QDateTime timestamp
                    = candidate.commentModifiedDate.isValid()
                    ? candidate.commentModifiedDate
                    : candidate.commentCreationDate;
                includeCommentStateAuthorEvidence(
                    stateAuthorEvidenceBySpelling,
                    requestedStateAuthor,
                    candidate.commentAuthor,
                    timestamp,
                    candidate.sourceAnnotationIndex);
            }
        }
        const QString canonicalStateAuthor
            = preferredCommentStateAuthor(
                stateAuthorEvidenceBySpelling,
                requestedStateAuthor);

        QHash<QString, QVector<const Candidate *>> stateHistoriesByTarget;
        for (const Candidate &candidate : std::as_const(candidates)) {
            if (!candidate.commentStateAnnotation
                || candidate.commentAuthor.trimmed().compare(
                       requestedStateAuthor,
                       Qt::CaseInsensitive) != 0) {
                continue;
            }

            // State transitions can point to an earlier state. The first
            // non-state /IRT ancestor is the actual root-or-reply owner.
            int parentAnnotationIndex
                = candidate.replyDirectParentSourceAnnotationIndex;
            QSet<int> visitedAnnotationIndices;
            while (parentAnnotationIndex >= 0
                   && !visitedAnnotationIndices.contains(
                       parentAnnotationIndex)) {
                visitedAnnotationIndices.insert(parentAnnotationIndex);
                const auto parentIndex
                    = candidatesBySourceAnnotationIndex.constFind(
                        parentAnnotationIndex);
                if (parentIndex
                    == candidatesBySourceAnnotationIndex.cend()) {
                    break;
                }

                const Candidate &parentCandidate
                    = candidates.at(*parentIndex);
                if (!parentCandidate.commentStateAnnotation) {
                    if (isCommentCandidateKind(parentCandidate.kind)) {
                        stateHistoriesByTarget[parentCandidate.objectId]
                            .append(&candidate);
                    }
                    break;
                }
                parentAnnotationIndex = parentCandidate
                    .replyDirectParentSourceAnnotationIndex;
            }
        }

        const auto stateTimestamp = [](const Candidate *candidate) {
            return candidate->commentModifiedDate.isValid()
                ? candidate->commentModifiedDate
                : candidate->commentCreationDate;
        };
        const auto stateDepths = [](const QVector<const Candidate *> &history) {
            QHash<int, const Candidate *> statesByAnnotationIndex;
            for (const Candidate *candidate : history) {
                if (candidate && candidate->sourceAnnotationIndex >= 0) {
                    statesByAnnotationIndex.insert(
                        candidate->sourceAnnotationIndex,
                        candidate);
                }
            }

            QHash<int, int> depths;
            for (const Candidate *start : history) {
                if (!start
                    || start->sourceAnnotationIndex < 0
                    || depths.contains(start->sourceAnnotationIndex)) {
                    continue;
                }

                QVector<const Candidate *> path;
                QSet<int> pathIndices;
                const Candidate *current = start;
                int baseDepth = 0;
                bool cycleDetected = false;
                while (current) {
                    const auto cached = depths.constFind(
                        current->sourceAnnotationIndex);
                    if (cached != depths.cend()) {
                        baseDepth = *cached;
                        break;
                    }
                    if (pathIndices.contains(
                            current->sourceAnnotationIndex)) {
                        cycleDetected = true;
                        break;
                    }
                    path.append(current);
                    pathIndices.insert(current->sourceAnnotationIndex);

                    const auto parent = statesByAnnotationIndex.constFind(
                        current->replyDirectParentSourceAnnotationIndex);
                    if (parent == statesByAnnotationIndex.cend()
                        || (*parent)->commentStateModel.compare(
                               current->commentStateModel,
                               Qt::CaseInsensitive) != 0) {
                        current = nullptr;
                    } else {
                        current = *parent;
                    }
                }

                if (cycleDetected) {
                    for (const Candidate *candidate : path) {
                        depths.insert(candidate->sourceAnnotationIndex, 1);
                    }
                    continue;
                }
                for (auto candidate = path.crbegin();
                     candidate != path.crend();
                     ++candidate) {
                    depths.insert(
                        (*candidate)->sourceAnnotationIndex,
                        ++baseDepth);
                }
            }
            return depths;
        };

        struct AuthorHistory {
            QString author;
            QVector<const Candidate *> states;
            int maximumDepth = 0;
            int earliestAnnotationIndex = (std::numeric_limits<int>::max)();
            QDateTime earliestTimestamp;
            QDateTime latestTimestamp;
            bool exactlyMatchesRequestedAuthor = false;
        };
        const auto authorHistoryIsBetter = [](const AuthorHistory &candidate,
                                              const AuthorHistory &current) {
            if (candidate.maximumDepth != current.maximumDepth) {
                return candidate.maximumDepth > current.maximumDepth;
            }
            if (candidate.states.size() != current.states.size()) {
                return candidate.states.size() > current.states.size();
            }
            if (candidate.latestTimestamp.isValid()
                && current.latestTimestamp.isValid()
                && candidate.latestTimestamp != current.latestTimestamp) {
                return candidate.latestTimestamp
                    > current.latestTimestamp;
            }
            if (candidate.latestTimestamp.isValid()
                != current.latestTimestamp.isValid()) {
                return candidate.latestTimestamp.isValid();
            }
            if (candidate.earliestTimestamp.isValid()
                && current.earliestTimestamp.isValid()
                && candidate.earliestTimestamp != current.earliestTimestamp) {
                return candidate.earliestTimestamp
                    < current.earliestTimestamp;
            }
            if (candidate.earliestTimestamp.isValid()
                != current.earliestTimestamp.isValid()) {
                return candidate.earliestTimestamp.isValid();
            }
            if (candidate.earliestAnnotationIndex
                != current.earliestAnnotationIndex) {
                return candidate.earliestAnnotationIndex
                    < current.earliestAnnotationIndex;
            }
            if (candidate.exactlyMatchesRequestedAuthor
                != current.exactlyMatchesRequestedAuthor) {
                return candidate.exactlyMatchesRequestedAuthor;
            }
            return candidate.author < current.author;
        };
        const auto terminalStateForModel
            = [&](const AuthorHistory &history,
                  QLatin1StringView stateModel) -> const Candidate * {
                  QVector<const Candidate *> modelStates;
                  QHash<int, const Candidate *> statesByAnnotationIndex;
                  for (const Candidate *candidate : history.states) {
                      if (candidate->commentStateModel.compare(
                              stateModel,
                              Qt::CaseInsensitive) == 0) {
                          modelStates.append(candidate);
                          statesByAnnotationIndex.insert(
                              candidate->sourceAnnotationIndex,
                              candidate);
                      }
                  }
                  if (modelStates.isEmpty()) {
                      return nullptr;
                  }

                  QSet<int> statesWithChildren;
                  for (const Candidate *candidate : modelStates) {
                      if (statesByAnnotationIndex.contains(
                              candidate
                                  ->replyDirectParentSourceAnnotationIndex)) {
                          statesWithChildren.insert(
                              candidate
                                  ->replyDirectParentSourceAnnotationIndex);
                      }
                  }
                  const QHash<int, int> depths = stateDepths(modelStates);
                  const Candidate *best = nullptr;
                  for (const Candidate *candidate : modelStates) {
                      if (statesWithChildren.contains(
                              candidate->sourceAnnotationIndex)) {
                          continue;
                      }
                      if (!best) {
                          best = candidate;
                          continue;
                      }

                      const int candidateDepth = depths.value(
                          candidate->sourceAnnotationIndex,
                          1);
                      const int bestDepth = depths.value(
                          best->sourceAnnotationIndex,
                          1);
                      const QDateTime candidateTimestamp
                          = stateTimestamp(candidate);
                      const QDateTime bestTimestamp = stateTimestamp(best);
                      const bool newerTimestamp
                          = candidateTimestamp.isValid()
                          && (!bestTimestamp.isValid()
                              || candidateTimestamp > bestTimestamp);
                      const bool sameTimestamp
                          = (!candidateTimestamp.isValid()
                             && !bestTimestamp.isValid())
                          || (candidateTimestamp.isValid()
                              && bestTimestamp.isValid()
                              && candidateTimestamp == bestTimestamp);
                      if (candidateDepth > bestDepth
                          || (candidateDepth == bestDepth
                              && (newerTimestamp
                                  || (sameTimestamp
                                      && candidate->sourceAnnotationIndex
                                          > best
                                                ->sourceAnnotationIndex)))) {
                          best = candidate;
                      }
                  }
                  return best;
              };

        for (auto targetHistory = stateHistoriesByTarget.cbegin();
             targetHistory != stateHistoriesByTarget.cend();
             ++targetHistory) {
            const auto targetIndex = stateTargetsByObjectId.constFind(
                targetHistory.key());
            if (targetIndex == stateTargetsByObjectId.cend()) {
                continue;
            }

            QVector<AuthorHistory> authorHistories;
            QHash<QString, qsizetype> historyIndexByAuthor;
            // Match environment/profile aliases case-insensitively only long
            // enough to find their histories. From this point onward, exact
            // spelling identifies a reviewer, matching Foxit's behavior.
            for (const Candidate *stateCandidate : targetHistory.value()) {
                const QString author = stateCandidate->commentAuthor.trimmed();
                auto historyIndex = historyIndexByAuthor.constFind(author);
                if (historyIndex == historyIndexByAuthor.cend()) {
                    AuthorHistory history;
                    history.author = author;
                    history.exactlyMatchesRequestedAuthor
                        = author == requestedStateAuthor;
                    authorHistories.append(std::move(history));
                    const qsizetype addedIndex = authorHistories.size() - 1;
                    historyIndexByAuthor.insert(author, addedIndex);
                    historyIndex = historyIndexByAuthor.constFind(author);
                }
                AuthorHistory &history = authorHistories[*historyIndex];
                history.states.append(stateCandidate);
                history.earliestAnnotationIndex = qMin(
                    history.earliestAnnotationIndex,
                    stateCandidate->sourceAnnotationIndex);
                const QDateTime timestamp = stateTimestamp(stateCandidate);
                if (timestamp.isValid()
                    && (!history.earliestTimestamp.isValid()
                        || timestamp < history.earliestTimestamp)) {
                    history.earliestTimestamp = timestamp;
                }
                if (timestamp.isValid()
                    && (!history.latestTimestamp.isValid()
                        || timestamp > history.latestTimestamp)) {
                    history.latestTimestamp = timestamp;
                }
            }
            for (AuthorHistory &history : authorHistories) {
                const QHash<int, int> depths = stateDepths(history.states);
                for (auto depth = depths.cbegin(); depth != depths.cend(); ++depth) {
                    history.maximumDepth = qMax(
                        history.maximumDepth,
                        depth.value());
                }
            }
            if (authorHistories.isEmpty()) {
                continue;
            }

            const AuthorHistory *selectedHistory = nullptr;
            for (const AuthorHistory &history : std::as_const(authorHistories)) {
                if (history.author == canonicalStateAuthor) {
                    selectedHistory = &history;
                    break;
                }
            }
            if (!selectedHistory) {
                selectedHistory = &authorHistories.constFirst();
                for (qsizetype historyIndex = 1;
                     historyIndex < authorHistories.size();
                     ++historyIndex) {
                    const AuthorHistory &candidateHistory
                        = authorHistories.at(historyIndex);
                    if (authorHistoryIsBetter(
                            candidateHistory,
                            *selectedHistory)) {
                        selectedHistory = &candidateHistory;
                    }
                }
            }

            Candidate &targetCandidate = candidates[*targetIndex];
            targetCandidate.commentStateAuthor = canonicalStateAuthor;
            // A state transition is ordered by /IRT, not by its position in
            // /Annots. Date and annotation index only resolve equal-depth
            // terminal forks in malformed or concurrently edited files.
            const Candidate *reviewState = terminalStateForModel(
                *selectedHistory,
                QLatin1StringView("Review"));
            if (reviewState) {
                targetCandidate.commentStatusAnnotationIndex
                    = reviewState->sourceAnnotationIndex;
                targetCandidate.commentStatus = commentStatusForPdfName(
                    reviewState->commentState).value_or(NoCommentStatus);
            }
            const Candidate *markedState = terminalStateForModel(
                *selectedHistory,
                QLatin1StringView("Marked"));
            if (markedState) {
                targetCandidate.commentMarkedAnnotationIndex
                    = markedState->sourceAnnotationIndex;
                targetCandidate.commentMarked
                    = markedState->commentState.compare(
                          QLatin1String("Marked"),
                          Qt::CaseInsensitive) == 0;
            }
        }
    }
    return succeeded;
}
