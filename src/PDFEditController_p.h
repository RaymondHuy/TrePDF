#pragma once

// Implementation details shared only by the PDFEditController translation units.
#include "PDFEditController.h"
#include "PDFAnnotationUtils.h"
#include "PDFStampPresets.h"

#include <array>
#include <limits>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

namespace PDFEditControllerPrivate {

using PDFAnnotationUtils::annotationString;

using PDFAnnotationUtils::annotationUsesReplyRelation;

using PDFAnnotationUtils::isMarkupAnnotation;

using PDFAnnotationUtils::isCommentStateAnnotation;

using PDFAnnotationUtils::isSupportedCommentAnnotationSubtype;

using PDFAnnotationUtils::normalizedCommentText;

constexpr int kCoordinateMapExtent = 100000;

constexpr qreal kMinimumShapeDragPoints = 3.0;

constexpr qreal kFreeTextFontSizePoints = 12.0;

constexpr int kHighlightRed = 255;

constexpr int kHighlightGreen = 235;

constexpr int kHighlightBlue = 59;

constexpr int kHighlightAlpha = 96;

constexpr qsizetype kMaximumDrawingCommentVertices = 256;

constexpr FS_MATRIX kIdentityMatrix{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};

constexpr int kAnnotationFlagLockedContents = 1 << 9;

qsizetype saturatedByteCountAdd(qsizetype left, qsizetype right);

using StampPreset = PDFStampPresets::Preset;

const StampPreset *stampPreset(const QString &name);

QString stampTextForName(const QString &name);

QColor stampColorForName(const QString &name);

bool candidateScanCancelled(
    const std::shared_ptr<std::atomic_bool> &cancellation);

bool isTextCandidateKind(const QString &kind);

bool isShapeCandidateKind(const QString &kind);

bool isSupportedShapeType(PDFEditController::ShapeType shapeType);

qreal distanceToSegment(const QPointF &point,
                        const QPointF &start,
                        const QPointF &end);

qreal shapeStrokeWidthPoints(qreal strokeWidthRatio, qreal pageHeight);

qreal shapeHitTolerancePoints(qreal strokeWidthRatio, qreal pageHeight);

bool draftRepresentsDeletion(const QString &kind,
                             const QString &text,
                             bool explicitlyDeleted);

bool isTextMarkupTool(PDFEditController::Tool tool);

bool isDrawingCommentTool(PDFEditController::Tool tool);

bool isTextMarkupCandidateKind(const QString &kind);

bool isDrawingCommentCandidateKind(const QString &kind);

bool isGeometryCommentCandidateKind(const QString &kind);

bool isCommentCandidateKind(const QString &kind);

QString drawingKind(PDFCommentAnnotation::DrawingType type);

bool sameFontFamily(const QString &left, const QString &right);

QSize mappingDeviceSize(const QSizeF &pageSize);

QPointF mapPoint(const FS_MATRIX &matrix, const QPointF &point);

bool pagePointToDevice(FPDF_PAGE page,
                       const QSize &deviceSize,
                       const QPointF &pagePoint,
                       QPointF &devicePoint);

bool normalizedPagePoint(FPDF_PAGE page,
                         const QSizeF &pageSize,
                         const QPointF &pagePoint,
                         QPointF &normalizedPoint);

QRectF normalizedRect(FPDF_PAGE page,
                      double left,
                      double bottom,
                      double right,
                      double top,
                      const FS_MATRIX &containerTransform,
                      const QSizeF &pageSize);

QString currentCommentAuthor();

struct CommentStateAuthorEvidence {
    QString author;
    qsizetype transitionCount = 0;
    QDateTime latestTimestamp;
    qint64 firstSequence = (std::numeric_limits<qint64>::max)();
    bool exactlyMatchesRequestedAuthor = false;
};

void includeCommentStateAuthorEvidence(
    QHash<QString, CommentStateAuthorEvidence> &evidenceByAuthor,
    const QString &requestedAuthor,
    const QString &stateAuthor,
    const QDateTime &timestamp,
    qint64 sequence);

QString preferredCommentStateAuthor(
    const QHash<QString, CommentStateAuthorEvidence> &evidenceByAuthor,
    const QString &requestedAuthor);

QString fontFamilyMatchKey(QString name);

} // namespace PDFEditControllerPrivate
