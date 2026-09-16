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

namespace PDFEditControllerPrivate {

qsizetype saturatedByteCountAdd(qsizetype left, qsizetype right)
{
    constexpr qsizetype maximum = (std::numeric_limits<qsizetype>::max)();
    if (left < 0 || right < 0 || left > maximum - right) {
        return maximum;
    }
    return left + right;
}

const StampPreset *stampPreset(const QString &name)
{
    return PDFStampPresets::find(name);
}

QString stampTextForName(const QString &name)
{
    const StampPreset *preset = stampPreset(name);
    return preset
        ? QString::fromLatin1(preset->text)
        : name.trimmed().left(64);
}

QColor stampColorForName(const QString &name)
{
    const StampPreset *preset = stampPreset(name);
    return preset
        ? QColor(preset->red, preset->green, preset->blue)
        : QColor(97, 97, 97);
}

bool candidateScanCancelled(
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation
        && cancellation->load(std::memory_order_relaxed);
}

bool isTextCandidateKind(const QString &kind)
{
    return kind == QLatin1String("text")
        || kind == QLatin1String("addedText");
}

bool isShapeCandidateKind(const QString &kind)
{
    return kind == QLatin1String("addedShape")
        || kind == QLatin1String("sourceShape");
}

bool isSupportedShapeType(PDFEditController::ShapeType shapeType)
{
    switch (shapeType) {
    case PDFEditController::RectangleShape:
    case PDFEditController::EllipseShape:
    case PDFEditController::LineShape:
    case PDFEditController::ArrowShape:
    case PDFEditController::TriangleShape:
    case PDFEditController::DiamondShape:
        return true;
    }
    return false;
}

qreal distanceToSegment(const QPointF &point,
                        const QPointF &start,
                        const QPointF &end)
{
    const QPointF segment = end - start;
    const qreal lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 0.0) {
        return std::hypot(point.x() - start.x(), point.y() - start.y());
    }

    const qreal projection = qBound<qreal>(
        0.0,
        QPointF::dotProduct(point - start, segment) / lengthSquared,
        1.0);
    const QPointF nearest = start + projection * segment;
    return std::hypot(point.x() - nearest.x(), point.y() - nearest.y());
}

qreal shapeStrokeWidthPoints(qreal strokeWidthRatio, qreal pageHeight)
{
    return qMax<qreal>(0.75, strokeWidthRatio * pageHeight);
}

qreal shapeHitTolerancePoints(qreal strokeWidthRatio, qreal pageHeight)
{
    return qMax<qreal>(
        3.0,
        shapeStrokeWidthPoints(strokeWidthRatio, pageHeight) / 2.0 + 2.0);
}

bool draftRepresentsDeletion(const QString &kind,
                             const QString &text,
                             bool explicitlyDeleted)
{
    return explicitlyDeleted || (isTextCandidateKind(kind) && text.isEmpty());
}

bool isTextMarkupTool(PDFEditController::Tool tool)
{
    return tool == PDFEditController::HighlightTool
        || tool == PDFEditController::UnderlineTool
        || tool == PDFEditController::SquigglyTool
        || tool == PDFEditController::StrikeoutTool;
}

bool isDrawingCommentTool(PDFEditController::Tool tool)
{
    return tool == PDFEditController::SquareTool
        || tool == PDFEditController::CircleTool
        || tool == PDFEditController::LineTool
        || tool == PDFEditController::PolygonTool
        || tool == PDFEditController::PolyLineTool
        || tool == PDFEditController::InkTool;
}

bool isTextMarkupCandidateKind(const QString &kind)
{
    return kind == QLatin1String("highlight")
        || kind == QLatin1String("underline")
        || kind == QLatin1String("squiggly")
        || kind == QLatin1String("strikeout");
}

bool isDrawingCommentCandidateKind(const QString &kind)
{
    return kind == QLatin1String("square")
        || kind == QLatin1String("circle")
        || kind == QLatin1String("line")
        || kind == QLatin1String("polygon")
        || kind == QLatin1String("polyLine")
        || kind == QLatin1String("ink");
}

bool isGeometryCommentCandidateKind(const QString &kind)
{
    return isTextMarkupCandidateKind(kind)
        || isDrawingCommentCandidateKind(kind)
        || kind == QLatin1String("stickyNote")
        || kind == QLatin1String("freeText")
        || kind == QLatin1String("stamp");
}

bool isCommentCandidateKind(const QString &kind)
{
    return isTextMarkupCandidateKind(kind)
        || isDrawingCommentCandidateKind(kind)
        || kind == QLatin1String("comment")
        || kind == QLatin1String("stickyNote")
        || kind == QLatin1String("freeText")
        || kind == QLatin1String("stamp");
}

QString drawingKind(PDFCommentAnnotation::DrawingType type)
{
    switch (type) {
    case PDFCommentAnnotation::DrawingType::Circle:
        return QStringLiteral("circle");
    case PDFCommentAnnotation::DrawingType::Line:
        return QStringLiteral("line");
    case PDFCommentAnnotation::DrawingType::Polygon:
        return QStringLiteral("polygon");
    case PDFCommentAnnotation::DrawingType::PolyLine:
        return QStringLiteral("polyLine");
    case PDFCommentAnnotation::DrawingType::Ink:
        return QStringLiteral("ink");
    default:
        return QStringLiteral("square");
    }
}

bool sameFontFamily(const QString &left, const QString &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

QSize mappingDeviceSize(const QSizeF &pageSize)
{
    const qreal maximumDimension = qMax(pageSize.width(), pageSize.height());
    if (maximumDimension <= 0.0) {
        return {};
    }

    const qreal scale = kCoordinateMapExtent / maximumDimension;
    return QSize(qMax(1, qRound(pageSize.width() * scale)),
                 qMax(1, qRound(pageSize.height() * scale)));
}

QPointF mapPoint(const FS_MATRIX &matrix, const QPointF &point)
{
    return QPointF(matrix.a * point.x() + matrix.c * point.y() + matrix.e,
                   matrix.b * point.x() + matrix.d * point.y() + matrix.f);
}

bool pagePointToDevice(FPDF_PAGE page,
                       const QSize &deviceSize,
                       const QPointF &pagePoint,
                       QPointF &devicePoint)
{
    int deviceX = 0;
    int deviceY = 0;
    if (!FPDF_PageToDevice(page,
                           0,
                           0,
                           deviceSize.width(),
                           deviceSize.height(),
                           0,
                           pagePoint.x(),
                           pagePoint.y(),
                           &deviceX,
                           &deviceY)) {
        return false;
    }

    devicePoint = QPointF(deviceX, deviceY);
    return true;
}

bool normalizedPagePoint(FPDF_PAGE page,
                         const QSizeF &pageSize,
                         const QPointF &pagePoint,
                         QPointF &normalizedPoint)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF devicePoint;
    if (deviceSize.isEmpty()
        || !pagePointToDevice(page, deviceSize, pagePoint, devicePoint)) {
        return false;
    }

    normalizedPoint = QPointF(devicePoint.x() / deviceSize.width(),
                              devicePoint.y() / deviceSize.height());
    return true;
}

QRectF normalizedRect(FPDF_PAGE page,
                      double left,
                      double bottom,
                      double right,
                      double top,
                      const FS_MATRIX &containerTransform,
                      const QSizeF &pageSize)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    if (!page || deviceSize.isEmpty()) {
        return {};
    }

    const std::array<QPointF, 4> containerPoints{
        QPointF(left, bottom),
        QPointF(left, top),
        QPointF(right, bottom),
        QPointF(right, top)};
    std::array<QPointF, 4> devicePoints;
    for (size_t index = 0; index < containerPoints.size(); ++index) {
        if (!pagePointToDevice(page,
                               deviceSize,
                               mapPoint(containerTransform, containerPoints[index]),
                               devicePoints[index])) {
            return {};
        }
    }

    qreal minimumX = devicePoints.front().x();
    qreal maximumX = minimumX;
    qreal minimumY = devicePoints.front().y();
    qreal maximumY = minimumY;
    for (const QPointF &point : devicePoints) {
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    }

    const QRectF normalized(minimumX / deviceSize.width(),
                            minimumY / deviceSize.height(),
                            (maximumX - minimumX) / deviceSize.width(),
                            (maximumY - minimumY) / deviceSize.height());
    return normalized.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

QString currentCommentAuthor()
{
    const QStringList candidates{
        qEnvironmentVariable("USER"),
        qEnvironmentVariable("LOGNAME"),
        QFileInfo(QDir::homePath()).fileName(),
        QCoreApplication::applicationName()};
    for (const QString &candidate : candidates) {
        const QString author = candidate.trimmed();
        if (!author.isEmpty()) {
            return author;
        }
    }
    return {};
}

void includeCommentStateAuthorEvidence(
    QHash<QString, CommentStateAuthorEvidence> &evidenceByAuthor,
    const QString &requestedAuthor,
    const QString &stateAuthor,
    const QDateTime &timestamp,
    qint64 sequence)
{
    const QString author = stateAuthor.trimmed();
    if (author.isEmpty()
        || author.compare(requestedAuthor, Qt::CaseInsensitive) != 0) {
        return;
    }

    auto evidence = evidenceByAuthor.find(author);
    if (evidence == evidenceByAuthor.end()) {
        CommentStateAuthorEvidence added;
        added.author = author;
        added.exactlyMatchesRequestedAuthor = author == requestedAuthor;
        evidence = evidenceByAuthor.insert(author, std::move(added));
    }
    ++evidence->transitionCount;
    if (timestamp.isValid()
        && (!evidence->latestTimestamp.isValid()
            || timestamp > evidence->latestTimestamp)) {
        evidence->latestTimestamp = timestamp;
    }
    evidence->firstSequence = qMin(evidence->firstSequence, sequence);
}

QString preferredCommentStateAuthor(
    const QHash<QString, CommentStateAuthorEvidence> &evidenceByAuthor,
    const QString &requestedAuthor)
{
    const auto isBetter = [](const CommentStateAuthorEvidence &candidate,
                             const CommentStateAuthorEvidence &current) {
        if (candidate.transitionCount != current.transitionCount) {
            return candidate.transitionCount > current.transitionCount;
        }
        if (candidate.latestTimestamp.isValid()
            && current.latestTimestamp.isValid()
            && candidate.latestTimestamp != current.latestTimestamp) {
            return candidate.latestTimestamp > current.latestTimestamp;
        }
        if (candidate.latestTimestamp.isValid()
            != current.latestTimestamp.isValid()) {
            return candidate.latestTimestamp.isValid();
        }
        if (candidate.exactlyMatchesRequestedAuthor
            != current.exactlyMatchesRequestedAuthor) {
            return candidate.exactlyMatchesRequestedAuthor;
        }
        if (candidate.firstSequence != current.firstSequence) {
            return candidate.firstSequence < current.firstSequence;
        }
        return candidate.author < current.author;
    };

    const CommentStateAuthorEvidence *preferred = nullptr;
    for (auto evidence = evidenceByAuthor.cbegin();
         evidence != evidenceByAuthor.cend();
         ++evidence) {
        if (!preferred || isBetter(*evidence, *preferred)) {
            preferred = &*evidence;
        }
    }
    return preferred ? preferred->author : requestedAuthor.trimmed();
}

QString fontFamilyMatchKey(QString name)
{
    if (name.size() > 7 && name.at(6) == QLatin1Char('+')) {
        name = name.sliced(7);
    }

    const qsizetype styleSeparator = name.indexOf(QLatin1Char('-'));
    if (styleSeparator > 0) {
        name.truncate(styleSeparator);
    }

    if (name.endsWith(QStringLiteral("PSMT"), Qt::CaseInsensitive)) {
        name.chop(4);
    } else if (name.endsWith(QStringLiteral("MT"), Qt::CaseInsensitive)
               || name.endsWith(QStringLiteral("PS"), Qt::CaseInsensitive)) {
        name.chop(2);
    }

    QString key;
    key.reserve(name.size());
    for (const QChar character : name) {
        if (character.isLetterOrNumber()) {
            key.append(character.toCaseFolded());
        }
    }
    return key;
}

} // namespace PDFEditControllerPrivate
