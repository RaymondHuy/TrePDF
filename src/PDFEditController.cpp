#include "PDFEditController.h"
#include "ApplicationFonts.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
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

namespace {

using PDFAnnotationUtils::annotationString;
using PDFAnnotationUtils::annotationUsesReplyRelation;
using PDFAnnotationUtils::isMarkupAnnotation;
using PDFAnnotationUtils::isCommentStateAnnotation;
using PDFAnnotationUtils::isSupportedCommentAnnotationSubtype;
using PDFAnnotationUtils::normalizedCommentText;

constexpr int kCoordinateMapExtent = 100000;
constexpr qsizetype kMaximumCachedCandidatePages = 12;
constexpr qreal kAddedTextWidthRatio = 0.25;
constexpr qreal kAddedTextMinimumWidthRatio = 0.02;
constexpr qreal kAddedTextHeightRatio = 0.04;
constexpr qreal kAddedTextFontSizeRatio = 0.022;
constexpr qreal kAddedImageMaximumDimensionRatio = 0.25;
constexpr qreal kMinimumShapeDragPoints = 3.0;
constexpr qreal kMinimumShapeFramePoints = 6.0;
constexpr qreal kAddedShapeStrokeWidthRatio = 0.0022;
constexpr qreal kShapeAxisSnapRatio = 0.12;
constexpr qreal kMinimumHighlightDimensionRatio = 0.0005;
constexpr qreal kStickyNoteSizePoints = 24.0;
constexpr qreal kFreeTextDefaultWidthPoints = 160.0;
constexpr qreal kFreeTextDefaultHeightPoints = 64.0;
constexpr qreal kFreeTextMinimumWidthPoints = 36.0;
constexpr qreal kFreeTextMinimumHeightPoints = 24.0;
constexpr qreal kFreeTextFontSizePoints = 12.0;
constexpr qreal kStampDefaultWidthPoints = 160.0;
constexpr qreal kStampDefaultHeightPoints = 50.0;
constexpr qreal kStampMinimumWidthPoints = 48.0;
constexpr qreal kStampMinimumHeightPoints = 24.0;
constexpr int kHighlightRed = 255;
constexpr int kHighlightGreen = 235;
constexpr int kHighlightBlue = 59;
constexpr int kHighlightAlpha = 96;
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
constexpr qreal kDrawingCommentBorderWidthPoints = 1.5;
constexpr qreal kDrawingCommentVertexMergeDistancePoints = 0.5;
constexpr qsizetype kMaximumDrawingCommentVertices = 256;
constexpr qsizetype kMaximumRawInkPoints = 4096;
constexpr qsizetype kMaximumSimplifiedInkPoints = 1024;
constexpr qreal kInkSimplificationTolerancePoints = 0.75;
constexpr unsigned long kMaximumSourceInkStrokes = 64;
constexpr unsigned long kMaximumSourceInkPointsPerStroke = 4096;
constexpr unsigned long kMaximumSourceInkPoints = 32768;
constexpr size_t kMaximumSourceTextMarkupRects = 50000;
constexpr size_t kMaximumEditableSourceTextMarkupQuads = 4096;
// This is a conservative estimate of heap storage retained by the
// whole-document source-comment index. Per-page interaction caches have their
// own LRU bound and are not charged twice for implicitly shared payloads.
constexpr qsizetype kMaximumCommentIndexBytes = 16 * 1024 * 1024;
constexpr qsizetype kEstimatedHeapAllocationOverhead = 32;
constexpr qsizetype kEstimatedIndexedCandidateOverhead = 128;
constexpr qint64 kMaximumAddedImageFileBytes = 64 * 1024 * 1024;
constexpr qint64 kMaximumAddedImagePixels = 40 * 1000 * 1000;
constexpr unsigned long kMaximumAnnotationStringBytes
    = 4UL * 1024UL * 1024UL;
constexpr qsizetype kMaximumCommentCharacters = 700;
constexpr qreal kLineBaselineToleranceEm = 0.25;
constexpr qreal kLineMinimumHorizontalGapEm = -0.35;
constexpr qreal kLineMaximumHorizontalGapEm = 1.25;
constexpr qreal kLineWordSeparatorGapEm = 0.20;
constexpr qreal kLineFontSizeToleranceRatio = 0.08;
constexpr qreal kLineFontSizeTolerancePoints = 0.5;
constexpr qreal kTextMarkupMinimumLineOverlapRatio = 0.25;
constexpr qreal kTextMarkupLineCenterToleranceRatio = 0.55;
constexpr qreal kTextMarkupMaximumGapHeightRatio = 1.25;
constexpr qreal kTextMarkupMaximumAngleRadians = 0.017453292519943295;
constexpr qreal kSquigglyBottomPaddingPoints = 3.5;
constexpr qsizetype kMaximumPendingSquigglyCreations = 32;
constexpr qreal kFullTurnRadians = 6.2831853071795864769;
constexpr int kMaximumTextMarkupCharactersPerPage = 50000;
constexpr qreal kInitialTextFrameSafetyPoints = 4.0;
constexpr qreal kTextEditMinimumPageMarginPoints = 12.0;
constexpr qreal kTextEditMaximumPageMarginRatio = 0.12;
constexpr qreal kTextEditMinimumExpansionPoints = 2.0;
constexpr qreal kTextEditRowToleranceRatio = 0.35;
constexpr int kLineFontStretchTolerance = 5;
constexpr int kPdfFontFlagItalic = 1 << 6;
constexpr int kPdfFontFlagForceBold = 1 << 18;
constexpr qsizetype kMaximumEmbeddedFontBytes = 32 * 1024 * 1024;
constexpr qsizetype kMaximumRegisteredFontBytes = 64 * 1024 * 1024;
constexpr qsizetype kMaximumRegisteredFontCount = 128;
constexpr qsizetype kMaximumRejectedFontFingerprints = 128;
constexpr FS_MATRIX kIdentityMatrix{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
constexpr char kPreservedPaintMark[] = "TrePDFPreservedPaint";
constexpr char kCommentThreadIdKey[] = "TrePDFThreadId";
constexpr char kCommentReplyToKey[] = "TrePDFReplyTo";
constexpr int kAnnotationFlagLockedContents = 1 << 9;

qsizetype saturatedByteCountAdd(qsizetype left, qsizetype right)
{
    constexpr qsizetype maximum = (std::numeric_limits<qsizetype>::max)();
    if (left < 0 || right < 0 || left > maximum - right) {
        return maximum;
    }
    return left + right;
}

qsizetype estimatedContiguousStorageBytes(qsizetype elementCount,
                                          qsizetype elementSize)
{
    if (elementCount <= 0 || elementSize <= 0) {
        return 0;
    }
    constexpr qsizetype maximum = (std::numeric_limits<qsizetype>::max)();
    if (elementCount > (maximum - kEstimatedHeapAllocationOverhead)
            / elementSize) {
        return maximum;
    }
    return kEstimatedHeapAllocationOverhead + elementCount * elementSize;
}

qsizetype estimatedStringStorageBytes(const QString &value)
{
    return estimatedContiguousStorageBytes(
        qMax(value.size(), value.capacity()),
        sizeof(QChar));
}

struct StampPreset {
    const char *name;
    const char *text;
    int red;
    int green;
    int blue;
};

constexpr std::array<StampPreset, 6> kStampPresets{{
    {"Approved", "APPROVED", 46, 125, 50},
    {"Draft", "DRAFT", 21, 101, 192},
    {"Final", "FINAL", 40, 53, 147},
    {"Confidential", "CONFIDENTIAL", 198, 40, 40},
    {"ForComment", "FOR COMMENT", 239, 108, 0},
    {"NotApproved", "NOT APPROVED", 198, 40, 40},
}};

const StampPreset *stampPreset(const QString &name)
{
    const auto preset = std::find_if(
        kStampPresets.cbegin(),
        kStampPresets.cend(),
        [&name](const StampPreset &candidate) {
            return name == QLatin1String(candidate.name);
        });
    return preset == kStampPresets.cend() ? nullptr : &*preset;
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

bool pageObjectHasContentMark(FPDF_PAGEOBJECT object,
                              const QString &expectedName)
{
    assertPDFiumProcessLockHeld();
    const int markCount = FPDFPageObj_CountMarks(object);
    for (int index = 0; index < markCount; ++index) {
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(
            object,
            static_cast<unsigned long>(index));
        unsigned long byteLength = 0;
        if (!mark
            || !FPDFPageObjMark_GetName(mark, nullptr, 0, &byteLength)
            || byteLength < sizeof(FPDF_WCHAR)) {
            continue;
        }
        QVector<FPDF_WCHAR> buffer(
            static_cast<qsizetype>(
                (byteLength + sizeof(FPDF_WCHAR) - 1)
                / sizeof(FPDF_WCHAR)),
            0);
        unsigned long actualByteLength = 0;
        if (!FPDFPageObjMark_GetName(
                mark,
                buffer.data(),
                byteLength,
                &actualByteLength)) {
            continue;
        }
        qsizetype characterCount = static_cast<qsizetype>(
            actualByteLength / sizeof(FPDF_WCHAR));
        while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
            --characterCount;
        }
        const QString name = QString::fromUtf16(
            reinterpret_cast<const char16_t *>(buffer.constData()),
            characterCount);
        if (name == expectedName) {
            return true;
        }
    }
    return false;
}

bool candidateScanCancelled(
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation
        && cancellation->load(std::memory_order_relaxed);
}

QThreadPool &candidateScanThreadPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

QThreadPool &commentIndexThreadPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

class CandidateScanDocument final
{
public:
    CandidateScanDocument() = default;

    ~CandidateScanDocument()
    {
        // The pool can expire its worker independently of a scan transaction.
        PDFiumProcessLock pdfiumLock;
        closeLocked();
    }

    FPDF_DOCUMENT documentLocked(const QString &filePath, int revision)
    {
        assertPDFiumProcessLockHeld();
        if (m_document && m_filePath == filePath && m_revision == revision) {
            return m_document;
        }

        closeLocked();
        const QByteArray encodedPath = QFile::encodeName(filePath);
        m_document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        if (m_document) {
            m_filePath = filePath;
            m_revision = revision;
        }
        return m_document;
    }

    Q_DISABLE_COPY_MOVE(CandidateScanDocument)

private:
    void closeLocked()
    {
        assertPDFiumProcessLockHeld();
        if (m_document) {
            FPDF_CloseDocument(m_document);
        }
        m_document = nullptr;
        m_filePath.clear();
        m_revision = -1;
    }

    FPDF_DOCUMENT m_document = nullptr;
    QString m_filePath;
    int m_revision = -1;
};

class CandidateScanWorkQueue final
{
public:
    CandidateScanWorkQueue() = default;

    void submit(std::function<void()> work,
                std::function<void()> dropped)
    {
        std::function<void()> previouslyDropped;
        bool startWorker = false;
        {
            QMutexLocker locker(&m_mutex);
            previouslyDropped = std::move(m_pendingDropped);
            m_pendingWork = std::move(work);
            m_pendingDropped = std::move(dropped);
            if (!m_workerActive) {
                m_workerActive = true;
                startWorker = true;
            }
        }

        if (previouslyDropped) {
            previouslyDropped();
        }
        if (startWorker) {
            candidateScanThreadPool().start(QRunnable::create([this]() {
                drain();
            }));
        }
    }

    Q_DISABLE_COPY_MOVE(CandidateScanWorkQueue)

private:
    void drain()
    {
        while (true) {
            std::function<void()> work;
            {
                QMutexLocker locker(&m_mutex);
                if (!m_pendingWork) {
                    m_workerActive = false;
                    return;
                }
                work = std::move(m_pendingWork);
                m_pendingDropped = {};
            }
            work();
        }
    }

    QMutex m_mutex;
    std::function<void()> m_pendingWork;
    std::function<void()> m_pendingDropped;
    bool m_workerActive = false;
};

CandidateScanWorkQueue &candidateScanWorkQueue()
{
    static CandidateScanWorkQueue queue;
    return queue;
}

bool isTextCandidateKind(const QString &kind)
{
    return kind == QLatin1String("text")
        || kind == QLatin1String("addedText");
}

bool isShapeCandidateKind(const QString &kind)
{
    return kind == QLatin1String("addedShape");
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

QString shapeLabel(PDFEditController::ShapeType shapeType)
{
    switch (shapeType) {
    case PDFEditController::RectangleShape:
        //% "Rectangle"
        return qtTrId("RectangleShape");
    case PDFEditController::EllipseShape:
        //% "Oval"
        return qtTrId("OvalShape");
    case PDFEditController::LineShape:
        //% "Line"
        return qtTrId("LineShape");
    case PDFEditController::ArrowShape:
        //% "Arrow"
        return qtTrId("ArrowShape");
    case PDFEditController::TriangleShape:
        //% "Triangle"
        return qtTrId("TriangleShape");
    case PDFEditController::DiamondShape:
        //% "Diamond"
        return qtTrId("DiamondShape");
    }
    return {};
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

bool polygonContainsPoint(const QPolygonF &polygon,
                          const QPointF &point,
                          qreal tolerance,
                          bool includeInterior)
{
    if (includeInterior
        && polygon.containsPoint(point, Qt::OddEvenFill)) {
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

bool shapeDragIsLongEnough(const QSizeF &pageSize,
                           const QPointF &startPoint,
                           const QPointF &endPoint)
{
    return !pageSize.isEmpty()
        && std::hypot(
               (endPoint.x() - startPoint.x()) * pageSize.width(),
               (endPoint.y() - startPoint.y()) * pageSize.height())
            >= kMinimumShapeDragPoints;
}

bool shapeContainsPoint(PDFEditController::ShapeType shapeType,
                        const QRectF &normalizedFrame,
                        const QPointF &startRatio,
                        const QPointF &endRatio,
                        qreal strokeWidthRatio,
                        const QColor &fillColor,
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
    const bool includeInterior = fillColor.isValid()
        && fillColor.alpha() > 0;

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
            tolerance,
            includeInterior);
    }
    case PDFEditController::EllipseShape: {
        const qreal radiusX = drawingRect.width() / 2.0;
        const qreal radiusY = drawingRect.height() / 2.0;
        const qreal deltaX = point.x() - drawingRect.center().x();
        const qreal deltaY = point.y() - drawingRect.center().y();
        const qreal outerX = deltaX / (radiusX + tolerance);
        const qreal outerY = deltaY / (radiusY + tolerance);
        const bool insideOuter
            = outerX * outerX + outerY * outerY <= 1.0;
        if (!insideOuter
            || includeInterior
            || radiusX <= tolerance
            || radiusY <= tolerance) {
            return insideOuter;
        }

        const qreal innerX = deltaX / (radiusX - tolerance);
        const qreal innerY = deltaY / (radiusY - tolerance);
        return innerX * innerX + innerY * innerY >= 1.0;
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
            tolerance,
            includeInterior);
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
            tolerance,
            includeInterior);
    }
    }
    return false;
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

bool supportsCommentState(const QString &kind)
{
    return isGeometryCommentCandidateKind(kind);
}

int commentAnnotationSubtype(const QString &kind)
{
    if (kind == QLatin1String("underline")) {
        return FPDF_ANNOT_UNDERLINE;
    }
    if (kind == QLatin1String("squiggly")) {
        return FPDF_ANNOT_SQUIGGLY;
    }
    if (kind == QLatin1String("strikeout")) {
        return FPDF_ANNOT_STRIKEOUT;
    }
    if (kind == QLatin1String("stickyNote")) {
        return FPDF_ANNOT_TEXT;
    }
    if (kind == QLatin1String("freeText")) {
        return FPDF_ANNOT_FREETEXT;
    }
    if (kind == QLatin1String("stamp")) {
        return FPDF_ANNOT_STAMP;
    }
    if (kind == QLatin1String("square")) {
        return FPDF_ANNOT_SQUARE;
    }
    if (kind == QLatin1String("circle")) {
        return FPDF_ANNOT_CIRCLE;
    }
    if (kind == QLatin1String("line")) {
        return FPDF_ANNOT_LINE;
    }
    if (kind == QLatin1String("polygon")) {
        return FPDF_ANNOT_POLYGON;
    }
    if (kind == QLatin1String("polyLine")) {
        return FPDF_ANNOT_POLYLINE;
    }
    if (kind == QLatin1String("ink")) {
        return FPDF_ANNOT_INK;
    }
    return kind == QLatin1String("highlight")
        ? FPDF_ANNOT_HIGHLIGHT
        : FPDF_ANNOT_UNKNOWN;
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

bool draftSuppressesSourceObjects(const QString &kind, bool deleted)
{
    return kind == QLatin1String("text")
        || (kind == QLatin1String("image") && deleted);
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

FS_MATRIX combinedTransform(const FS_MATRIX &parent, const FS_MATRIX &local)
{
    return FS_MATRIX{
        static_cast<float>(parent.a * local.a + parent.c * local.b),
        static_cast<float>(parent.b * local.a + parent.d * local.b),
        static_cast<float>(parent.a * local.c + parent.c * local.d),
        static_cast<float>(parent.b * local.c + parent.d * local.d),
        static_cast<float>(parent.a * local.e + parent.c * local.f + parent.e),
        static_cast<float>(parent.b * local.e + parent.d * local.f + parent.f)};
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

bool transformedPointToDevice(FPDF_PAGE page,
                              const QSize &deviceSize,
                              const FS_MATRIX &transform,
                              const QPointF &point,
                              QPointF &devicePoint)
{
    return pagePointToDevice(page, deviceSize, mapPoint(transform, point), devicePoint);
}

bool supportsInlineTextLayout(FPDF_PAGE page,
                              const QSizeF &pageSize,
                              const FS_MATRIX &transform)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF baselinePoint;
    QPointF verticalPoint;
    if (deviceSize.isEmpty()
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(1.0, 0.0), baselinePoint)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 1.0), verticalPoint)) {
        return false;
    }

    const QPointF baseline = baselinePoint - origin;
    const QPointF vertical = verticalPoint - origin;
    const qreal baselineLength = std::hypot(baseline.x(), baseline.y());
    const qreal verticalLength = std::hypot(vertical.x(), vertical.y());
    if (baselineLength <= 0.0 || verticalLength <= 0.0) {
        return false;
    }

    return baseline.x() > 0.0
        && qAbs(baseline.y()) <= baselineLength * 0.05
        && vertical.y() < 0.0
        && qAbs(vertical.x()) <= verticalLength * 0.10;
}

qreal normalizedFontSize(FPDF_PAGE page,
                         const QSizeF &pageSize,
                         const FS_MATRIX &transform,
                         qreal fontSize)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF fontTop;
    if (fontSize <= 0.0
        || deviceSize.isEmpty()
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, fontSize), fontTop)) {
        return 0.0;
    }

    return std::hypot(fontTop.x() - origin.x(), fontTop.y() - origin.y())
        / deviceSize.height();
}

qreal horizontalScaleForTransform(FPDF_PAGE page,
                                  const QSizeF &pageSize,
                                  const FS_MATRIX &transform)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF baselinePoint;
    QPointF verticalPoint;
    if (deviceSize.isEmpty()
        || !transformedPointToDevice(
            page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(
            page, deviceSize, transform, QPointF(1.0, 0.0), baselinePoint)
        || !transformedPointToDevice(
            page, deviceSize, transform, QPointF(0.0, 1.0), verticalPoint)) {
        return 1.0;
    }

    const QPointF baseline = baselinePoint - origin;
    const QPointF vertical = verticalPoint - origin;
    const qreal baselineLength = std::hypot(baseline.x(), baseline.y());
    const qreal verticalLength = std::hypot(vertical.x(), vertical.y());
    if (!qIsFinite(baselineLength)
        || !qIsFinite(verticalLength)
        || baselineLength <= 0.0
        || verticalLength <= 0.0) {
        return 1.0;
    }

    const qreal horizontalScale = baselineLength / verticalLength;
    return qIsFinite(horizontalScale) && horizontalScale > 0.0
        ? horizontalScale
        : 1.0;
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

bool textMarkupAngleIsSupported(float angle)
{
    if (!std::isfinite(angle) || angle < 0.0F) {
        return false;
    }
    const qreal normalizedAngle = std::fmod(
        static_cast<qreal>(angle),
        kFullTurnRadians);
    const qreal uprightDistance = qMin(
        normalizedAngle,
        kFullTurnRadians - normalizedAngle);
    return uprightDistance <= kTextMarkupMaximumAngleRadians;
}

QString textObjectText(FPDF_PAGEOBJECT object, FPDF_TEXTPAGE textPage)
{
    if (!object || !textPage) {
        return {};
    }

    const unsigned long byteLength = FPDFTextObj_GetText(object, textPage, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR)
        || byteLength > kMaximumAnnotationStringBytes) {
        return {};
    }

    const qsizetype unitCount = static_cast<qsizetype>(
        (byteLength + sizeof(FPDF_WCHAR) - 1) / sizeof(FPDF_WCHAR));
    QVector<FPDF_WCHAR> buffer(unitCount);
    const unsigned long actualLength = FPDFTextObj_GetText(
        object,
        textPage,
        buffer.data(),
        byteLength);
    if (actualLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    qsizetype characterCount = static_cast<qsizetype>(actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
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

QString pdfFontName(FPDF_FONT font, bool baseName)
{
    if (!font) {
        return {};
    }

    const size_t length = baseName
        ? FPDFFont_GetBaseFontName(font, nullptr, 0)
        : FPDFFont_GetFamilyName(font, nullptr, 0);
    if (length == 0) {
        return {};
    }

    QByteArray buffer(static_cast<qsizetype>(length), '\0');
    const size_t actualLength = baseName
        ? FPDFFont_GetBaseFontName(font, buffer.data(), length)
        : FPDFFont_GetFamilyName(font, buffer.data(), length);
    if (actualLength == 0) {
        return {};
    }

    return QString::fromUtf8(buffer.constData()).trimmed();
}

QString normalizedPdfFontIdentity(FPDF_FONT font)
{
    const QString baseName = pdfFontName(font, true);
    return baseName.size() > 7 && baseName.at(6) == QLatin1Char('+')
        ? baseName.sliced(7)
        : baseName;
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

bool equivalentFontFamilyName(const QString &left, const QString &right)
{
    const QString leftKey = fontFamilyMatchKey(left);
    return sameFontFamily(left, right)
        || (!leftKey.isEmpty() && leftKey == fontFamilyMatchKey(right));
}

QString matchingKnownFontFamilyName(const QString &family,
                                    const QStringList &knownFamilies)
{
    for (const QString &knownFamily : knownFamilies) {
        if (family.compare(knownFamily, Qt::CaseInsensitive) == 0) {
            return knownFamily;
        }
    }

    const QString key = fontFamilyMatchKey(family);
    for (const QString &knownFamily : knownFamilies) {
        if (!key.isEmpty() && key == fontFamilyMatchKey(knownFamily)) {
            return knownFamily;
        }
    }

    const QStringList nameParts = family.split(
        QLatin1Char('-'),
        Qt::SkipEmptyParts);
    for (const QString &namePart : nameParts) {
        const QString partKey = fontFamilyMatchKey(namePart);
        for (const QString &knownFamily : knownFamilies) {
            if (!partKey.isEmpty()
                && partKey == fontFamilyMatchKey(knownFamily)) {
                return knownFamily;
            }
        }
    }
    return {};
}

QString matchingBundledFamilyName(const QString &family)
{
    return matchingKnownFontFamilyName(
        family,
        ApplicationFonts::bundledFontFamilies());
}

QString matchingSystemFamilyName(const QString &family)
{
    return matchingKnownFontFamilyName(
        family,
        ApplicationFonts::systemFontFamilies());
}

bool isArialCompatibleFontFamily(const QString &family)
{
    const auto isCompatibleKey = [](const QString &key) {
        return key.contains(QStringLiteral("arial"))
            || key.contains(QStringLiteral("helvetica"))
            || key == QLatin1String("sans")
            || key == QLatin1String("sansserif");
    };
    if (isCompatibleKey(fontFamilyMatchKey(family))) {
        return true;
    }
    const QStringList nameParts = family.split(
        QLatin1Char('-'),
        Qt::SkipEmptyParts);
    return std::any_of(
        nameParts.cbegin(),
        nameParts.cend(),
        [&isCompatibleKey](const QString &namePart) {
            return isCompatibleKey(fontFamilyMatchKey(namePart));
        });
}

bool isGenericArialCompatibleFontFamily(const QString &family)
{
    const QString key = fontFamilyMatchKey(family);
    return key == QLatin1String("arial")
        || key == QLatin1String("helvetica")
        || key == QLatin1String("sans")
        || key == QLatin1String("sansserif");
}

bool containsNonAscii(const QString &text)
{
    return std::any_of(
        text.cbegin(),
        text.cend(),
        [](QChar character) {
            return character.unicode() > 0x7f;
        });
}

bool pdfFontIsBold(FPDF_FONT font)
{
    if (!font) {
        return false;
    }

    const int weight = FPDFFont_GetWeight(font);
    if (weight >= static_cast<int>(QFont::Bold)) {
        return true;
    }

    const int flags = FPDFFont_GetFlags(font);
    if (flags >= 0 && (flags & kPdfFontFlagForceBold) != 0) {
        return true;
    }

    const QString baseName = pdfFontName(font, true);
    return baseName.contains(QStringLiteral("bold"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("black"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("heavy"), Qt::CaseInsensitive);
}

bool pdfFontIsItalic(FPDF_FONT font)
{
    if (!font) {
        return false;
    }

    int italicAngle = 0;
    if (FPDFFont_GetItalicAngle(font, &italicAngle) && italicAngle != 0) {
        return true;
    }

    const int flags = FPDFFont_GetFlags(font);
    if (flags >= 0 && (flags & kPdfFontFlagItalic) != 0) {
        return true;
    }

    const QString baseName = pdfFontName(font, true);
    return baseName.contains(QStringLiteral("italic"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("oblique"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("slanted"), Qt::CaseInsensitive);
}

QString matchingFontFamily(FPDF_FONT font, const QStringList &availableFamilies)
{
    QStringList pdfNames;
    const auto appendUniqueName = [&pdfNames](const QString &name) {
        if (!name.isEmpty() && !pdfNames.contains(name, Qt::CaseInsensitive)) {
            pdfNames.append(name);
        }
    };
    appendUniqueName(pdfFontName(font, false));
    appendUniqueName(normalizedPdfFontIdentity(font));

    for (const QString &pdfName : std::as_const(pdfNames)) {
        for (const QString &availableFamily : availableFamilies) {
            if (pdfName.compare(availableFamily, Qt::CaseInsensitive) == 0) {
                return availableFamily;
            }
        }
    }

    QSet<QString> pdfNameKeys;
    for (const QString &pdfName : std::as_const(pdfNames)) {
        const QString key = fontFamilyMatchKey(pdfName);
        if (!key.isEmpty()) {
            pdfNameKeys.insert(key);
        }
    }
    for (const QString &availableFamily : availableFamilies) {
        if (pdfNameKeys.contains(fontFamilyMatchKey(availableFamily))) {
            return availableFamily;
        }
    }
    return {};
}

bool fontSupportsText(const QString &family, const QString &text)
{
    if (family.isEmpty()) {
        return false;
    }

    QFont font(family);
    font.setStyleStrategy(QFont::NoFontMerging);
    const QFontMetricsF metrics(font);
    for (uint character : text.toUcs4()) {
        if (!QChar::isSpace(static_cast<char32_t>(character))
            && !metrics.inFontUcs4(character)) {
            return false;
        }
    }
    return true;
}

QColor pageObjectColor(FPDF_PAGEOBJECT object, bool stroke)
{
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int alpha = 255;
    const FPDF_BOOL foundColor = stroke
        ? FPDFPageObj_GetStrokeColor(object, &red, &green, &blue, &alpha)
        : FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha);
    if (!foundColor) {
        return Qt::black;
    }
    return QColor::fromRgb(red, green, blue, alpha);
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

QColor textColor(FPDF_PAGEOBJECT object, FPDF_TEXT_RENDERMODE renderMode)
{
    const bool strokeOnly = renderMode == FPDF_TEXTRENDERMODE_STROKE
        || renderMode == FPDF_TEXTRENDERMODE_STROKE_CLIP;
    return pageObjectColor(object, strokeOnly);
}

bool isSuppressibleTextRenderMode(FPDF_TEXT_RENDERMODE renderMode)
{
    return renderMode == FPDF_TEXTRENDERMODE_FILL
        || renderMode == FPDF_TEXTRENDERMODE_STROKE
        || renderMode == FPDF_TEXTRENDERMODE_FILL_STROKE;
}

QString pageObjectId(const QVector<int> &objectPath)
{
    QStringList pathSegments;
    pathSegments.reserve(objectPath.size());
    for (int index : objectPath) {
        pathSegments.append(QString::number(index));
    }
    return QStringLiteral("object:") + pathSegments.join(QLatin1Char('/'));
}

QString saveFailureText(const PDFSaveWriter::Result &result)
{
    switch (result.error) {
    case PDFSaveWriter::Error::None:
        return {};
    case PDFSaveWriter::Error::SourceUnavailable:
        //% "No PDF is open."
        return qtTrId("NoPdfOpenForSaving");
    case PDFSaveWriter::Error::SourceOpenFailed:
        //% "The source PDF could not be opened for saving."
        return qtTrId("PdfSaveSourceOpenFailed");
    case PDFSaveWriter::Error::FormSnapshotFailed:
        //% "The PDF could not be saved."
        return qtTrId("PdfSaveFailed");
    case PDFSaveWriter::Error::PageLoadFailed:
        //% "Page %1 could not be loaded while saving."
        return qtTrId("PdfSavePageLoadFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::UnsupportedSourceObject:
        //% "Page %1 contains edited text that cannot be saved safely yet."
        return qtTrId("PdfSaveUnsupportedSourceObject").arg(result.page + 1);
    case PDFSaveWriter::Error::SourceObjectMissing:
        //% "An edited object could not be found on page %1."
        return qtTrId("PdfSaveSourceObjectMissing").arg(result.page + 1);
    case PDFSaveWriter::Error::SourceObjectRemovalFailed:
        //% "An original object could not be removed or replaced on page %1."
        return qtTrId("PdfSaveObjectRemovalFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::FontUnavailable: {
        if (result.unsupportedCodePoint != 0) {
            const char32_t codePoint = static_cast<char32_t>(
                result.unsupportedCodePoint);
            const QString character = QString::fromUcs4(&codePoint, 1);
            const QString unicodeName = QStringLiteral("U+%1")
                .arg(
                    result.unsupportedCodePoint,
                    4,
                    16,
                    QLatin1Char('0'))
                .toUpper();
            //% "The font \u201c%1\u201d does not support the character \u201c%2\u201d (%3). Choose a different font."
            return qtTrId("PdfSaveFontMissingCharacter")
                .arg(result.detail, character, unicodeName);
        }
        //% "The font %1 cannot be embedded in the saved PDF."
        return qtTrId("PdfSaveFontUnavailable").arg(result.detail);
    }
    case PDFSaveWriter::Error::TextObjectCreationFailed:
        //% "Saved text could not be created on page %1."
        return qtTrId("PdfSaveTextCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::ImageObjectCreationFailed:
        //% "The image could not be created on page %1."
        return qtTrId("PdfSaveImageCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::ShapeObjectCreationFailed:
        //% "The shape could not be created on page %1."
        return qtTrId("PdfSaveShapeCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::TextMarkupAnnotationCreationFailed:
        //% "The text markup could not be created on page %1."
        return qtTrId("PdfSaveTextMarkupCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::CommentAnnotationCreationFailed:
        //% "The comment could not be created on page %1."
        return qtTrId("PdfSaveCommentCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::CommentAnnotationUpdateFailed:
        //% "The comment could not be updated on page %1."
        return qtTrId("PdfSaveCommentUpdateFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::CommentAnnotationRemovalFailed:
        //% "The comment could not be deleted on page %1."
        return qtTrId("PdfSaveCommentRemovalFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::PageCreationFailed:
        //% "Blank page %1 could not be created while saving."
        return qtTrId("PdfSavePageCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::PageDeletionFailed:
        //% "Page %1 could not be deleted while saving."
        return qtTrId("PdfSavePageDeletionFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::PageContentGenerationFailed:
        //% "Page %1 could not be finalized for saving."
        return qtTrId("PdfSavePageGenerationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::AcroFormCreationFailed:
        //% "The AcroForm fields could not be created: %1"
        return qtTrId("PdfSaveAcroFormCreationFailed").arg(result.detail);
    case PDFSaveWriter::Error::OutputOpenFailed:
        //% "The destination file could not be opened: %1"
        return qtTrId("PdfSaveOutputOpenFailed").arg(result.detail);
    case PDFSaveWriter::Error::OutputWriteFailed:
        //% "The PDF could not be written: %1"
        return qtTrId("PdfSaveOutputWriteFailed").arg(result.detail);
    case PDFSaveWriter::Error::OutputCommitFailed:
        //% "The saved PDF could not replace the destination file: %1"
        return qtTrId("PdfSaveOutputCommitFailed").arg(result.detail);
    }
    //% "The PDF could not be saved."
    return qtTrId("PdfSaveFailed");
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

bool PDFEditController::hasDrafts() const
{
    return !m_drafts.isEmpty()
        || !m_pendingSquigglyCreations.isEmpty()
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

QStringList PDFEditController::bundledFontFamilies() const
{
    return ApplicationFonts::bundledFontFamilies();
}

QStringList PDFEditController::availableFontFamilies() const
{
    return ApplicationFonts::availableFontFamilies(
        selectedFontBold(),
        selectedFontItalic());
}

QStringList PDFEditController::fontFamiliesForFilter(FontFilter filter) const
{
    struct FontRequirement {
        bool bold = false;
        bool italic = false;
    };

    QVector<FontRequirement> requirements;
    const auto appendRequirement = [&requirements](
                                       const CharacterFormat &format) {
        const auto existing = std::find_if(
            requirements.begin(),
            requirements.end(),
            [&format](const FontRequirement &requirement) {
                return requirement.bold == format.fontBold
                    && requirement.italic == format.fontItalic;
            });
        if (existing == requirements.end()) {
            requirements.append(FontRequirement{
                format.fontBold,
                format.fontItalic});
        }
    };

    if (m_hasSelection) {
        const Draft draft = selectedDraftForDisplay();
        qsizetype rangeStart = 0;
        qsizetype rangeLength = 0;
        selectedTextRangeForDraft(draft, rangeStart, rangeLength);
        const qsizetype rangeEnd = rangeStart + rangeLength;
        for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
            const qsizetype spanStart = qMax<qsizetype>(rangeStart, span.start);
            const qsizetype spanEnd = qMin<qsizetype>(
                rangeEnd,
                span.start + span.length);
            if (spanEnd > spanStart) {
                appendRequirement(span.format);
            }
        }
        if (requirements.isEmpty()) {
            appendRequirement(primaryCharacterFormat(draft));
        }
        if (m_textEditActive
            && m_selectedTextRangeLength == 0
            && m_typingCharacterFormatActive) {
            appendRequirement(m_typingCharacterFormat);
        }
    } else {
        requirements.append(FontRequirement{
            selectedFontBold(),
            selectedFontItalic()});
    }

    const auto familiesForRequirement = [filter](
                                            const FontRequirement &requirement) {
        switch (filter) {
        case VietnameseFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Vietnamese,
                requirement.bold,
                requirement.italic));
        case EnglishFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::English,
                requirement.bold,
                requirement.italic));
        case ChineseFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Chinese,
                requirement.bold,
                requirement.italic));
        case JapaneseFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Japanese,
                requirement.bold,
                requirement.italic));
        case KoreanFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Korean,
                requirement.bold,
                requirement.italic));
        }
        return QStringList(ApplicationFonts::availableFontFamilies(
            requirement.bold,
            requirement.italic));
    };

    QStringList families;
    bool firstRequirement = true;
    for (const FontRequirement &requirement : std::as_const(requirements)) {
        const QStringList requirementFamilies = familiesForRequirement(
            requirement);
        if (firstRequirement) {
            families = requirementFamilies;
            firstRequirement = false;
            continue;
        }
        families.erase(
            std::remove_if(
                families.begin(),
                families.end(),
                [&requirementFamilies](const QString &family) {
                    return !requirementFamilies.contains(
                        family,
                        Qt::CaseInsensitive);
                }),
            families.end());
    }
    return families;
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

QVariantList PDFEditController::selectedCharacterSpans() const
{
    return m_hasSelection
        ? characterSpanMaps(selectedDraftForDisplay().characterSpans)
        : QVariantList();
}

QVariantList PDFEditController::selectedParagraphFormats() const
{
    return m_hasSelection
        ? paragraphFormatMaps(selectedDraftForDisplay().paragraphFormats)
        : QVariantList();
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

QString PDFEditController::selectedText() const
{
    return m_hasSelection ? m_selectedText : QString();
}

QString PDFEditController::selectedFontFamily() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().fontFamily
        : QString();
}

qreal PDFEditController::selectedFontSizeRatio() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().fontSizeRatio
        : 0.0;
}

qreal PDFEditController::selectedDisplayFontSizeRatio() const
{
    if (!m_hasSelection) {
        return 0.0;
    }
    const CharacterFormat format = selectedCharacterFormatForDisplay();
    return format.displayFontSizeRatio > 0.0
        ? format.displayFontSizeRatio
        : format.fontSizeRatio;
}

int PDFEditController::selectedFontStretch() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().fontStretch
        : QFont::Unstretched;
}

bool PDFEditController::selectedFontBold() const
{
    return m_hasSelection && selectedCharacterFormatForDisplay().fontBold;
}

bool PDFEditController::selectedFontItalic() const
{
    return m_hasSelection && selectedCharacterFormatForDisplay().fontItalic;
}

bool PDFEditController::selectedFontUnderline() const
{
    return m_hasSelection && selectedCharacterFormatForDisplay().fontUnderline;
}

QColor PDFEditController::selectedColor() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().color
        : QColor(Qt::black);
}

int PDFEditController::selectedParagraphAlignment() const
{
    if (!m_hasSelection || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return -1;
    }

    const Draft draft = selectedDraftForDisplay();
    qsizetype first = -1;
    qsizetype last = -1;
    selectedParagraphRangeForDraft(draft, first, last);
    if (first < 0 || last < first) {
        return -1;
    }

    const ParagraphAlignment alignment = draft.paragraphFormats.at(first).alignment;
    for (qsizetype index = first + 1; index <= last; ++index) {
        if (draft.paragraphFormats.at(index).alignment != alignment) {
            return -1;
        }
    }
    return static_cast<int>(alignment);
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

bool PDFEditController::commentStateEditable(
    const Candidate &candidate) const
{
    const int protectedFlags = FPDF_ANNOT_FLAG_READONLY
        | FPDF_ANNOT_FLAG_LOCKED
        | kAnnotationFlagLockedContents;
    return isCommentCandidateKind(candidate.kind)
        && supportsCommentState(candidate.kind)
        && !candidate.commentStateAnnotation
        && !candidate.isReplyAnnotation
        && candidate.replyToObjectId.isEmpty()
        && documentAllowsAnnotations()
        && (candidate.commentProperties.flags & protectedFlags) == 0
        && !candidate.commentTextReadOnly
        && (candidate.added || candidate.sourceAnnotationIndex >= 0);
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
    return isDrawingCommentCandidateKind(candidate.kind)
        && candidate.editable
        && candidate.drawingPropertiesEditable
        && documentAllowsAnnotations();
}

bool PDFEditController::selectedDeletable() const
{
    return m_hasSelection && candidateDeletable(m_selectedCandidate);
}

bool PDFEditController::selectedDrawingPropertiesEditable() const
{
    return m_hasSelection
        && drawingPropertiesEditable(m_selectedCandidate);
}

bool PDFEditController::selectedDrawingSupportsFill() const
{
    return m_hasSelection
        && drawingSupportsFill(m_selectedCandidate.kind);
}

QColor PDFEditController::selectedDrawingStrokeColor() const
{
    return m_hasSelection
            && isDrawingCommentCandidateKind(m_selectedCandidate.kind)
        ? selectedDraftForDisplay().commentProperties.strokeColor
        : QColor();
}

QColor PDFEditController::selectedDrawingInteriorColor() const
{
    return m_hasSelection
            && drawingSupportsFill(m_selectedCandidate.kind)
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
            && isDrawingCommentCandidateKind(m_selectedCandidate.kind)
        ? selectedDraftForDisplay().commentProperties.opacity
        : 1.0;
}

QString PDFEditController::selectedReason() const
{
    return m_hasSelection ? m_selectedCandidate.reason : QString();
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

quint64 PDFEditController::draftRevisionForPage(int page) const
{
    return m_pageDraftRevisions.value(page, 0);
}

QVector<QVector<int>> PDFEditController::draftedTextObjectPathsForPage(int page) const
{
    QVector<QVector<int>> objectPaths;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && draftSuppressesSourceObjects(candidate.kind, draft->deleted)
            && !isSourceTextMoveOnly(*draft)
            && candidate.renderSuppressible) {
            if (!candidate.parts.isEmpty()) {
                for (const CandidatePart &part : candidate.parts) {
                    if (!part.objectPath.isEmpty()) {
                        objectPaths.append(part.objectPath);
                    }
                }
            } else if (!candidate.objectPath.isEmpty()) {
                objectPaths.append(candidate.objectPath);
            }
        }
    }

    std::sort(objectPaths.begin(), objectPaths.end(), [](const QVector<int> &left,
                                                         const QVector<int> &right) {
        return std::lexicographical_compare(
            left.cbegin(), left.cend(), right.cbegin(), right.cend());
    });
    objectPaths.erase(std::unique(objectPaths.begin(), objectPaths.end()), objectPaths.end());
    return objectPaths;
}

QVector<PDFEditController::DraftObjectMove>
PDFEditController::draftedObjectMovesForPage(int page) const
{
    QVector<DraftObjectMove> moves;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page != page || draft->deleted
            || !candidate.renderSuppressible) {
            continue;
        }

        if (candidate.kind == QLatin1String("image")
            && candidate.objectPath.size() == 1
            && candidate.geometry.sourceRect.topLeft()
                != draft->frameRect.topLeft()) {
            moves.append(DraftObjectMove{
                DraftObjectMove::Kind::Image,
                candidate.objectPath,
                candidate.geometry.sourceRect,
                draft->frameRect});
            continue;
        }

        if (!isSourceTextMoveOnly(*draft)) {
            continue;
        }

        const QRectF initialFrameRect
            = (candidate.initialDraftFrameRect.isEmpty()
                   ? candidate.geometry.sourceRect
                   : candidate.initialDraftFrameRect)
                  .normalized();
        const QRectF movedFrameRect = draft->frameRect.normalized();
        const auto appendTextMove = [
            &moves,
            &initialFrameRect,
            &movedFrameRect](const QVector<int> &objectPath) {
            if (objectPath.size() != 1) {
                return;
            }
            moves.append(DraftObjectMove{
                DraftObjectMove::Kind::Text,
                objectPath,
                initialFrameRect,
                movedFrameRect});
        };
        if (candidate.parts.isEmpty()) {
            appendTextMove(candidate.objectPath);
        } else {
            for (const CandidatePart &part : candidate.parts) {
                appendTextMove(part.objectPath);
            }
        }
    }

    std::sort(moves.begin(), moves.end(), [](const DraftObjectMove &left,
                                             const DraftObjectMove &right) {
        return std::lexicographical_compare(
            left.objectPath.cbegin(), left.objectPath.cend(),
            right.objectPath.cbegin(), right.objectPath.cend());
    });
    return moves;
}

QVector<PDFEditController::DraftAnnotationDeletion>
PDFEditController::deletedSourceCommentAnnotationsForPage(int page) const
{
    QVector<DraftAnnotationDeletion> deletions;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        const bool movesGeometryComment
            = isGeometryCommentCandidateKind(candidate.kind)
            && draft->frameRect
                != initialDraftForCandidate(candidate).frameRect;
        const bool changesDrawingProperties
            = isDrawingCommentCandidateKind(candidate.kind)
            && draft->commentProperties != candidate.commentProperties;
        const bool suppressesSourceAppearance
            = draft->deleted
            || candidate.kind == QLatin1String("freeText")
            || movesGeometryComment
            || changesDrawingProperties;
        if (candidate.page != page
            || candidate.added
            || !suppressesSourceAppearance
            || !isCommentCandidateKind(candidate.kind)
            || candidate.sourceAnnotationIndex < 0) {
            continue;
        }

        const int subtype = commentAnnotationSubtype(candidate.kind);
        if (subtype != FPDF_ANNOT_UNKNOWN) {
            deletions.append(DraftAnnotationDeletion{
                candidate.sourceAnnotationIndex,
                subtype});
        }
    }
    if (m_frameResizeActive
        && m_hasSelection
        && m_selectedCandidate.page == page
        && isGeometryCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.added
        && m_selectedCandidate.sourceAnnotationIndex >= 0) {
        deletions.append(DraftAnnotationDeletion{
            m_selectedCandidate.sourceAnnotationIndex,
            commentAnnotationSubtype(m_selectedCandidate.kind)});
    }

    std::sort(
        deletions.begin(),
        deletions.end(),
        [](const DraftAnnotationDeletion &left,
           const DraftAnnotationDeletion &right) {
            return left.annotationIndex < right.annotationIndex;
        });
    deletions.erase(
        std::unique(
            deletions.begin(),
            deletions.end(),
            [](const DraftAnnotationDeletion &left,
               const DraftAnnotationDeletion &right) {
                return left.annotationIndex == right.annotationIndex;
            }),
        deletions.end());
    return deletions;
}

bool PDFEditController::allDraftedTextObjectsSuppressibleForPage(int page) const
{
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && draftSuppressesSourceObjects(candidate.kind, draft->deleted)
            && !isSourceTextMoveOnly(*draft)
            && !candidate.renderSuppressible) {
            return false;
        }
    }
    return true;
}

QVariantList PDFEditController::candidatesForPage(int page)
{
    QVariantList result;
    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    result.reserve(candidates.size() + m_addedCandidates.size());
    for (const Candidate &candidate : candidates) {
        if (candidate.textMarkupHitTestOnly) {
            continue;
        }
        if (isDesignerManagedSourceWidget(candidate)) {
            continue;
        }
        if (candidate.isReplyAnnotation) {
            continue;
        }
        const QString key = draftKey(candidate.page, candidate.objectId);
        const auto draft = m_drafts.constFind(key);
        if (draft != m_drafts.cend()
            && draftRepresentsDeletion(candidate.kind, draft->text, draft->deleted)) {
            continue;
        }
        const Draft displayDraft = draft == m_drafts.cend()
            ? initialDraftForCandidate(candidate)
            : *draft;
        result.append(candidateMap(displayDraft));
    }
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page != page) {
            continue;
        }
        if (candidate.isReplyAnnotation) {
            continue;
        }

        const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
        if (draft != m_drafts.cend()
            && draftRepresentsDeletion(candidate.kind, draft->text, draft->deleted)) {
            continue;
        }
        const Draft displayDraft = draft == m_drafts.cend()
            ? initialDraftForCandidate(candidate)
            : *draft;
        result.append(candidateMap(displayDraft));
    }
    return result;
}

void PDFEditController::requestCandidatesForPage(int page)
{
    ensurePageCandidates(page);
}

QVariantList PDFEditController::draftsForPage(int page) const
{
    QVariantList result;
    const auto pageCandidates = m_pageCandidates.constFind(page);
    if (pageCandidates != m_pageCandidates.cend()) {
        for (const Candidate &candidate : pageCandidates.value()) {
            if (candidate.isReplyAnnotation) {
                continue;
            }
            const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
            if (draft == m_drafts.cend()) {
                continue;
            }

            const Draft originalDraft = initialDraftForCandidate(candidate);
            QVariantMap map = candidateMap(*draft);
            map.insert(QStringLiteral("originalText"), originalDraft.text);
            map.insert(
                QStringLiteral("originalCharacterSpans"),
                characterSpanMaps(originalDraft.characterSpans));
            map.insert(
                QStringLiteral("originalParagraphFormats"),
                paragraphFormatMaps(originalDraft.paragraphFormats));
            map.insert(QStringLiteral("originalFrameRect"), originalDraft.frameRect);
            map.insert(QStringLiteral("originalContentBounds"), originalDraft.contentBounds);
            if (isDrawingCommentCandidateKind(candidate.kind)) {
                map.insert(
                    QStringLiteral("originalPreview"),
                    candidateMap(originalDraft));
            }
            result.append(map);
        }
    }

    for (const Candidate &candidate : m_addedCandidates) {
        if (candidate.page != page) {
            continue;
        }
        if (candidate.isReplyAnnotation) {
            continue;
        }

        const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
        if (draft == m_drafts.cend()) {
            continue;
        }
        const Draft originalDraft = initialDraftForCandidate(candidate);
        QVariantMap map = candidateMap(*draft);
        map.insert(QStringLiteral("originalText"), originalDraft.text);
        map.insert(
            QStringLiteral("originalCharacterSpans"),
            characterSpanMaps(originalDraft.characterSpans));
        map.insert(
            QStringLiteral("originalParagraphFormats"),
            paragraphFormatMaps(originalDraft.paragraphFormats));
        map.insert(QStringLiteral("originalFrameRect"), originalDraft.frameRect);
        map.insert(QStringLiteral("originalContentBounds"), originalDraft.contentBounds);
        result.append(map);
    }
    return result;
}

QVariantMap PDFEditController::selectedDraftPreview() const
{
    return m_hasSelection
        ? candidateMap(selectedDraftForDisplay())
        : QVariantMap();
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
                candidate.fillColor,
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

QVariantMap PDFEditController::addText(int page, qreal x, qreal y)
{
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }

    finishSelectedTextEdit();

    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("added:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedText");
    //% "Add Text"
    candidate.label = qtTrId("AddText");
    candidate.text.clear();
    const QStringList bundledFamilies = ApplicationFonts::bundledFontFamilies();
    candidate.fontFamily = bundledFamilies.contains(
        QStringLiteral("Arial"),
        Qt::CaseInsensitive)
        ? QStringLiteral("Arial")
        : bundledFamilies.isEmpty()
            ? QFontInfo(QFont()).family()
            : bundledFamilies.constFirst();
    candidate.fontSizeRatio = kAddedTextFontSizeRatio;
    candidate.displayFontSizeRatio = candidate.fontSizeRatio;
    candidate.color = Qt::black;
    candidate.editable = true;
    candidate.added = true;

    const qreal normalizedX = qBound<qreal>(0.0, x, 1.0);
    const qreal normalizedY = qBound<qreal>(0.0, y, 1.0);
    const qreal frameX = qMin(
        normalizedX,
        1.0 - kAddedTextMinimumWidthRatio);
    const qreal frameWidth = qMin(
        kAddedTextWidthRatio,
        1.0 - frameX);
    const qreal frameY = qBound<qreal>(
        0.0,
        normalizedY - kAddedTextHeightRatio / 2.0,
        1.0 - kAddedTextHeightRatio);
    const QRectF sourceRect(
        frameX,
        frameY,
        frameWidth,
        kAddedTextHeightRatio);
    setInitialGeometry(candidate, sourceRect);

    m_addedCandidates.append(candidate);
    setSelection(&m_addedCandidates.constLast());
    emit pageDataChanged(page);
    beginSelectedTextEdit();
    return candidateMap(initialDraftForCandidate(candidate));
}

void PDFEditController::requestAddImage(int page, qreal x, qreal y)
{
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return;
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return;
    }

    emit imageFileRequested(
        page,
        qBound<qreal>(0.0, x, 1.0),
        qBound<qreal>(0.0, y, 1.0));
}

QVariantMap PDFEditController::addImage(int page,
                                        qreal x,
                                        qreal y,
                                        const QUrl &source)
{
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }
    if (!source.isLocalFile() || source.toLocalFile().isEmpty()) {
        //% "Only local image files are supported."
        emit statusMessage(qtTrId("LocalImageFilesOnly"));
        return {};
    }

    const QFileInfo imageFile(source.toLocalFile());
    if (!imageFile.isFile()
        || !imageFile.isReadable()
        || imageFile.size() <= 0) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }
    if (imageFile.size() > kMaximumAddedImageFileBytes) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }

    QImageReader reader(imageFile.absoluteFilePath());
    reader.setAutoTransform(true);
    const QSize encodedSize = reader.size();
    const auto exceedsPixelLimit = [](const QSize &size) {
        return size.isValid()
            && static_cast<qint64>(size.width()) * size.height()
                > kMaximumAddedImagePixels;
    };
    if (exceedsPixelLimit(encodedSize)) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }

    QImage image = reader.read();
    if (image.isNull()) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }
    if (exceedsPixelLimit(image.size())) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }
    image = image.convertToFormat(QImage::Format_ARGB32);

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }
    qreal frameWidth = kAddedImageMaximumDimensionRatio;
    qreal frameHeight = frameWidth
        * pageSize.width()
        / pageSize.height()
        * static_cast<qreal>(image.height())
        / image.width();
    if (frameHeight > kAddedImageMaximumDimensionRatio) {
        const qreal scale = kAddedImageMaximumDimensionRatio / frameHeight;
        frameWidth *= scale;
        frameHeight *= scale;
    }

    const qreal normalizedX = qBound<qreal>(0.0, x, 1.0);
    const qreal normalizedY = qBound<qreal>(0.0, y, 1.0);
    const qreal frameX = qBound<qreal>(
        0.0,
        normalizedX - frameWidth / 2.0,
        1.0 - frameWidth);
    const qreal frameY = qBound<qreal>(
        0.0,
        normalizedY - frameHeight / 2.0,
        1.0 - frameHeight);

    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("added-image:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedImage");
    //% "Image"
    candidate.label = qtTrId("Image");
    candidate.editable = true;
    candidate.added = true;
    candidate.image = std::move(image);
    setInitialGeometry(
        candidate,
        QRectF(frameX, frameY, frameWidth, frameHeight));

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

    setTool(SelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QVariantMap PDFEditController::addShape(int page,
                                        const QPointF &startPoint,
                                        const QPointF &endPoint)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }
    if (!std::isfinite(startPoint.x())
        || !std::isfinite(startPoint.y())
        || !std::isfinite(endPoint.x())
        || !std::isfinite(endPoint.y())
        || !isSupportedShapeType(m_shapeType)) {
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }

    const QPointF boundedStart(
        qBound<qreal>(0.0, startPoint.x(), 1.0),
        qBound<qreal>(0.0, startPoint.y(), 1.0));
    const QPointF boundedEnd(
        qBound<qreal>(0.0, endPoint.x(), 1.0),
        qBound<qreal>(0.0, endPoint.y(), 1.0));
    const QPointF shapeEnd = constrainedShapeEnd(
        page,
        boundedStart,
        boundedEnd);
    if (!shapeDragIsLongEnough(pageSize, boundedStart, shapeEnd)) {
        return {};
    }

    const qreal minimumWidth = qMin<qreal>(
        1.0,
        kMinimumShapeFramePoints / pageSize.width());
    const qreal minimumHeight = qMin<qreal>(
        1.0,
        kMinimumShapeFramePoints / pageSize.height());
    const auto expandedInterval = [](qreal first,
                                     qreal second,
                                     qreal minimumSize) {
        const qreal minimum = qMin(first, second);
        const qreal maximum = qMax(first, second);
        if (maximum - minimum >= minimumSize) {
            return QPair<qreal, qreal>(minimum, maximum);
        }
        const qreal center = (first + second) / 2.0;
        const qreal start = qBound<qreal>(
            0.0,
            center - minimumSize / 2.0,
            1.0 - minimumSize);
        return QPair<qreal, qreal>(start, start + minimumSize);
    };
    qreal frameLeft = qMin(boundedStart.x(), shapeEnd.x());
    qreal frameTop = qMin(boundedStart.y(), shapeEnd.y());
    qreal frameRight = qMax(boundedStart.x(), shapeEnd.x());
    qreal frameBottom = qMax(boundedStart.y(), shapeEnd.y());
    if (m_shapeType == LineShape || m_shapeType == ArrowShape) {
        const qreal strokeWidth = shapeStrokeWidthPoints(
            kAddedShapeStrokeWidthRatio,
            pageSize.height());
        const auto includeDevicePoint = [&frameLeft,
                                         &frameTop,
                                         &frameRight,
                                         &frameBottom,
                                         &pageSize](
                                            const QPointF &point) {
            const qreal normalizedX = point.x() / pageSize.width();
            const qreal normalizedY = point.y() / pageSize.height();
            frameLeft = qMin(frameLeft, normalizedX);
            frameTop = qMin(frameTop, normalizedY);
            frameRight = qMax(frameRight, normalizedX);
            frameBottom = qMax(frameBottom, normalizedY);
        };
        if (m_shapeType == ArrowShape) {
            const QPointF startDevice(
                boundedStart.x() * pageSize.width(),
                boundedStart.y() * pageSize.height());
            const QPointF endDevice(
                shapeEnd.x() * pageSize.width(),
                shapeEnd.y() * pageSize.height());
            const QPointF direction = endDevice - startDevice;
            const qreal length = std::hypot(
                direction.x(),
                direction.y());
            const QPointF unit = direction / length;
            const QPointF normal(-unit.y(), unit.x());
            const qreal headLength = qMin(
                qMax<qreal>(6.0, strokeWidth * 4.0),
                length * 0.35);
            const qreal halfWidth = headLength * 0.55;
            const QPointF headBase = endDevice - unit * headLength;
            includeDevicePoint(headBase + normal * halfWidth);
            includeDevicePoint(headBase - normal * halfWidth);
        }

        const qreal halfStroke = strokeWidth / 2.0;
        frameLeft = qMax<qreal>(
            0.0,
            frameLeft - halfStroke / pageSize.width());
        frameTop = qMax<qreal>(
            0.0,
            frameTop - halfStroke / pageSize.height());
        frameRight = qMin<qreal>(
            1.0,
            frameRight + halfStroke / pageSize.width());
        frameBottom = qMin<qreal>(
            1.0,
            frameBottom + halfStroke / pageSize.height());
    }
    const auto horizontal = expandedInterval(
        frameLeft,
        frameRight,
        minimumWidth);
    const auto vertical = expandedInterval(
        frameTop,
        frameBottom,
        minimumHeight);
    const QRectF frameRect(
        horizontal.first,
        vertical.first,
        horizontal.second - horizontal.first,
        vertical.second - vertical.first);
    const auto pointRatio = [&frameRect](const QPointF &point) {
        return QPointF(
            qBound<qreal>(
                0.0,
                (point.x() - frameRect.x()) / frameRect.width(),
                1.0),
            qBound<qreal>(
                0.0,
                (point.y() - frameRect.y()) / frameRect.height(),
                1.0));
    };

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("added-shape:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedShape");
    candidate.label = shapeLabel(m_shapeType);
    candidate.editable = true;
    candidate.added = true;
    candidate.shapeType = m_shapeType;
    candidate.shapeStartRatio = pointRatio(boundedStart);
    candidate.shapeEndRatio = pointRatio(shapeEnd);
    candidate.shapeStrokeWidthRatio = kAddedShapeStrokeWidthRatio;
    candidate.strokeColor = QColor(Qt::black);
    candidate.fillColor = QColor(Qt::transparent);
    setInitialGeometry(candidate, frameRect);

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

    setTool(SelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QPointF PDFEditController::constrainedShapeEnd(
    int page,
    const QPointF &startPoint,
    const QPointF &endPoint) const
{
    const QPointF boundedStart(
        qBound<qreal>(0.0, startPoint.x(), 1.0),
        qBound<qreal>(0.0, startPoint.y(), 1.0));
    QPointF boundedEnd(
        qBound<qreal>(0.0, endPoint.x(), 1.0),
        qBound<qreal>(0.0, endPoint.y(), 1.0));
    if ((m_shapeType != LineShape && m_shapeType != ArrowShape)
        || !m_document) {
        return boundedEnd;
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return boundedEnd;
    }

    const qreal deltaX = (boundedEnd.x() - boundedStart.x())
        * pageSize.width();
    const qreal deltaY = (boundedEnd.y() - boundedStart.y())
        * pageSize.height();
    const qreal absoluteX = qAbs(deltaX);
    const qreal absoluteY = qAbs(deltaY);
    if (absoluteY <= absoluteX * kShapeAxisSnapRatio) {
        boundedEnd.setY(boundedStart.y());
    } else if (absoluteX <= absoluteY * kShapeAxisSnapRatio) {
        boundedEnd.setX(boundedStart.x());
    }
    return boundedEnd;
}

bool PDFEditController::shapeDragIsValid(
    int page,
    const QPointF &startPoint,
    const QPointF &endPoint) const
{
    if (!m_document
        || !std::isfinite(startPoint.x())
        || !std::isfinite(startPoint.y())
        || !std::isfinite(endPoint.x())
        || !std::isfinite(endPoint.y())) {
        return false;
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return false;
    }

    const QPointF boundedStart(
        qBound<qreal>(0.0, startPoint.x(), 1.0),
        qBound<qreal>(0.0, startPoint.y(), 1.0));
    const QPointF boundedEnd(
        qBound<qreal>(0.0, endPoint.x(), 1.0),
        qBound<qreal>(0.0, endPoint.y(), 1.0));
    return shapeDragIsLongEnough(
        pageSize,
        boundedStart,
        constrainedShapeEnd(page, boundedStart, boundedEnd));
}

qreal PDFEditController::shapeHitToleranceRatio(
    int page,
    qreal strokeWidthRatio) const
{
    if (!m_document || !std::isfinite(strokeWidthRatio)) {
        return 0.0;
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return 0.0;
    }
    return shapeHitTolerancePoints(
               qMax<qreal>(0.0, strokeWidthRatio),
               pageSize.height())
        / pageSize.height();
}

QVector<QRectF> PDFEditController::resolvedTextMarkupRectsForSelection(
    int page,
    const QRectF &selectionRect,
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

    // This resolver currently feeds only Squiggly preview and creation. Keep
    // the top and width aligned to the glyphs while extending the bottom by
    // enough padding for the wave band plus a visible gap. Other markup tools
    // keep their freehand rectangle.
    const qreal gapRatio = kSquigglyBottomPaddingPoints / pageSize.height();
    for (QRectF &markupRect : markupRects) {
        markupRect.setBottom(qMin<qreal>(
            1.0,
            markupRect.bottom() + gapRatio));
    }
    return markupRects;
}

QVariantList PDFEditController::textMarkupRectsForSelection(
    int page,
    const QRectF &selectionRect)
{
    const QVector<QRectF> rects = resolvedTextMarkupRectsForSelection(
        page,
        selectionRect,
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
    const Tool markupTool = m_tool;
    if (normalizedRect.width() < kMinimumHighlightDimensionRatio
        || (markupTool != SquigglyTool
            && normalizedRect.height() < kMinimumHighlightDimensionRatio)
        || normalizedRect.height() <= 0.0) {
        return {};
    }

    QVector<QRectF> annotationRects{normalizedRect};
    if (markupTool == SquigglyTool) {
        bool geometryReady = false;
        annotationRects = resolvedTextMarkupRectsForSelection(
            page,
            normalizedRect,
            &geometryReady);
        if (!geometryReady) {
            if (m_pendingSquigglyCreations.size()
                >= kMaximumPendingSquigglyCreations) {
                //% "Text information is still loading. Please try again."
                emit statusMessage(qtTrId("TextGeometryStillLoading"));
                return {};
            }

            PendingSquigglyCreation pendingCreation;
            pendingCreation.filePath = m_document->renderFilePath();
            pendingCreation.selectionRect = normalizedRect;
            pendingCreation.pageSize = m_document->pageSize(page);
            pendingCreation.color = m_squigglyColor;
            pendingCreation.revision = m_document->renderRevision();
            pendingCreation.page = page;
            pendingCreation.sourcePage = m_document->sourcePageIndex(page);
            m_pendingSquigglyCreations.append(std::move(pendingCreation));
            requestPageCandidateScan(page);
            emit historyStateChanged();

            QVariantMap pendingResult;
            pendingResult.insert(QStringLiteral("kind"), QStringLiteral("squiggly"));
            pendingResult.insert(QStringLiteral("page"), page);
            pendingResult.insert(QStringLiteral("pending"), true);
            return pendingResult;
        }
        if (annotationRects.isEmpty()) {
            //% "No text was found in the selected area."
            emit statusMessage(qtTrId("NoTextInSelectedArea"));
            return {};
        }
    }

    QColor markupColor = m_highlightColor;
    if (markupTool == UnderlineTool) {
        markupColor = m_underlineColor;
    } else if (markupTool == SquigglyTool) {
        markupColor = m_squigglyColor;
    } else if (markupTool == StrikeoutTool) {
        markupColor = m_strikeoutColor;
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

bool PDFEditController::pendingSquigglyCreationIsCurrent(
    const PendingSquigglyCreation &creation) const
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

int PDFEditController::pendingSquigglyScanPage() const
{
    for (const PendingSquigglyCreation &creation
         : m_pendingSquigglyCreations) {
        if (pendingSquigglyCreationIsCurrent(creation)
            && !m_pageCandidates.contains(creation.page)) {
            return creation.page;
        }
    }
    return -1;
}

void PDFEditController::completePendingSquigglyCreations(int page)
{
    if (m_pendingSquigglyCreations.isEmpty()) {
        return;
    }

    QVector<PendingSquigglyCreation> readyCreations;
    QVector<PendingSquigglyCreation> remainingCreations;
    readyCreations.reserve(m_pendingSquigglyCreations.size());
    remainingCreations.reserve(m_pendingSquigglyCreations.size());
    for (PendingSquigglyCreation &creation : m_pendingSquigglyCreations) {
        if (creation.page == page) {
            readyCreations.append(std::move(creation));
        } else {
            remainingCreations.append(std::move(creation));
        }
    }
    m_pendingSquigglyCreations = std::move(remainingCreations);
    if (readyCreations.isEmpty()) {
        return;
    }

    bool createdDraft = false;
    bool pendingStateChanged = false;
    for (const PendingSquigglyCreation &creation : readyCreations) {
        if (!pendingSquigglyCreationIsCurrent(creation)) {
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
                &geometryReady);
        if (!geometryReady) {
            m_pendingSquigglyCreations.append(creation);
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
            SquigglyTool,
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

int PDFEditController::addBlankPage()
{
    if (!m_document || !m_document->isLoaded() || m_saving) {
        return -1;
    }
    if (!documentAllowsPageAssembly()) {
        //% "This PDF does not permit adding or deleting pages."
        emit statusMessage(qtTrId("PdfPageAssemblyNotPermitted"));
        return -1;
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    QSizeF pageSize = m_document->pageSize(m_document->currentPage());
    if (pageSize.isEmpty() && m_document->sourcePageCount() > 0) {
        pageSize = m_document->pageSize(0);
    }
    if (pageSize.isEmpty()) {
        pageSize = QSizeF(595.0, 842.0);
    }

    Command command;
    command.kind = Command::Kind::PageStructure;
    command.page = m_document->pageCount() > 0
        ? m_document->currentPage() + 1
        : 0;
    command.pageStateBefore = capturePageState();
    command.pageStateAfter = command.pageStateBefore;
    for (PDFPageInsertion &insertion
         : command.pageStateAfter.pageInsertions) {
        if (insertion.page >= command.page) {
            ++insertion.page;
        }
    }
    command.pageStateAfter.pageInsertions.append(
        PDFPageInsertion{command.page, pageSize});
    std::sort(
        command.pageStateAfter.pageInsertions.begin(),
        command.pageStateAfter.pageInsertions.end(),
        [](const PDFPageInsertion &left, const PDFPageInsertion &right) {
            return left.page < right.page;
        });

    for (Candidate &candidate : command.pageStateAfter.addedCandidates) {
        if (candidate.page >= command.page) {
            ++candidate.page;
        }
    }

    QHash<QString, Draft> shiftedDrafts;
    shiftedDrafts.reserve(command.pageStateAfter.drafts.size());
    for (Draft draft : std::as_const(command.pageStateAfter.drafts)) {
        if (draft.candidate.page >= command.page) {
            ++draft.candidate.page;
        }
        shiftedDrafts.insert(
            draftKey(draft.candidate.page, draft.candidate.objectId),
            std::move(draft));
    }
    command.pageStateAfter.drafts = std::move(shiftedDrafts);

    if (command.pageStateAfter.hasSelection
        && command.pageStateAfter.selectedCandidate.page >= command.page) {
        ++command.pageStateAfter.selectedCandidate.page;
    }
    command.pageStateAfter.currentPage = command.page;
    command.pageStateAfter.hasSelection = false;
    command.pageStateAfter.selectedCandidate = Candidate();
    applyDraftState(command, true, true);
    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
    return command.page;
}

int PDFEditController::deleteCurrentPage()
{
    if (!m_document || !m_document->isLoaded() || m_saving) {
        return -1;
    }
    if (!documentAllowsPageAssembly()) {
        //% "This PDF does not permit adding or deleting pages."
        emit statusMessage(qtTrId("PdfPageAssemblyNotPermitted"));
        return -1;
    }
    if (m_document->pageCount() <= 1) {
        //% "A PDF must contain at least one page."
        emit statusMessage(qtTrId("CannotDeleteOnlyPage"));
        return -1;
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();

    const int deletedPage = m_document->currentPage();
    const int deletedSourcePage = m_document->sourcePageIndex(deletedPage);
    Command command;
    command.kind = Command::Kind::PageStructure;
    command.page = deletedPage;
    command.pageStateBefore = capturePageState();
    command.pageStateAfter = command.pageStateBefore;

    if (deletedSourcePage >= 0) {
        command.pageStateAfter.deletedSourcePages.insert(deletedSourcePage);
    } else {
        const auto insertion = std::find_if(
            command.pageStateAfter.pageInsertions.cbegin(),
            command.pageStateAfter.pageInsertions.cend(),
            [deletedPage](const PDFPageInsertion &candidate) {
                return candidate.page == deletedPage;
            });
        if (insertion == command.pageStateAfter.pageInsertions.cend()) {
            return -1;
        }
        command.pageStateAfter.pageInsertions.erase(insertion);
    }
    for (PDFPageInsertion &insertion
         : command.pageStateAfter.pageInsertions) {
        if (insertion.page > deletedPage) {
            --insertion.page;
        }
    }

    QVector<Candidate> shiftedCandidates;
    shiftedCandidates.reserve(command.pageStateAfter.addedCandidates.size());
    for (Candidate candidate
         : std::as_const(command.pageStateAfter.addedCandidates)) {
        if (candidate.page == deletedPage) {
            continue;
        }
        if (candidate.page > deletedPage) {
            --candidate.page;
        }
        shiftedCandidates.append(std::move(candidate));
    }
    command.pageStateAfter.addedCandidates = std::move(shiftedCandidates);

    QHash<QString, Draft> shiftedDrafts;
    shiftedDrafts.reserve(command.pageStateAfter.drafts.size());
    for (Draft draft : std::as_const(command.pageStateAfter.drafts)) {
        if (draft.candidate.page == deletedPage) {
            continue;
        }
        if (draft.candidate.page > deletedPage) {
            --draft.candidate.page;
        }
        shiftedDrafts.insert(
            draftKey(draft.candidate.page, draft.candidate.objectId),
            std::move(draft));
    }
    command.pageStateAfter.drafts = std::move(shiftedDrafts);

    if (command.pageStateAfter.hasSelection) {
        if (command.pageStateAfter.selectedCandidate.page == deletedPage) {
            command.pageStateAfter.hasSelection = false;
            command.pageStateAfter.selectedCandidate = Candidate();
        } else if (command.pageStateAfter.selectedCandidate.page > deletedPage) {
            --command.pageStateAfter.selectedCandidate.page;
        }
    }
    const int remainingPageCount = m_document->sourcePageCount()
        - command.pageStateAfter.deletedSourcePages.size()
        + command.pageStateAfter.pageInsertions.size();
    command.pageStateAfter.currentPage = qBound(
        0,
        deletedPage,
        remainingPageCount - 1);

    applyDraftState(command, true, true);
    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
    return m_document->currentPage();
}

QFont PDFEditController::fontForDisplay(const QString &fontFamily,
                                        int fontStretch,
                                        bool fontBold,
                                        bool fontItalic,
                                        bool fontUnderline,
                                        qreal pixelSize) const
{
    const int boundedPixelSize = qMax(1, qRound(pixelSize));
    QFont font;
    if (!fontFamily.isEmpty()) {
        font.setFamily(fontFamily);
    }
    font.setBold(fontBold);
    font.setItalic(fontItalic);
    font.setUnderline(fontUnderline);
    font.setPixelSize(boundedPixelSize);
    font.setStretch(displayFontStretch(
        fontFamily,
        fontStretch,
        fontBold,
        fontItalic,
        boundedPixelSize));
    return font;
}

int PDFEditController::displayFontStretch(const QString &fontFamily,
                                          int fontStretch,
                                          bool fontBold,
                                          bool fontItalic,
                                          qreal pixelSize) const
{
    return ApplicationFonts::calibratedFontStretch(
        fontFamily,
        fontStretch,
        fontBold,
        fontItalic,
        qMax(1, qRound(pixelSize)));
}

void PDFEditController::applyTypingFormatToEditorDocument(
    QObject *textDocument,
    int start,
    int length) const
{
    auto *quickTextDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickTextDocument
        ? quickTextDocument->textDocument()
        : nullptr;
    if (!document || !m_typingCharacterFormatActive || length <= 0) {
        return;
    }

    const int maximumPosition = qMax(0, document->characterCount() - 1);
    const int rangeStart = qBound(0, start, maximumPosition);
    const int rangeEnd = qBound(
        rangeStart,
        rangeStart + length,
        maximumPosition);
    if (rangeEnd <= rangeStart) {
        return;
    }

    QTextCharFormat format;
    format.setFontWeight(
        m_typingCharacterFormat.fontBold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(m_typingCharacterFormat.fontItalic);
    format.setFontUnderline(m_typingCharacterFormat.fontUnderline);

    QTextCursor cursor(document);
    cursor.setPosition(rangeStart);
    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
    cursor.joinPreviousEditBlock();
    cursor.mergeCharFormat(format);
    cursor.endEditBlock();
}

bool PDFEditController::applyFontStyleToEditorDocument(
    QObject *textDocument,
    int start,
    int length,
    int style,
    bool enabled) const
{
    auto *quickTextDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickTextDocument
        ? quickTextDocument->textDocument()
        : nullptr;
    if (!document || length <= 0
        || style < static_cast<int>(FontStyle::Bold)
        || style > static_cast<int>(FontStyle::Underline)) {
        return false;
    }

    const int maximumPosition = qMax(0, document->characterCount() - 1);
    const int rangeStart = qBound(0, start, maximumPosition);
    const int rangeEnd = qBound(
        rangeStart,
        rangeStart + length,
        maximumPosition);
    if (rangeEnd <= rangeStart) {
        return false;
    }

    QTextCharFormat format;
    switch (static_cast<FontStyle>(style)) {
    case FontStyle::Bold:
        format.setFontWeight(enabled ? QFont::Bold : QFont::Normal);
        break;
    case FontStyle::Italic:
        format.setFontItalic(enabled);
        break;
    case FontStyle::Underline:
        format.setFontUnderline(enabled);
        break;
    }

    QTextCursor cursor(document);
    cursor.setPosition(rangeStart);
    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
    cursor.beginEditBlock();
    cursor.mergeCharFormat(format);
    cursor.endEditBlock();
    return true;
}

bool PDFEditController::syncEditorFontStyles(QObject *textDocument)
{
    auto *quickTextDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickTextDocument
        ? quickTextDocument->textDocument()
        : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection) {
        return false;
    }

    Draft draft = selectedDraftForDisplay();
    if (document->toPlainText() != draft.text || draft.text.isEmpty()) {
        return false;
    }

    const qsizetype textLength = draft.text.size();
    const CharacterFormat fallbackFormat = primaryCharacterFormat(draft);
    QVector<CharacterFormat> formats(textLength, fallbackFormat);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, textLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            textLength);
        for (qsizetype position = spanStart; position < spanEnd; ++position) {
            formats[position] = span.format;
        }
    }

    QTextCursor cursor(document);
    for (qsizetype position = 0; position < textLength; ++position) {
        cursor.setPosition(static_cast<int>(position));
        cursor.setPosition(static_cast<int>(position + 1), QTextCursor::KeepAnchor);
        const QTextCharFormat editorFormat = cursor.charFormat();
        CharacterFormat &format = formats[position];
        format.fontBold = editorFormat.fontWeight() >= QFont::Bold;
        format.fontItalic = editorFormat.fontItalic();
        format.fontUnderline = editorFormat.fontUnderline();
    }

    QVector<CharacterSpan> spans;
    qsizetype spanStart = 0;
    CharacterFormat spanFormat = formats.constFirst();
    for (qsizetype position = 1; position <= formats.size(); ++position) {
        if (position < formats.size()
            && sameCharacterFormat(spanFormat, formats.at(position))) {
            continue;
        }
        spans.append(CharacterSpan{
            spanStart,
            position - spanStart,
            spanFormat});
        if (position < formats.size()) {
            spanStart = position;
            spanFormat = formats.at(position);
        }
    }
    if (sameCharacterSpans(draft.characterSpans, spans)) {
        return false;
    }

    draft.characterSpans = std::move(spans);
    updateSelectedDraft(draft);
    return true;
}

void PDFEditController::snapshotFontResolutionLocked(
    FPDF_PAGEOBJECT textObject,
    Candidate &candidate,
    const CandidateScanRequest &request,
    QHash<FPDF_FONT, FontResolutionSnapshot> &fontResolutionCache,
    qsizetype &embeddedFontDataBytes)
{
    assertPDFiumProcessLockHeld();
    FPDF_FONT font = FPDFTextObj_GetFont(textObject);
    if (!font) {
        return;
    }

    auto cachedSnapshot = fontResolutionCache.constFind(font);
    if (cachedSnapshot == fontResolutionCache.cend()) {
        FontResolutionSnapshot snapshot;
        snapshot.scanIndex = static_cast<int>(fontResolutionCache.size());
        snapshot.expectedFamily = pdfFontName(font, false);
        snapshot.bundledFamily = matchingFontFamily(
            font,
            request.bundledFontFamilies);
        snapshot.systemFamily = matchingFontFamily(
            font,
            request.systemFontFamilies);
        snapshot.identity = normalizedPdfFontIdentity(font);
        snapshot.bold = pdfFontIsBold(font);
        snapshot.italic = pdfFontIsItalic(font);

        const qsizetype remainingSnapshotBytes = qMax<qsizetype>(
            0,
            request.maximumEmbeddedFontSnapshotBytes - embeddedFontDataBytes);
        if (remainingSnapshotBytes > 0 && FPDFFont_GetIsEmbedded(font) == 1) {
            size_t fontByteCount = 0;
            if (FPDFFont_GetFontData(font, nullptr, 0, &fontByteCount)
                && fontByteCount > 0
                && fontByteCount <= static_cast<size_t>(kMaximumEmbeddedFontBytes)
                && fontByteCount <= static_cast<size_t>(remainingSnapshotBytes)) {
                snapshot.embeddedFontData.resize(
                    static_cast<qsizetype>(fontByteCount));
                size_t actualByteCount = fontByteCount;
                if (!FPDFFont_GetFontData(
                        font,
                        reinterpret_cast<uint8_t *>(
                            snapshot.embeddedFontData.data()),
                        fontByteCount,
                        &actualByteCount)
                    || actualByteCount == 0
                    || actualByteCount > fontByteCount) {
                    snapshot.embeddedFontData.clear();
                } else {
                    snapshot.embeddedFontData.resize(
                        static_cast<qsizetype>(actualByteCount));
                    embeddedFontDataBytes += snapshot.embeddedFontData.size();
                }
            }
        }

        fontResolutionCache.insert(font, std::move(snapshot));
        cachedSnapshot = fontResolutionCache.constFind(font);
    }

    candidate.fontIdentity = cachedSnapshot->identity;
    candidate.fontBold = cachedSnapshot->bold;
    candidate.fontItalic = cachedSnapshot->italic;
    candidate.pendingFontSnapshotIndex = cachedSnapshot->scanIndex;
    candidate.pendingFontExpectedFamily = cachedSnapshot->expectedFamily;
    candidate.pendingBundledFontFamily = cachedSnapshot->bundledFamily;
    candidate.pendingSystemFontFamily = cachedSnapshot->systemFamily;
    candidate.pendingEmbeddedFontData = cachedSnapshot->embeddedFontData;
}

void PDFEditController::resolveCandidateFontFamily(
    Candidate &candidate,
    QHash<int, QString> &resolvedEmbeddedFamilies)
{
    assertPDFiumProcessLockNotHeld();
    // Resolve known exact faces before registering a PDF-embedded face.
    // Embedded fonts are commonly subsetted but retain their original family
    // name. Registering such a subset first can make QFontDatabase select it
    // instead of the complete bundled/system face for every later object with
    // that family.
    if (ApplicationFonts::fontFaceSupportsText(
            candidate.pendingBundledFontFamily,
            candidate.fontBold,
            candidate.fontItalic,
            candidate.text)) {
        candidate.fontFamily = candidate.pendingBundledFontFamily;
    } else if (ApplicationFonts::fontFaceSupportsText(
                   candidate.pendingSystemFontFamily,
                   candidate.fontBold,
                   candidate.fontItalic,
                   candidate.text)) {
        candidate.fontFamily = candidate.pendingSystemFontFamily;
    } else {
        QString embeddedFamily;
        const auto resolvedFamily = resolvedEmbeddedFamilies.constFind(
            candidate.pendingFontSnapshotIndex);
        if (resolvedFamily != resolvedEmbeddedFamilies.cend()) {
            embeddedFamily = resolvedFamily.value();
        } else {
            embeddedFamily = registerEmbeddedFont(
                candidate.pendingEmbeddedFontData,
                candidate.pendingFontExpectedFamily);
            resolvedEmbeddedFamilies.insert(
                candidate.pendingFontSnapshotIndex,
                embeddedFamily);
        }

        candidate.fontFamily = fontSupportsText(embeddedFamily, candidate.text)
            ? embeddedFamily
            : QString();
    }

    candidate.pendingFontSnapshotIndex = -1;
    candidate.pendingFontExpectedFamily.clear();
    candidate.pendingBundledFontFamily.clear();
    candidate.pendingSystemFontFamily.clear();
    candidate.pendingEmbeddedFontData.clear();
}

QString PDFEditController::registerEmbeddedFont(
    const QByteArray &fontData,
    const QString &expectedFamily)
{
    assertPDFiumProcessLockNotHeld();
    if (fontData.isEmpty()) {
        return {};
    }

    const QByteArray fingerprint = QCryptographicHash::hash(
        fontData,
        QCryptographicHash::Sha256);
    const auto registeredFamily = m_registeredFontFamilies.constFind(fingerprint);
    if (registeredFamily != m_registeredFontFamilies.cend()) {
        return registeredFamily.value();
    }
    if (m_rejectedFontFingerprints.contains(fingerprint)) {
        return {};
    }
    if (m_registeredFontIds.size() >= kMaximumRegisteredFontCount
        || fontData.size() > kMaximumRegisteredFontBytes - m_registeredFontBytes) {
        rememberRejectedFontFingerprint(fingerprint);
        return {};
    }

    // Mark the fingerprint before calling Qt. Adding an application font emits
    // fontDatabaseChanged synchronously; a re-entrant candidate finalization
    // must not try to register the same bytes recursively.
    rememberRejectedFontFingerprint(fingerprint);

    const int fontId = QFontDatabase::addApplicationFontFromData(fontData);
    if (fontId < 0) {
        return {};
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (families.isEmpty()) {
        QFontDatabase::removeApplicationFont(fontId);
        return {};
    }

    QString family = families.constFirst();
    for (const QString &candidateFamily : families) {
        if (candidateFamily.compare(expectedFamily, Qt::CaseInsensitive) == 0) {
            family = candidateFamily;
            break;
        }
    }

    m_registeredFontIds.append(fontId);
    m_registeredFontBytes += fontData.size();
    m_rejectedFontFingerprints.remove(fingerprint);
    m_rejectedFontFingerprintLru.removeAll(fingerprint);
    m_registeredFontFamilies.insert(fingerprint, family);
    return family;
}

void PDFEditController::rememberRejectedFontFingerprint(
    const QByteArray &fingerprint)
{
    m_rejectedFontFingerprints.insert(fingerprint);
    m_rejectedFontFingerprintLru.removeAll(fingerprint);
    m_rejectedFontFingerprintLru.append(fingerprint);
    while (m_rejectedFontFingerprintLru.size()
           > kMaximumRejectedFontFingerprints) {
        const QByteArray oldest = m_rejectedFontFingerprintLru.takeFirst();
        m_rejectedFontFingerprints.remove(oldest);
    }
}

void PDFEditController::clearRegisteredFonts()
{
    assertPDFiumProcessLockNotHeld();
    for (auto fontId = m_registeredFontIds.crbegin();
         fontId != m_registeredFontIds.crend();
         ++fontId) {
        QFontDatabase::removeApplicationFont(*fontId);
    }
    m_registeredFontIds.clear();
    m_registeredFontFamilies.clear();
    m_rejectedFontFingerprints.clear();
    m_rejectedFontFingerprintLru.clear();
    m_registeredFontBytes = 0;
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

void PDFEditController::requestCommentIndex()
{
    ensureCacheRevision();
    if (!m_document
        || !m_document->isLoaded()
        || m_commentIndexState == CommentIndexLoading
        || m_commentIndexState == CommentIndexReady
        || m_commentIndexState == CommentIndexLimited) {
        return;
    }

    const QString filePath = m_document->renderFilePath();
    const int revision = m_document->renderRevision();
    if (filePath.isEmpty()) {
        setCommentIndexState(CommentIndexFailed);
        return;
    }

    QVector<CandidateScanRequest> requests;
    requests.reserve(m_document->pageCount());
    bool scanSetupFailed = false;
    for (int page = 0; page < m_document->pageCount(); ++page) {
        CandidateScanRequest request;
        request.filePath = filePath;
        request.revision = revision;
        request.page = page;
        request.sourcePage = m_document->sourcePageIndex(page);
        request.pageSize = m_document->pageSize(page);
        request.editingAllowed = documentAllowsEditing();
        request.annotationsAllowed = documentAllowsAnnotations();
        request.commentAuthor = currentCommentAuthor();
        if (request.sourcePage < 0) {
            continue;
        }
        if (request.pageSize.isEmpty()) {
            scanSetupFailed = true;
            continue;
        }
        requests.append(std::move(request));
    }

    QSet<int> completeIndexedPages;
    completeIndexedPages.reserve(
        m_indexedCommentCandidates.size() + m_pageCandidates.size());
    for (auto page = m_indexedCommentCandidates.cbegin();
         page != m_indexedCommentCandidates.cend();
         ++page) {
        completeIndexedPages.insert(page.key());
    }

    bool commentMemoryBudgetExceeded
        = m_indexedCommentCandidateBytes > kMaximumCommentIndexBytes;
    for (auto page = m_pageCandidates.cbegin();
         page != m_pageCandidates.cend();
         ++page) {
        if (completeIndexedPages.contains(page.key())) {
            continue;
        }
        if (!cacheCommentCandidates(page.key(), page.value())) {
            commentMemoryBudgetExceeded = true;
            break;
        }
        completeIndexedPages.insert(page.key());
    }
    if (!m_indexedCommentCandidates.isEmpty()) {
        emit commentsChanged();
    }
    if (commentMemoryBudgetExceeded) {
        setCommentIndexState(CommentIndexLimited);
        return;
    }

    requests.erase(
        std::remove_if(
            requests.begin(),
            requests.end(),
            [&completeIndexedPages](const CandidateScanRequest &request) {
                return completeIndexedPages.contains(request.page);
            }),
        requests.end());
    const qsizetype initialIndexedBytes
        = m_indexedCommentCandidateBytes;

    if (requests.isEmpty()) {
        setCommentIndexState(
            scanSetupFailed ? CommentIndexFailed : CommentIndexReady);
        return;
    }

    m_commentIndexCancellation
        = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancellation
        = m_commentIndexCancellation;
    setCommentIndexState(CommentIndexLoading);

    const QPointer<PDFEditController> controller(this);
    commentIndexThreadPool().start(QRunnable::create(
        [controller,
         requests = std::move(requests),
         filePath,
         revision,
         indexedBytes = initialIndexedBytes,
         initialScanFailed = scanSetupFailed,
         cancellation]() mutable {
            CandidateScanDocument commentDocument;
            bool scanFailed = initialScanFailed;
            bool indexLimited = false;
            for (const CandidateScanRequest &request : requests) {
                if (candidateScanCancelled(cancellation)) {
                    break;
                }
                CandidateScanRequest budgetedRequest = request;
                budgetedRequest.maximumCommentIndexBytes = qMax<qsizetype>(
                    0,
                    kMaximumCommentIndexBytes - indexedBytes);
                CommentPageScanResult scanResult;
                {
                    PDFiumProcessLock pdfiumLock;
                    FPDF_DOCUMENT document = commentDocument.documentLocked(
                        filePath,
                        revision);
                    scanResult = PDFEditController::scanCommentPageLocked(
                        document,
                        budgetedRequest,
                        cancellation);
                }
                if (candidateScanCancelled(cancellation)) {
                    break;
                }
                if (scanResult.memoryBudgetExceeded) {
                    indexLimited = true;
                    break;
                }
                if (!scanResult.succeeded) {
                    scanFailed = true;
                    continue;
                }
                indexedBytes = saturatedByteCountAdd(
                    indexedBytes,
                    scanResult.estimatedRetainedBytes);
                QVector<Candidate> comments
                    = std::move(scanResult.candidates);
                if (!comments.isEmpty()) {
                    QObject *app = QCoreApplication::instance();
                    if (!app) {
                        cancellation->store(true, std::memory_order_relaxed);
                        break;
                    }
                    QMetaObject::invokeMethod(
                        app,
                        [controller,
                         request,
                         comments = std::move(comments),
                         cancellation]() mutable {
                            if (!controller
                                || candidateScanCancelled(cancellation)
                                || controller->m_commentIndexCancellation
                                    != cancellation
                                || !controller->m_document
                                || !controller->m_document->isLoaded()
                                || controller->m_document->renderRevision()
                                    != request.revision
                                || controller->m_document->renderFilePath()
                                    != request.filePath
                                || request.page < 0
                                || request.page
                                    >= controller->m_document->pageCount()
                                || controller->m_document->sourcePageIndex(
                                       request.page)
                                    != request.sourcePage
                                || controller->m_document->pageSize(request.page)
                                    != request.pageSize) {
                                return;
                            }
                            if (!controller->cacheCommentCandidates(
                                    request.page,
                                    comments)) {
                                cancellation->store(
                                    true,
                                    std::memory_order_relaxed);
                                controller->setCommentIndexState(
                                    CommentIndexLimited);
                                return;
                            }
                            emit controller->commentsChanged();
                        },
                        Qt::QueuedConnection);
                }
            }

            QObject *app = QCoreApplication::instance();
            if (!app) {
                return;
            }
            QMetaObject::invokeMethod(
                app,
                [controller,
                 filePath,
                 revision,
                 cancellation,
                 scanFailed,
                 indexLimited]() {
                    if (!controller
                        || controller->m_commentIndexCancellation
                            != cancellation) {
                        return;
                    }
                    const CommentIndexState terminalState
                        = controller->m_commentIndexState;
                    controller->m_commentIndexCancellation.reset();
                    if (terminalState == CommentIndexFailed
                        || terminalState == CommentIndexLimited) {
                        return;
                    }
                    const bool completedCurrentDocument
                        = !candidateScanCancelled(cancellation)
                        && controller->m_document
                        && controller->m_document->isLoaded()
                        && controller->m_document->renderRevision() == revision
                        && controller->m_document->renderFilePath()
                            == filePath;
                    controller->setCommentIndexState(
                        !completedCurrentDocument
                            ? CommentIndexIdle
                            : indexLimited
                                ? CommentIndexLimited
                                : scanFailed
                                    ? CommentIndexFailed
                                    : CommentIndexReady);
                },
                Qt::QueuedConnection);
        }));
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

void PDFEditController::clearSelection()
{
    if (!m_hasSelection) {
        return;
    }
    finishSelectedTextEdit();
    setSelection(nullptr);
}

void PDFEditController::beginSelectedTextEdit()
{
    if (m_hasSelection
        && !isTextCandidateKind(m_selectedCandidate.kind)) {
        return;
    }
    beginSelectedTextEditSession(true);
}

void PDFEditController::beginSelectedTextEditSession(bool expandFrame)
{
    if (m_textEditActive || !m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    m_textEditCommand = Command();
    m_textEditCommand.key = key;
    m_textEditCommand.page = m_selectedCandidate.page;
    m_textEditCommand.objectId = m_selectedCandidate.objectId;
    m_textEditCommand.hadBefore = existingDraft != m_drafts.cend();
    m_textEditCommand.before = m_textEditCommand.hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    m_textEditDisplayDraft = m_textEditCommand.before;
    m_textEditDisplayDraftActive = true;
    m_textEditFrameAutomaticallyExpanded = false;
    m_textEditGeometryExplicitlyChanged = false;
    if (expandFrame) {
        const QRectF expandedFrame = expandedTextEditFrame(m_textEditDisplayDraft);
        if (expandedFrame != m_textEditDisplayDraft.frameRect) {
            const qreal contentBottom = qBound<qreal>(
                expandedFrame.bottom(),
                qMax(
                    m_textEditDisplayDraft.frameRect.bottom(),
                    m_textEditDisplayDraft.contentBounds.bottom()),
                1.0);
            m_textEditDisplayDraft.frameRect = expandedFrame;
            m_textEditDisplayDraft.contentBounds = QRectF(
                expandedFrame.x(),
                expandedFrame.y(),
                expandedFrame.width(),
                contentBottom - expandedFrame.y());
            m_textEditFrameAutomaticallyExpanded = true;
        }
    }
    m_textEditInitialDisplayFrameRect = m_textEditDisplayDraft.frameRect;
    resetTypingFormat();
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditActive = true;
    emit textEditActiveChanged();
    emit selectionChanged();
}

void PDFEditController::updateSelectedTextPreview(const QString &text,
                                                  int cursorPosition)
{
    if (!m_textEditActive) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }
    Draft draft = selectedDraftForDisplay();
    if (draft.text == text) {
        return;
    }
    const TextChange change = setDraftText(
        draft,
        text,
        m_typingCharacterFormatActive ? &m_typingCharacterFormat : nullptr,
        m_typingCharacterFormatPosition,
        static_cast<qsizetype>(cursorPosition));
    bool shouldSyncEditorFormat = false;
    if (m_typingCharacterFormatActive) {
        const bool typingChange = change.start == m_typingCharacterFormatPosition
            || change.start + change.removedLength
                == m_typingCharacterFormatPosition;
        if (typingChange) {
            shouldSyncEditorFormat = change.insertedLength > 0;
            m_typingCharacterFormatPosition = change.start + change.insertedLength;
            m_selectedTextRangeStart = m_typingCharacterFormatPosition;
            m_selectedTextRangeLength = 0;
        } else {
            resetTypingFormat();
        }
    }
    updateSelectedDraft(draft);
    if (shouldSyncEditorFormat) {
        emit typingFormatApplied(
            static_cast<int>(change.start),
            static_cast<int>(change.insertedLength));
    }
}

void PDFEditController::beginSelectedFrameResize()
{
    if (m_frameResizeActive
        || !m_hasSelection
        || !m_selectedCandidate.editable) {
        return;
    }

    const bool ownsTextEdit = !m_textEditActive;
    if (ownsTextEdit) {
        beginSelectedTextEditSession(false);
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const Draft draft = selectedDraftForDisplay();
    m_frameResizeActive = true;
    m_frameResizeOwnsTextEdit = ownsTextEdit;
    m_frameResizeStartGeometryExplicitlyChanged
        = m_textEditGeometryExplicitlyChanged;
    m_frameResizeStartFrameRect = draft.frameRect;
    m_frameResizeStartContentBounds = draft.contentBounds;
    if (isGeometryCommentCandidateKind(draft.candidate.kind)
        && !draft.candidate.added) {
        markDraftStateChanged(draft.candidate.page, true, false);
    }
}

void PDFEditController::updateSelectedFrameRect(const QRectF &frameRect)
{
    if (!m_frameResizeActive || !m_textEditActive || !m_hasSelection) {
        return;
    }

    const QRectF normalizedRect = frameRect.normalized();
    Draft draft = selectedDraftForDisplay();
    const QRectF currentFrameRect = draft.frameRect.normalized();
    constexpr qreal geometryEpsilon = 1e-9;
    const bool movesFrame
        = qAbs(normalizedRect.width() - currentFrameRect.width())
              <= geometryEpsilon
        && qAbs(normalizedRect.height() - currentFrameRect.height())
               <= geometryEpsilon;

    QRectF boundedRect;
    QRectF resizedContentBounds;
    if (movesFrame) {
        const QRectF effectiveBounds = currentFrameRect.united(
            draft.contentBounds.normalized());
        const qreal requestedX
            = normalizedRect.x() - currentFrameRect.x();
        const qreal requestedY
            = normalizedRect.y() - currentFrameRect.y();
        const qreal translationX = qBound<qreal>(
            -effectiveBounds.left(),
            requestedX,
            1.0 - effectiveBounds.right());
        const qreal translationY = qBound<qreal>(
            -effectiveBounds.top(),
            requestedY,
            1.0 - effectiveBounds.bottom());
        const QPointF translation(translationX, translationY);
        boundedRect = currentFrameRect.translated(translation);
        resizedContentBounds = draft.contentBounds.translated(translation);
    } else {
        const qreal left = qBound<qreal>(0.0, normalizedRect.left(), 1.0);
        const qreal top = qBound<qreal>(0.0, normalizedRect.top(), 1.0);
        const qreal right = qBound<qreal>(0.0, normalizedRect.right(), 1.0);
        const qreal bottom = qBound<qreal>(0.0, normalizedRect.bottom(), 1.0);
        if (right - left <= 0.0 || bottom - top <= 0.0) {
            return;
        }
        boundedRect = QRectF(left, top, right - left, bottom - top);
    }

    const bool sourceImageDraft = draft.candidate.kind == QLatin1String("image");
    const bool fixedBoundsDraft = sourceImageDraft
        || draft.candidate.kind == QLatin1String("addedImage")
        || draft.candidate.kind == QLatin1String("freeText")
        || draft.candidate.kind == QLatin1String("stamp")
        || draft.candidate.kind == QLatin1String("stickyNote")
        || isTextMarkupCandidateKind(draft.candidate.kind)
        || isDrawingCommentCandidateKind(draft.candidate.kind)
        || isShapeCandidateKind(draft.candidate.kind);
    if (!movesFrame && sourceImageDraft) {
        const qreal width = qBound<qreal>(0.0, draft.frameRect.width(), 1.0);
        const qreal height = qBound<qreal>(0.0, draft.frameRect.height(), 1.0);
        boundedRect = QRectF(
            qBound<qreal>(0.0, boundedRect.x(), 1.0 - width),
            qBound<qreal>(0.0, boundedRect.y(), 1.0 - height),
            width,
            height);
    }
    if (!movesFrame) {
        const qreal previousContentHeight = qMax(
            draft.frameRect.height(),
            draft.contentBounds.bottom() - draft.frameRect.y());
        resizedContentBounds = fixedBoundsDraft
            ? boundedRect
            : QRectF(
                  boundedRect.x(),
                  boundedRect.y(),
                  boundedRect.width(),
                  qMin(
                      1.0 - boundedRect.y(),
                      qMax(boundedRect.height(), previousContentHeight)));
    }
    if (draft.frameRect == boundedRect
        && draft.contentBounds == resizedContentBounds) {
        return;
    }

    m_textEditGeometryExplicitlyChanged
        = boundedRect != m_textEditInitialDisplayFrameRect;
    draft.frameRect = boundedRect;
    draft.contentBounds = resizedContentBounds;
    if (isGeometryCommentCandidateKind(draft.candidate.kind)) {
        m_textEditDisplayDraft = draft;
        emit selectedGeometryChanged();
        return;
    }
    updateSelectedDraft(draft);
}

void PDFEditController::updateSelectedContentBounds(const QRectF &contentBounds)
{
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    const QRectF &frameRect = draft.frameRect;
    const qreal contentBottom = qBound<qreal>(
        frameRect.bottom(),
        contentBounds.normalized().bottom(),
        1.0);
    const QRectF boundedContentBounds(
        frameRect.x(),
        frameRect.y(),
        frameRect.width(),
        contentBottom - frameRect.y());
    if (draft.contentBounds == boundedContentBounds) {
        return;
    }

    draft.contentBounds = boundedContentBounds;
    updateSelectedDraft(draft);
}

void PDFEditController::finishSelectedFrameResize()
{
    if (!m_frameResizeActive) {
        return;
    }

    const bool refreshesSourceComment = m_hasSelection
        && isGeometryCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.added;
    const int page = refreshesSourceComment
        ? m_selectedCandidate.page
        : -1;
    const quint64 suppressionRevision = refreshesSourceComment
        ? draftRevisionForPage(page)
        : 0;
    const bool ownsTextEdit = m_frameResizeOwnsTextEdit;
    resetFrameResizeState();
    if (ownsTextEdit) {
        finishSelectedTextEdit();
    }
    if (refreshesSourceComment
        && draftRevisionForPage(page) == suppressionRevision) {
        // Beginning a source comment gesture renders a texture with the source
        // annotation suppressed. A no-op finish still needs a new revision;
        // otherwise PDFPageItem can reuse that suppressed texture forever.
        markDraftStateChanged(page, true, false);
    }
}

void PDFEditController::cancelSelectedFrameResize()
{
    if (!m_frameResizeActive) {
        return;
    }

    const bool refreshesSourceComment = m_hasSelection
        && isGeometryCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.added;
    const int page = refreshesSourceComment
        ? m_selectedCandidate.page
        : -1;
    const quint64 suppressionRevision = refreshesSourceComment
        ? draftRevisionForPage(page)
        : 0;
    const bool ownsTextEdit = m_frameResizeOwnsTextEdit;
    const QRectF startFrameRect = m_frameResizeStartFrameRect;
    const QRectF startContentBounds = m_frameResizeStartContentBounds;
    const bool startGeometryExplicitlyChanged
        = m_frameResizeStartGeometryExplicitlyChanged;
    resetFrameResizeState();

    if (ownsTextEdit) {
        cancelSelectedTextEdit();
        if (refreshesSourceComment
            && draftRevisionForPage(page) == suppressionRevision) {
            markDraftStateChanged(page, true, false);
        }
        return;
    }

    Draft draft = selectedDraftForDisplay();
    draft.frameRect = startFrameRect;
    draft.contentBounds = startContentBounds;
    updateSelectedDraft(draft);
    m_textEditGeometryExplicitlyChanged = startGeometryExplicitlyChanged;
    if (refreshesSourceComment
        && draftRevisionForPage(page) == suppressionRevision) {
        markDraftStateChanged(page, true, false);
    }
}

void PDFEditController::setSelectedTextRange(int start, int length)
{
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const qsizetype textLength = m_selectedText.size();
    const qsizetype rangeStart = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(start),
        textLength);
    const qsizetype rangeLength = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(length),
        textLength - rangeStart);
    const bool rangeChanged = rangeStart != m_selectedTextRangeStart
        || rangeLength != m_selectedTextRangeLength;
    bool typingFormatChanged = false;
    if (m_typingCharacterFormatActive
        && (rangeLength > 0
            || rangeStart != m_typingCharacterFormatPosition)) {
        resetTypingFormat();
        typingFormatChanged = true;
    }
    m_selectedTextRangeStart = rangeStart;
    m_selectedTextRangeLength = rangeLength;
    if (rangeChanged || typingFormatChanged) {
        emit textFormatStateChanged();
    }
}

void PDFEditController::setSelectedFontFamily(const QString &fontFamily)
{
    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || fontFamily.trimmed().isEmpty()
        || !QFontDatabase::hasFamily(fontFamily)) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    const qsizetype rangeEnd = rangeStart + rangeLength;
    bool checkedSpan = false;
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qMax<qsizetype>(0, span.start);
        const qsizetype spanEnd = qMin<qsizetype>(
            draft.text.size(),
            span.start + span.length);
        if (spanEnd <= rangeStart || spanStart >= rangeEnd) {
            continue;
        }
        checkedSpan = true;
        if (!ApplicationFonts::isFontFaceSelectable(
                fontFamily,
                span.format.fontBold,
                span.format.fontItalic)) {
            return;
        }
    }
    if (!checkedSpan) {
        const CharacterFormat format = primaryCharacterFormat(draft);
        if (!ApplicationFonts::isFontFaceSelectable(
                fontFamily,
                format.fontBold,
                format.fontItalic)) {
            return;
        }
    }
    if (m_textEditActive
        && m_selectedTextRangeLength == 0
        && m_typingCharacterFormatActive
        && !ApplicationFonts::isFontFaceSelectable(
            fontFamily,
            m_typingCharacterFormat.fontBold,
            m_typingCharacterFormat.fontItalic)) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    draft = selectedDraftForDisplay();
    const bool changed = updateCharacterFormats(
        draft,
        rangeStart,
        rangeLength,
        [&fontFamily](CharacterFormat &format) {
            if (sameFontFamily(format.fontFamily, fontFamily)
                && format.fontStretch == QFont::Unstretched) {
                return;
            }
            format.fontFamily = fontFamily;
            format.fontStretch = QFont::Unstretched;
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    rebaseTypingFormat(draft);
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontBold(bool enabled)
{
    setSelectedFontStyle(FontStyle::Bold, enabled);
}

void PDFEditController::setSelectedFontItalic(bool enabled)
{
    setSelectedFontStyle(FontStyle::Italic, enabled);
}

void PDFEditController::setSelectedFontUnderline(bool enabled)
{
    setSelectedFontStyle(FontStyle::Underline, enabled);
}

void PDFEditController::setSelectedColor(const QColor &color)
{
    if (!m_hasSelection || !m_selectedCandidate.editable || !color.isValid()) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    Draft draft = selectedDraftForDisplay();
    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    const bool changed = updateCharacterFormats(
        draft,
        rangeStart,
        rangeLength,
        [&normalizedColor](CharacterFormat &format) {
            format.color = normalizedColor;
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    rebaseTypingFormat(draft);
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedParagraphAlignment(
    ParagraphAlignment alignment)
{
    switch (alignment) {
    case LeftAlignment:
    case RightAlignment:
    case CenterAlignment:
    case JustifyAlignment:
        break;
    default:
        return;
    }

    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    qsizetype first = -1;
    qsizetype last = -1;
    if (finishImmediately) {
        first = 0;
        last = draft.paragraphFormats.size() - 1;
    } else {
        selectedParagraphRangeForDraft(draft, first, last);
    }

    bool changed = false;
    for (qsizetype index = first; index >= 0 && index <= last; ++index) {
        ParagraphFormat &format = draft.paragraphFormats[index];
        if (format.alignment != alignment) {
            format.alignment = alignment;
            changed = true;
        }
    }
    if (changed) {
        updateSelectedDraft(draft);
    }

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontStyle(FontStyle style, bool enabled)
{
    if (!m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    const auto updatedStyle = [style, enabled](CharacterFormat format) {
        switch (style) {
        case FontStyle::Bold:
            format.fontBold = enabled;
            break;
        case FontStyle::Italic:
            format.fontItalic = enabled;
            break;
        case FontStyle::Underline:
            format.fontUnderline = enabled;
            break;
        }
        return format;
    };
    const auto faceIsSelectable = [](const CharacterFormat &format) {
        const bool knownFamily = ApplicationFonts::bundledFontFamilies().contains(
                format.fontFamily,
                Qt::CaseInsensitive)
            || ApplicationFonts::systemFontFamilies().contains(
                format.fontFamily,
                Qt::CaseInsensitive);
        return !knownFamily
            || ApplicationFonts::isFontFaceSelectable(
                format.fontFamily,
                format.fontBold,
                format.fontItalic);
    };

    Draft draft = selectedDraftForDisplay();
    if (m_textEditActive && m_selectedTextRangeLength == 0) {
        const qsizetype caretPosition = qBound<qsizetype>(
            0,
            m_selectedTextRangeStart,
            draft.text.size());
        const CharacterFormat currentFormat = selectedCharacterFormatForDisplay();
        const CharacterFormat typingFormat = updatedStyle(currentFormat);
        if ((style != FontStyle::Underline && !faceIsSelectable(typingFormat))
            || sameCharacterFormat(currentFormat, typingFormat)) {
            return;
        }

        const CharacterFormat inheritedFormat = characterFormatAtPosition(
            draft,
            caretPosition,
            true);
        if (sameCharacterFormat(typingFormat, inheritedFormat)) {
            resetTypingFormat();
        } else {
            m_typingCharacterFormat = typingFormat;
            m_typingCharacterFormatPosition = caretPosition;
            m_typingCharacterFormatActive = true;
        }
        emit textFormatStateChanged();
        return;
    }

    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    if (style != FontStyle::Underline) {
        const qsizetype rangeEnd = rangeStart + rangeLength;
        bool checkedSpan = false;
        for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
            const qsizetype spanStart = qMax<qsizetype>(0, span.start);
            const qsizetype spanEnd = qMin<qsizetype>(
                draft.text.size(),
                span.start + span.length);
            if (spanEnd <= rangeStart || spanStart >= rangeEnd) {
                continue;
            }
            checkedSpan = true;
            if (!faceIsSelectable(updatedStyle(span.format))) {
                return;
            }
        }
        if (!checkedSpan
            && !faceIsSelectable(updatedStyle(primaryCharacterFormat(draft)))) {
            return;
        }
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    draft = selectedDraftForDisplay();
    const bool changed = updateCharacterFormats(
        draft,
        rangeStart,
        rangeLength,
        [&updatedStyle](CharacterFormat &format) {
            format = updatedStyle(format);
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    if (m_textEditActive && rangeLength > 0) {
        emit selectionFontStyleApplied(
            static_cast<int>(rangeStart),
            static_cast<int>(rangeLength),
            static_cast<int>(style),
            enabled);
    }
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::finishSelectedTextEdit()
{
    if (!m_textEditActive) {
        return;
    }

    const Command baseline = m_textEditCommand;
    Draft finalizedDraft = selectedDraftForDisplay();
    if (m_textEditFrameAutomaticallyExpanded
        && !m_textEditGeometryExplicitlyChanged
        && finalizedDraft.text == baseline.before.text
        && sameParagraphFormats(
            finalizedDraft.paragraphFormats,
            baseline.before.paragraphFormats)) {
        finalizedDraft.frameRect = baseline.before.frameRect;
        finalizedDraft.contentBounds = baseline.before.contentBounds;
    }
    const bool draftChanged
        = !sameDraftContent(finalizedDraft, baseline.before);
    if (draftChanged) {
        applyUnicodeFontFallback(finalizedDraft);
        const bool movesGeometryComment
            = isGeometryCommentCandidateKind(
                  finalizedDraft.candidate.kind)
            && finalizedDraft.frameRect != baseline.before.frameRect;
        if (finalizedDraft.candidate.kind == QLatin1String("freeText")
            || movesGeometryComment) {
            finalizedDraft.candidate.commentModifiedDate
                = QDateTime::currentDateTime();
        }
    }
    updateSelectedDraft(finalizedDraft);
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    emit textEditActiveChanged();
    emit selectionChanged();

    Command command = baseline;
    const auto currentDraft = m_drafts.constFind(command.key);
    command.hadAfter = currentDraft != m_drafts.cend();
    command.after = command.hadAfter
        ? *currentDraft
        : initialDraftForCandidate(command.before.candidate);
    const bool removedAddedCandidate = command.before.candidate.added
        && !command.hadAfter
        && removeAddedCandidate(command.page, command.objectId);
    if (removedAddedCandidate) {
        if (m_hasSelection
            && m_selectedCandidate.page == command.page
            && m_selectedCandidate.objectId == command.objectId) {
            setSelection(nullptr);
        }
        emit pageDataChanged(command.page);
        if (isCommentCandidateKind(command.before.candidate.kind)) {
            emit commentsChanged();
        }
    }
    requestDraftRender(command.page, true);
    const bool changed = command.hadBefore != command.hadAfter
        || !sameDraftContent(command.before, command.after);
    if (!changed) {
        return;
    }

    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
}

void PDFEditController::cancelSelectedTextEdit()
{
    if (!m_textEditActive) {
        return;
    }

    const Command baseline = m_textEditCommand;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    emit textEditActiveChanged();
    applyDraftState(baseline, false, true);
    emit historyStateChanged();
}

void PDFEditController::commitSelectedText(const QString &text)
{
    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)) {
        if (m_hasSelection && !m_selectedCandidate.reason.isEmpty()) {
            emit statusMessage(m_selectedCandidate.reason);
        }
        return;
    }
    if (m_textEditActive) {
        updateSelectedTextPreview(text);
        finishSelectedTextEdit();
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool hadBefore = existingDraft != m_drafts.cend();
    const Draft before = hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    if (before.text == text) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = hadBefore;
    command.before = before;
    command.after = before;
    setDraftText(command.after, text);
    applyUnicodeFontFallback(command.after);
    command.hadAfter = !sameDraftContent(
        command.after,
        initialDraftForCandidate(m_selectedCandidate));

    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
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
    if (command.after.commentStateAuthor.trimmed().isEmpty()) {
        command.after.commentStateAuthor = currentCommentAuthor();
    }
    command.after.commentStateModifiedDate = QDateTime::currentDateTime();
    if (command.after.candidate.commentThreadId.isEmpty()) {
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
    if (command.after.commentStateAuthor.trimmed().isEmpty()) {
        command.after.commentStateAuthor = currentCommentAuthor();
    }
    command.after.commentStateModifiedDate = QDateTime::currentDateTime();
    if (command.after.candidate.commentThreadId.isEmpty()) {
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

    command.after.candidate.commentModifiedDate
        = QDateTime::currentDateTime();
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
        || !drawingSupportsFill(m_selectedCandidate.kind)
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
        || !drawingSupportsFill(m_selectedCandidate.kind)) {
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

void PDFEditController::deleteSelectedObject()
{
    if (!selectedDeletable()) {
        return;
    }
    if (isCommentCandidateKind(m_selectedCandidate.kind)) {
        removeCommentCandidate(m_selectedCandidate);
        return;
    }
    if (isTextCandidateKind(m_selectedCandidate.kind)) {
        commitSelectedText(QString());
        return;
    }

    finishSelectedTextEdit();
    const QString key = draftKey(
        m_selectedCandidate.page,
        m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    if (m_selectedCandidate.kind == QLatin1String("addedImage")
        || isShapeCandidateKind(m_selectedCandidate.kind)) {
        if (existingDraft == m_drafts.cend()) {
            return;
        }

        Command command;
        command.key = key;
        command.page = m_selectedCandidate.page;
        command.objectId = m_selectedCandidate.objectId;
        command.hadBefore = true;
        command.before = *existingDraft;
        command.hadAfter = false;
        command.after = command.before;
        m_undoStack.append(command);
        m_redoStack.clear();
        applyDraftState(command, true, true);
        emit historyStateChanged();
        return;
    }
    if (m_selectedCandidate.kind != QLatin1String("image")) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = existingDraft != m_drafts.cend();
    command.before = command.hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    if (command.before.deleted) {
        return;
    }
    command.hadAfter = true;
    command.after = command.before;
    command.after.deleted = true;
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
}

int PDFEditController::undo()
{
    if (m_saving || m_textEditActive) {
        return -1;
    }
    if (m_undoStack.isEmpty()) {
        if (m_formController && m_formController->canUndo()) {
            m_formController->undo();
        }
        return -1;
    }

    const Command command = m_undoStack.takeLast();
    applyDraftState(command, false, true);
    m_redoStack.append(command);
    emit historyStateChanged();
    return command.kind == Command::Kind::PageStructure && m_document
        ? m_document->currentPage()
        : -1;
}

int PDFEditController::redo()
{
    if (m_saving || m_textEditActive) {
        return -1;
    }
    if (m_redoStack.isEmpty()) {
        if (m_formController && m_formController->canRedo()) {
            m_formController->redo();
        }
        return -1;
    }

    const Command command = m_redoStack.takeLast();
    applyDraftState(command, true, true);
    m_undoStack.append(command);
    emit historyStateChanged();
    return command.kind == Command::Kind::PageStructure && m_document
        ? m_document->currentPage()
        : -1;
}

void PDFEditController::discardDrafts()
{
    if (m_saving
        || (!m_textEditActive
            && m_drafts.isEmpty()
            && m_pendingSquigglyCreations.isEmpty()
            && m_pageInsertions.isEmpty()
            && m_deletedSourcePages.isEmpty()
            && m_undoStack.isEmpty()
            && m_redoStack.isEmpty()
            && (!m_formController
                || (!m_formController->hasDrafts()
                    && !m_formController->canUndo()
                    && !m_formController->canRedo()))
            && (!m_formDesignerController
                || !m_formDesignerController->hasDrafts()))) {
        return;
    }

    QSet<int> affectedPages;
    for (auto revision = m_pageDraftRevisions.cbegin();
         revision != m_pageDraftRevisions.cend();
         ++revision) {
        affectedPages.insert(revision.key());
    }
    for (const PendingSquigglyCreation &creation
         : std::as_const(m_pendingSquigglyCreations)) {
        affectedPages.insert(creation.page);
    }
    const bool discardedPageStructure
        = !m_pageInsertions.isEmpty() || !m_deletedSourcePages.isEmpty();
    int deferredSourcePage = -1;
    if (discardedPageStructure && m_document) {
        deferredSourcePage = m_document->sourcePageIndex(
            m_deferredCandidateScanPage);
    }

    const bool wasEditing = m_textEditActive;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    if (discardedPageStructure) {
        if (m_candidateScanCancellation) {
            m_candidateScanCancellation->store(
                true,
                std::memory_order_relaxed);
        }
        m_deferredCandidateScanPage = -1;
        m_pageCandidates.clear();
        m_pageCandidateLru.clear();
        resetCommentIndex();
    }
    m_addedCandidates.clear();
    m_pendingSquigglyCreations.clear();
    m_pageInsertions.clear();
    m_deletedSourcePages.clear();
    if (m_document) {
        m_document->setTransientPageStructure(
            m_deletedSourcePages,
            m_pageInsertions);
    }
    if (discardedPageStructure) {
        if (deferredSourcePage >= 0
            && m_document
            && m_deferredCandidateScanPage < 0) {
            m_deferredCandidateScanPage
                = m_document->displayPageIndexForSourcePage(
                    deferredSourcePage);
        }
    }
    m_drafts.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    if (m_formController) {
        m_formController->discardChanges();
    }
    if (m_formDesignerController) {
        m_formDesignerController->resetFromDocument();
    }
    setSelection(nullptr);
    for (int page : std::as_const(affectedPages)) {
        markDraftStateChanged(page, true);
    }
    if (wasEditing) {
        emit textEditActiveChanged();
    }
    emit pageDataChanged(-1);
    if (discardedPageStructure) {
        emit commentsChanged();
        startPendingCandidateScan();
    }
    emit historyStateChanged();
    //% "All draft changes were discarded."
    emit statusMessage(qtTrId("AllDraftChangesDiscarded"));
}

bool PDFEditController::save()
{
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!hasDrafts()) {
        //% "There are no draft changes to save."
        emit statusMessage(qtTrId("NoDraftChangesToSave"));
        return true;
    }
    return saveAs(m_document->source());
}

bool PDFEditController::saveAs(const QUrl &target)
{
    if (m_saving) {
        return false;
    }
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!m_pendingSquigglyCreations.isEmpty()) {
        //% "Text information is still loading. Please try again."
        emit statusMessage(qtTrId("TextGeometryStillLoading"));
        return false;
    }
    if (!target.isLocalFile() || target.toLocalFile().isEmpty()) {
        //% "Only local PDF files are supported"
        emit statusMessage(qtTrId("LocalPdfFilesOnly"));
        return false;
    }

    finishSelectedTextEdit();
    PDFDocument *const expectedDocument = m_document.data();
    const int expectedRevision = expectedDocument->renderRevision();
    const QString expectedSourceFilePath = expectedDocument->renderFilePath();
    m_saving = true;
    emit savingChanged();
    const QString targetFilePath = QFileInfo(target.toLocalFile()).absoluteFilePath();
    QString temporarySourceFilePath;
    QString sourceFilePathOverride;
    QString snapshotError;
    if (m_formController && m_formController->hasDrafts()) {
        if (!m_formController->exportSnapshotBlocking(
                temporarySourceFilePath,
                snapshotError)) {
            return completeSave(
                PDFSaveWriter::Result{
                    PDFSaveWriter::Error::FormSnapshotFailed,
                    snapshotError},
                targetFilePath,
                expectedDocument,
                expectedRevision,
                expectedSourceFilePath);
        }
        sourceFilePathOverride = temporarySourceFilePath;
    }

    const PDFSaveWriter::Request request = PDFSaveWriter::prepare(
        *this,
        targetFilePath,
        sourceFilePathOverride);
    const PDFSaveWriter::Result result = PDFSaveWriter::save(request);
    return completeSave(
        result,
        targetFilePath,
        expectedDocument,
        expectedRevision,
        expectedSourceFilePath,
        temporarySourceFilePath);
}

bool PDFEditController::requestSave()
{
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!hasDrafts()) {
        //% "There are no draft changes to save."
        emit statusMessage(qtTrId("NoDraftChangesToSave"));
        emit saveFinished(true);
        return true;
    }
    return requestSaveAs(m_document->source());
}

bool PDFEditController::requestSaveAs(const QUrl &target)
{
    if (m_saving) {
        return false;
    }
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!m_pendingSquigglyCreations.isEmpty()) {
        //% "Text information is still loading. Please try again."
        emit statusMessage(qtTrId("TextGeometryStillLoading"));
        return false;
    }
    if (!target.isLocalFile() || target.toLocalFile().isEmpty()) {
        //% "Only local PDF files are supported"
        emit statusMessage(qtTrId("LocalPdfFilesOnly"));
        return false;
    }

    finishSelectedTextEdit();
    const QString targetFilePath = QFileInfo(target.toLocalFile()).absoluteFilePath();
    PDFDocument *const expectedDocument = m_document.data();
    const int expectedRevision = m_document->renderRevision();
    const QString expectedSourceFilePath = m_document->renderFilePath();
    m_saving = true;
    emit savingChanged();

    if (m_formController && m_formController->hasDrafts()) {
        QPointer<PDFEditController> controller(this);
        m_formController->exportSnapshot(
            [controller,
             targetFilePath,
             expectedDocument,
             expectedRevision,
             expectedSourceFilePath](
                bool succeeded,
                const QString &snapshotFilePath,
                const QString &error) {
                if (!controller) {
                    if (!snapshotFilePath.isEmpty()) {
                        QFile::remove(snapshotFilePath);
                    }
                    return;
                }
                if (!succeeded) {
                    controller->completeSave(
                        PDFSaveWriter::Result{
                            PDFSaveWriter::Error::FormSnapshotFailed,
                            error},
                        targetFilePath,
                        expectedDocument,
                        expectedRevision,
                        expectedSourceFilePath);
                    return;
                }
                controller->startAsyncSave(
                    targetFilePath,
                    expectedDocument,
                    expectedRevision,
                    expectedSourceFilePath,
                    snapshotFilePath,
                    snapshotFilePath);
            });
        return true;
    }

    startAsyncSave(
        targetFilePath,
        expectedDocument,
        expectedRevision,
        expectedSourceFilePath);
    return true;
}

void PDFEditController::startAsyncSave(
    const QString &targetFilePath,
    PDFDocument *expectedDocument,
    int expectedRevision,
    const QString &expectedSourceFilePath,
    const QString &sourceFilePathOverride,
    const QString &temporarySourceFilePath)
{
    const PDFSaveWriter::Request request = PDFSaveWriter::prepare(
        *this,
        targetFilePath,
        sourceFilePathOverride);
    if (!request.isValid()) {
        completeSave(
            PDFSaveWriter::Result{
                PDFSaveWriter::Error::SourceUnavailable},
            targetFilePath,
            expectedDocument,
            expectedRevision,
            expectedSourceFilePath,
            temporarySourceFilePath);
        return;
    }

    QPointer<PDFEditController> controller(this);
    QThreadPool::globalInstance()->start(QRunnable::create(
        [controller,
         expectedDocument,
         expectedRevision,
         expectedSourceFilePath,
         request,
         targetFilePath,
         temporarySourceFilePath]() {
            PDFSaveWriter::Result result = PDFSaveWriter::save(request);
            if (!controller) {
                if (!temporarySourceFilePath.isEmpty()) {
                    QFile::remove(temporarySourceFilePath);
                }
                return;
            }
            QObject *app = QCoreApplication::instance();
            if (!app) {
                if (!temporarySourceFilePath.isEmpty()) {
                    QFile::remove(temporarySourceFilePath);
                }
                return;
            }
            QMetaObject::invokeMethod(
                app,
                [controller,
                 expectedDocument,
                 expectedRevision,
                 expectedSourceFilePath,
                 result = std::move(result),
                 targetFilePath,
                 temporarySourceFilePath]() {
                    if (controller) {
                        controller->completeSave(
                            result,
                            targetFilePath,
                            expectedDocument,
                            expectedRevision,
                            expectedSourceFilePath,
                            temporarySourceFilePath);
                    } else if (!temporarySourceFilePath.isEmpty()) {
                        QFile::remove(temporarySourceFilePath);
                    }
                },
                Qt::QueuedConnection);
        }));
}

bool PDFEditController::completeSave(
    const PDFSaveWriter::Result &result,
    const QString &targetFilePath,
    PDFDocument *expectedDocument,
    int expectedRevision,
    const QString &expectedSourceFilePath,
    const QString &temporarySourceFilePath)
{
    if (!temporarySourceFilePath.isEmpty()) {
        QFile::remove(temporarySourceFilePath);
    }
    if (!result.succeeded()) {
        m_saving = false;
        emit savingChanged();
        emit statusMessage(saveFailureText(result));
        emit saveFinished(false);
        return false;
    }

    const bool documentUnchanged = m_document
        && m_document == expectedDocument
        && m_document->renderRevision() == expectedRevision
        && m_document->renderFilePath() == expectedSourceFilePath;
    if (!documentUnchanged) {
        m_saving = false;
        emit savingChanged();
        //% "Saved %1"
        emit statusMessage(qtTrId("PdfSavedFile").arg(
            QFileInfo(targetFilePath).fileName()));
        emit saveFinished(true);
        return true;
    }

    const bool reopened = m_document->open(QUrl::fromLocalFile(targetFilePath));
    m_saving = false;
    emit savingChanged();
    if (!reopened) {
        m_document->close();
        //% "The PDF was saved, but TrePDF could not reopen it."
        emit statusMessage(qtTrId("PdfSavedButReopenFailed"));
        emit saveFinished(true);
        return true;
    }

    //% "Saved %1"
    emit statusMessage(qtTrId("PdfSavedFile").arg(QFileInfo(targetFilePath).fileName()));
    emit saveFinished(true);
    return true;
}

void PDFEditController::resetSession()
{
    setPendingDrawingCommentPage(-1);
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_deferredCandidateScanPage = -1;
    m_pendingSquigglyCreations.clear();
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

void PDFEditController::ensureCacheRevision()
{
    const int revision = m_document ? m_document->renderRevision() : -1;
    if (m_cachedRevision == revision) {
        return;
    }

    const bool hadPendingSquigglyCreations
        = !m_pendingSquigglyCreations.isEmpty();
    m_cachedRevision = revision;
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_deferredCandidateScanPage = -1;
    m_pendingSquigglyCreations.clear();
    m_pageCandidates.clear();
    m_pageCandidateLru.clear();
    resetCommentIndex();
    emit commentsChanged();
    if (hadPendingSquigglyCreations) {
        emit historyStateChanged();
    }
}

const QVector<PDFEditController::Candidate> &PDFEditController::ensurePageCandidates(int page)
{
    static const QVector<Candidate> empty;

    ensureCacheRevision();
    if (!m_document || !m_document->isLoaded() || page < 0 || page >= m_document->pageCount()) {
        return empty;
    }

    if (m_document->isTransientBlankPage(page)
        && !m_pageCandidates.contains(page)) {
        m_pageCandidates.insert(page, {});
        touchPageCandidateCache(page);
        prunePageCandidateCache(page);
        emit pageCandidatesReady(page);
    }
    if (!m_pageCandidates.contains(page)) {
        requestPageCandidateScan(page);
        return empty;
    }
    touchPageCandidateCache(page);
    prunePageCandidateCache(page);
    return m_pageCandidates.constFind(page).value();
}

const QVector<PDFEditController::Candidate> *PDFEditController::cachedPageCandidates(
    int page)
{
    ensureCacheRevision();
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return nullptr;
    }

    auto cached = m_pageCandidates.constFind(page);
    if (cached != m_pageCandidates.cend()) {
        touchPageCandidateCache(page);
        cached = m_pageCandidates.constFind(page);
    }
    return cached == m_pageCandidates.cend() ? nullptr : &cached.value();
}

void PDFEditController::requestPageCandidateScan(int page)
{
    ensureCacheRevision();
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return;
    }

    if (m_pageCandidates.contains(page)) {
        if (page == m_deferredCandidateScanPage) {
            m_deferredCandidateScanPage = -1;
        }
        return;
    }
    if (m_document->isTransientBlankPage(page)) {
        if (page == m_deferredCandidateScanPage) {
            m_deferredCandidateScanPage = -1;
        }
        m_pageCandidates.insert(page, {});
        touchPageCandidateCache(page);
        prunePageCandidateCache(page);
        emit pageCandidatesReady(page);
        return;
    }
    if (m_document->pageSize(page).isEmpty()) {
        return;
    }

    const int priorityPage = pendingSquigglyScanPage();
    if (priorityPage < 0) {
        m_deferredCandidateScanPage = page;
    } else if (page != priorityPage) {
        // Preserve the latest ordinary request while unfinished Squiggly
        // geometry takes priority.
        m_deferredCandidateScanPage = page;
    }
    const int scanPage = priorityPage >= 0 ? priorityPage : page;

    if (m_candidateScanInFlight) {
        const bool cancellationPending
            = candidateScanCancelled(m_candidateScanCancellation);
        if (m_candidateScanInFlightPage != scanPage
            || cancellationPending) {
            if (m_candidateScanCancellation) {
                m_candidateScanCancellation->store(
                    true,
                    std::memory_order_relaxed);
            }
        }
        return;
    }

    startPendingCandidateScan();
}

void PDFEditController::startPendingCandidateScan()
{
    if (m_candidateScanInFlight) {
        return;
    }

    const int priorityPage = pendingSquigglyScanPage();
    const bool pendingSquigglyPriority = priorityPage >= 0;
    const int page = pendingSquigglyPriority
        ? priorityPage
        : m_deferredCandidateScanPage;
    if (page < 0) {
        return;
    }
    if (m_pageCandidates.contains(page)) {
        if (page == m_deferredCandidateScanPage) {
            m_deferredCandidateScanPage = -1;
        }
        return;
    }

    const CandidateScanRequest request = candidateScanRequest(page);
    if (request.page < 0) {
        if (page == m_deferredCandidateScanPage) {
            m_deferredCandidateScanPage = -1;
        }
        return;
    }

    m_candidateScanInFlight = true;
    m_candidateScanInFlightPage = page;
    m_candidateScanCancellation = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancellation
        = m_candidateScanCancellation;
    const QPointer<PDFEditController> controller(this);
    const auto deliver = [controller, request, cancellation](
                             QVector<Candidate> candidates,
                             bool cancelled) {
        QObject *app = QCoreApplication::instance();
        if (!app) {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [controller,
             request,
             candidates = std::move(candidates),
             cancellation,
             cancelled]() mutable {
                if (controller) {
                    controller->completePageCandidateScan(
                        request,
                        std::move(candidates),
                        cancelled || candidateScanCancelled(cancellation));
                }
            },
            Qt::QueuedConnection);
    };
    const auto deliverDropped = [controller, cancellation]() {
        cancellation->store(true, std::memory_order_relaxed);
        QObject *app = QCoreApplication::instance();
        if (!app) {
            return;
        }
        // A globally superseded request never ran, so do not retain its font
        // catalog or enqueue an empty candidate result. Each controller has at
        // most one such completion outstanding because its in-flight state is
        // reset only by this callback.
        QMetaObject::invokeMethod(
            app,
            [controller, cancellation]() {
                if (controller) {
                    controller->completeDroppedPageCandidateScan(cancellation);
                }
            },
            Qt::QueuedConnection);
    };
    candidateScanWorkQueue().submit(
        [request, cancellation, deliver]() mutable {
            QVector<Candidate> candidates = scanPageFromFile(
                request,
                cancellation);
            deliver(
                std::move(candidates),
                candidateScanCancelled(cancellation));
        },
        std::move(deliverDropped));
}

void PDFEditController::completePageCandidateScan(
    const CandidateScanRequest &request,
    QVector<Candidate> candidates,
    bool cancelled)
{
    m_candidateScanInFlight = false;
    m_candidateScanInFlightPage = -1;
    m_candidateScanCancellation.reset();
    ensureCacheRevision();

    const bool requestStillCurrent = !cancelled
        && m_document
        && m_document->isLoaded()
        && m_document->renderRevision() == request.revision
        && m_document->renderFilePath() == request.filePath
        && request.page >= 0
        && request.page < m_document->pageCount()
        && m_document->sourcePageIndex(request.page) == request.sourcePage
        && m_document->pageSize(request.page) == request.pageSize;
    if (requestStillCurrent && !m_pageCandidates.contains(request.page)) {
        QVector<Candidate> finalizedCandidates = finalizePageCandidates(
            std::move(candidates),
            request.page,
            request.pageSize);
        if (m_commentIndexState == CommentIndexIdle
            || m_commentIndexState == CommentIndexFailed) {
            if (!m_indexedCommentCandidates.contains(request.page)) {
                if (!cacheCommentCandidates(
                        request.page,
                        finalizedCandidates)) {
                    setCommentIndexState(CommentIndexLimited);
                }
            }
        }
        m_pageCandidates.insert(
            request.page,
            std::move(finalizedCandidates));
        touchPageCandidateCache(request.page);
        prunePageCandidateCache(request.page);
        completePendingSquigglyCreations(request.page);
        emit pageCandidatesReady(request.page);
        emit commentsChanged();
    }
    if (requestStillCurrent
        && request.page == m_deferredCandidateScanPage
        && m_pageCandidates.contains(request.page)) {
        m_deferredCandidateScanPage = -1;
    }

    startPendingCandidateScan();
}

void PDFEditController::completeDroppedPageCandidateScan(
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (m_candidateScanCancellation != cancellation) {
        return;
    }

    m_candidateScanInFlight = false;
    m_candidateScanInFlightPage = -1;
    m_candidateScanCancellation.reset();
    ensureCacheRevision();
    startPendingCandidateScan();
}

void PDFEditController::touchPageCandidateCache(int page)
{
    m_pageCandidateLru.removeAll(page);
    m_pageCandidateLru.append(page);
}

bool PDFEditController::isPageCandidateCachePinned(int page) const
{
    if (page == m_deferredCandidateScanPage
        || (m_hasSelection
            && !m_selectedCandidate.added
            && m_selectedCandidate.page == page)) {
        return true;
    }

    for (const PendingSquigglyCreation &creation
         : m_pendingSquigglyCreations) {
        if (creation.page == page) {
            return true;
        }
    }

    return std::any_of(
        m_drafts.cbegin(),
        m_drafts.cend(),
        [page](const Draft &draft) {
            return !draft.candidate.added && draft.candidate.page == page;
        });
}

void PDFEditController::prunePageCandidateCache(int protectedPage)
{
    while (m_pageCandidates.size() > kMaximumCachedCandidatePages) {
        bool removedPage = false;
        for (qsizetype index = 0; index < m_pageCandidateLru.size(); ++index) {
            const int page = m_pageCandidateLru.at(index);
            if (page == protectedPage || isPageCandidateCachePinned(page)) {
                continue;
            }

            m_pageCandidateLru.removeAt(index);
            m_pageCandidates.remove(page);
            removedPage = true;
            break;
        }
        if (!removedPage) {
            // Draft and selection state may legitimately pin more than the
            // normal viewport-sized cache budget.
            break;
        }
    }
}

void PDFEditController::resetCommentIndex()
{
    if (m_commentIndexCancellation) {
        m_commentIndexCancellation->store(true, std::memory_order_relaxed);
    }
    m_commentIndexCancellation.reset();
    setCommentIndexState(CommentIndexIdle);
    m_indexedCommentCandidates.clear();
    m_indexedCommentCandidateBytes = 0;
}

void PDFEditController::setCommentIndexState(CommentIndexState state)
{
    if (m_commentIndexState == state) {
        return;
    }
    m_commentIndexState = state;
    emit commentIndexStateChanged();
}

qsizetype PDFEditController::estimatedCommentCandidateBytes(
    const Candidate &candidate)
{
    qsizetype bytes = saturatedByteCountAdd(
        static_cast<qsizetype>(sizeof(Candidate)),
        kEstimatedIndexedCandidateOverhead);
    const auto addBytes = [&bytes](qsizetype additionalBytes) {
        bytes = saturatedByteCountAdd(bytes, additionalBytes);
    };
    const auto addString = [&addBytes](const QString &value) {
        addBytes(estimatedStringStorageBytes(value));
    };

    addString(candidate.objectId);
    addString(candidate.sourceWidgetId);
    addString(candidate.kind);
    addString(candidate.label);
    addString(candidate.text);
    addString(candidate.sourceAnnotationName);
    addString(candidate.commentAuthor);
    addString(candidate.commentState);
    addString(candidate.commentStateModel);
    addString(candidate.commentStateAuthor);
    addString(candidate.commentThreadId);
    addString(candidate.replyToObjectId);
    addString(candidate.replyToThreadId);
    addString(candidate.fontFamily);
    addString(candidate.fontIdentity);
    addString(candidate.stampText);
    addString(candidate.reason);
    addString(candidate.pendingFontExpectedFamily);
    addString(candidate.pendingBundledFontFamily);
    addString(candidate.pendingSystemFontFamily);
    addString(candidate.commentProperties.subject);
    addString(candidate.commentProperties.intent);
    addString(candidate.commentProperties.name);

    addBytes(estimatedContiguousStorageBytes(
        candidate.objectPath.capacity(),
        sizeof(int)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.parts.capacity(),
        sizeof(CandidatePart)));
    for (const CandidatePart &part : candidate.parts) {
        addString(part.objectId);
        addString(part.text);
        addString(part.separatorAfter);
        addBytes(estimatedContiguousStorageBytes(
            part.objectPath.capacity(),
            sizeof(int)));
    }
    addBytes(estimatedContiguousStorageBytes(
        candidate.textGlyphRects.capacity(),
        sizeof(QRectF)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.highlightRects.capacity(),
        sizeof(QRectF)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.textMarkupQuads.capacity(),
        sizeof(std::array<QPointF, 4>)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.drawingVertices.capacity(),
        sizeof(QPointF)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.inkStrokes.capacity(),
        sizeof(QVector<QPointF>)));
    for (const QVector<QPointF> &stroke : candidate.inkStrokes) {
        addBytes(estimatedContiguousStorageBytes(
            stroke.capacity(),
            sizeof(QPointF)));
    }
    addBytes(estimatedContiguousStorageBytes(
        candidate.commentProperties.dashPattern.capacity(),
        sizeof(qreal)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.sourceDrawingAppearanceFingerprint.capacity(),
        sizeof(char)));
    addBytes(estimatedContiguousStorageBytes(
        candidate.pendingEmbeddedFontData.capacity(),
        sizeof(char)));
    addBytes(candidate.image.sizeInBytes());
    return bytes;
}

qsizetype PDFEditController::estimatedCommentCandidatePageBytes(
    const QVector<Candidate> &candidates)
{
    const qsizetype spareCapacity = qMax<qsizetype>(
        0,
        candidates.capacity() - candidates.size());
    qsizetype bytes = estimatedContiguousStorageBytes(
        spareCapacity,
        sizeof(Candidate));
    for (const Candidate &candidate : candidates) {
        if (isCommentCandidateKind(candidate.kind)) {
            bytes = saturatedByteCountAdd(
                bytes,
                estimatedCommentCandidateBytes(candidate));
        }
    }
    return bytes;
}

bool PDFEditController::cacheCommentCandidates(
    int page,
    const QVector<Candidate> &candidates)
{
    QVector<Candidate> comments;
    for (const Candidate &candidate : candidates) {
        if (isCommentCandidateKind(candidate.kind)) {
            comments.append(candidate);
        }
    }

    const auto existing = m_indexedCommentCandidates.constFind(page);
    const qsizetype existingBytes
        = existing == m_indexedCommentCandidates.cend()
        ? 0
        : estimatedCommentCandidatePageBytes(existing.value());
    const qsizetype retainedWithoutPage
        = m_indexedCommentCandidateBytes >= existingBytes
        ? m_indexedCommentCandidateBytes - existingBytes
        : 0;
    const qsizetype replacementBytes
        = estimatedCommentCandidatePageBytes(comments);
    if (retainedWithoutPage > kMaximumCommentIndexBytes
        || replacementBytes
            > kMaximumCommentIndexBytes - retainedWithoutPage) {
        return false;
    }

    if (comments.isEmpty()) {
        m_indexedCommentCandidates.remove(page);
    } else {
        m_indexedCommentCandidates.insert(page, std::move(comments));
    }
    m_indexedCommentCandidateBytes = retainedWithoutPage + replacementBytes;
    return true;
}

PDFEditController::CandidateScanRequest PDFEditController::candidateScanRequest(
    int page) const
{
    CandidateScanRequest request;
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return request;
    }

    request.filePath = m_document->renderFilePath();
    request.revision = m_document->renderRevision();
    request.page = page;
    request.sourcePage = m_document->sourcePageIndex(page);
    request.pageSize = m_document->pageSize(page);
    request.editingAllowed = documentAllowsEditing();
    request.annotationsAllowed = documentAllowsAnnotations();
    request.commentAuthor = currentCommentAuthor();
    request.bundledFontFamilies = ApplicationFonts::bundledFontFamilies();
    request.systemFontFamilies = ApplicationFonts::systemFontFamilies();
    // A fixed per-scan budget keeps peak memory bounded while still allowing a
    // later page to fingerprint and reuse an already registered font after the
    // registration byte budget has been reached.
    request.maximumEmbeddedFontSnapshotBytes = kMaximumRegisteredFontBytes;
    if (request.filePath.isEmpty()
        || request.sourcePage < 0
        || request.pageSize.isEmpty()) {
        request.page = -1;
        request.sourcePage = -1;
    }
    return request;
}

QVector<PDFEditController::Candidate> PDFEditController::scanPageFromFile(
    const CandidateScanRequest &request,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    QVector<Candidate> candidates;
    if (candidateScanCancelled(cancellation)) {
        return candidates;
    }
    PDFiumProcessLock pdfiumLock;
    if (candidateScanCancelled(cancellation)) {
        return candidates;
    }

    thread_local CandidateScanDocument candidateDocument;
    FPDF_DOCUMENT document = candidateDocument.documentLocked(
        request.filePath,
        request.revision);
    if (document) {
        candidates = scanPageLocked(document, request, cancellation);
    }
    return candidates;
}

PDFEditController::CommentPageScanResult
PDFEditController::scanCommentPageLocked(
    FPDF_DOCUMENT document,
    const CandidateScanRequest &request,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    CommentPageScanResult result;
    if (!document
        || candidateScanCancelled(cancellation)
        || request.sourcePage < 0
        || request.pageSize.isEmpty()) {
        return result;
    }

    FPDF_PAGE page = FPDF_LoadPage(document, request.sourcePage);
    if (!page) {
        return result;
    }
    qsizetype estimatedCommentBytes = 0;
    bool commentMemoryBudgetExceeded = false;
    const bool annotationScanSucceeded = appendAnnotationCandidates(
        page,
        request,
        result.candidates,
        cancellation,
        estimatedCommentBytes,
        commentMemoryBudgetExceeded);
    FPDF_ClosePage(page);
    if (candidateScanCancelled(cancellation)) {
        result.candidates.clear();
        return result;
    }
    if (commentMemoryBudgetExceeded) {
        // A partial page could omit a reply ancestor or descendant and make
        // the retained thread look safely deletable when it is not.
        result.candidates.clear();
        result.memoryBudgetExceeded = true;
        result.succeeded = annotationScanSucceeded;
        return result;
    }

    result.candidates.erase(
        std::remove_if(
            result.candidates.begin(),
            result.candidates.end(),
            [](const Candidate &candidate) {
                return !isCommentCandidateKind(candidate.kind);
            }),
        result.candidates.end());
    applyCommentThreadDeletionProtection(result.candidates);
    result.estimatedRetainedBytes
        = estimatedCommentCandidatePageBytes(result.candidates);
    if (request.maximumCommentIndexBytes >= 0
        && result.estimatedRetainedBytes
            > request.maximumCommentIndexBytes) {
        result.candidates.clear();
        result.estimatedRetainedBytes = 0;
        result.memoryBudgetExceeded = true;
        result.succeeded = annotationScanSucceeded;
        return result;
    }
    result.succeeded = annotationScanSucceeded;
    return result;
}

QVector<PDFEditController::Candidate> PDFEditController::scanPageLocked(
    FPDF_DOCUMENT document,
    const CandidateScanRequest &request,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    QVector<Candidate> candidates;
    if (!document || candidateScanCancelled(cancellation)) {
        return candidates;
    }

    if (request.pageSize.isEmpty()) {
        return candidates;
    }

    FPDF_PAGE page = FPDF_LoadPage(document, request.sourcePage);
    FPDF_TEXTPAGE textPage = nullptr;
    if (page) {
        textPage = FPDFText_LoadPage(page);
    }
    if (!page) {
        return candidates;
    }

    QHash<FPDF_PAGEOBJECT, TextObjectLayout> textObjectLayouts;
    if (textPage) {
        const int characterCount = FPDFText_CountChars(textPage);
        // Keep the glyph cache bounded and all-or-nothing. A partial cache
        // could create a valid-looking annotation over only part of a page.
        const bool collectTextMarkupGlyphRects
            = characterCount >= 0
            && characterCount <= kMaximumTextMarkupCharactersPerPage;
        for (int index = 0;
             index < characterCount && !candidateScanCancelled(cancellation);
             ++index) {
            FPDF_PAGEOBJECT textObject = FPDFText_GetTextObject(textPage, index);
            if (!textObject) {
                continue;
            }

            auto objectLayout = textObjectLayouts.find(textObject);
            if (objectLayout == textObjectLayouts.end()) {
                objectLayout = textObjectLayouts.insert(textObject, TextObjectLayout());
            }
            const bool generated = FPDFText_IsGenerated(textPage, index) == 1;
            bool hasUsableUnicode = false;
            bool whitespace = false;
            if (!generated) {
                if (!objectLayout->hasOrigin) {
                    double originX = 0.0;
                    double originY = 0.0;
                    if (FPDFText_GetCharOrigin(textPage, index, &originX, &originY)) {
                        objectLayout->origin = QPointF(originX, originY);
                        objectLayout->hasOrigin = true;
                    }
                }

                const unsigned int unicode = FPDFText_GetUnicode(textPage, index);
                if (unicode > 0
                    && unicode <= 0x10FFFF
                    && !QChar::isSurrogate(static_cast<char32_t>(unicode))) {
                    const char32_t codePoint = static_cast<char32_t>(unicode);
                    hasUsableUnicode = true;
                    whitespace = QChar::isSpace(codePoint);
                    objectLayout->text.append(QString::fromUcs4(&codePoint, 1));
                }
            }

            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (!FPDFText_GetCharBox(
                    textPage, index, &left, &right, &bottom, &top)) {
                continue;
            }

            const QRectF characterBounds = QRectF(
                QPointF(left, bottom), QPointF(right, top)).normalized();
            if (characterBounds.isEmpty()) {
                continue;
            }

            if (objectLayout->bounds.isEmpty()) {
                objectLayout->bounds = characterBounds;
            } else {
                objectLayout->bounds = objectLayout->bounds.united(characterBounds);
            }
            // The current preview and writer use upright axis-aligned quads.
            // Do not create a misleading horizontal Squiggly for rotated text.
            if (!generated
                && hasUsableUnicode
                && !whitespace
                && collectTextMarkupGlyphRects
                && textMarkupAngleIsSupported(
                    FPDFText_GetCharAngle(textPage, index))) {
                objectLayout->glyphBounds.append(characterBounds);
            }
        }
    }

    QHash<FPDF_FONT, FontResolutionSnapshot> fontResolutionCache;
    qsizetype embeddedFontDataBytes = 0;
    const int objectCount = FPDFPage_CountObjects(page);
    for (int index = 0;
         index < objectCount && !candidateScanCancelled(cancellation);
         ++index) {
        appendPageObjectCandidates(
            page,
            FPDFPage_GetObject(page, index),
            QVector<int>{index},
            textPage,
            textObjectLayouts,
            kIdentityMatrix,
            request,
            candidates,
            fontResolutionCache,
            embeddedFontDataBytes,
            cancellation);
    }
    if (!candidateScanCancelled(cancellation)) {
        qsizetype ignoredCommentBytes = 0;
        bool ignoredCommentMemoryBudgetExceeded = false;
        appendAnnotationCandidates(
            page,
            request,
            candidates,
            cancellation,
            ignoredCommentBytes,
            ignoredCommentMemoryBudgetExceeded);
    }

    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    FPDF_ClosePage(page);

    return candidates;
}

void PDFEditController::applyCommentThreadDeletionProtection(
    QVector<Candidate> &candidates)
{
    QHash<int, qsizetype> candidateBySourceAnnotationIndex;
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        const int sourceAnnotationIndex
            = candidates.at(index).sourceAnnotationIndex;
        if (sourceAnnotationIndex >= 0) {
            candidateBySourceAnnotationIndex.insert(
                sourceAnnotationIndex,
                index);
        }
    }
    QSet<int> protectedAncestorIndices;
    for (const Candidate &candidate : std::as_const(candidates)) {
        const int flags = candidate.commentProperties.flags;
        if ((flags & (FPDF_ANNOT_FLAG_READONLY
                      | FPDF_ANNOT_FLAG_LOCKED
                      | kAnnotationFlagLockedContents)) == 0) {
            continue;
        }

        int ancestorIndex
            = candidate.replyDirectParentSourceAnnotationIndex;
        QSet<int> visitedIndices;
        while (ancestorIndex >= 0
               && !visitedIndices.contains(ancestorIndex)) {
            visitedIndices.insert(ancestorIndex);
            if (protectedAncestorIndices.contains(ancestorIndex)) {
                break;
            }
            protectedAncestorIndices.insert(ancestorIndex);
            const auto ancestor
                = candidateBySourceAnnotationIndex.constFind(ancestorIndex);
            if (ancestor == candidateBySourceAnnotationIndex.cend()) {
                break;
            }
            ancestorIndex = candidates.at(*ancestor)
                .replyDirectParentSourceAnnotationIndex;
        }
    }
    for (Candidate &candidate : candidates) {
        candidate.commentThreadDeletionProtected
            = protectedAncestorIndices.contains(
                candidate.sourceAnnotationIndex);
    }
}

QVector<PDFEditController::Candidate> PDFEditController::finalizePageCandidates(
    QVector<Candidate> candidates,
    int page,
    const QSizeF &pageSize)
{
    assertPDFiumProcessLockNotHeld();
    QHash<int, QString> resolvedEmbeddedFamilies;
    for (Candidate &candidate : candidates) {
        if (candidate.kind == QLatin1String("text")
            && !candidate.textMarkupHitTestOnly) {
            resolveCandidateFontFamily(candidate, resolvedEmbeddedFamilies);
            expandInitialTextFrame(candidate, pageSize);
        }
        candidate.page = page;
    }
    candidates = groupTextLineCandidates(std::move(candidates), pageSize);
    applyCommentThreadDeletionProtection(candidates);
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate &left, const Candidate &right) {
            return left.editable < right.editable;
        });
    return candidates;
}

void PDFEditController::appendPageObjectCandidates(
    FPDF_PAGE page,
    FPDF_PAGEOBJECT pageObject,
    const QVector<int> &objectPath,
    FPDF_TEXTPAGE textPage,
    const QHash<FPDF_PAGEOBJECT, TextObjectLayout> &textObjectLayouts,
    const FS_MATRIX &containerTransform,
    const CandidateScanRequest &request,
    QVector<Candidate> &candidates,
    QHash<FPDF_FONT, FontResolutionSnapshot> &fontResolutionCache,
    qsizetype &embeddedFontDataBytes,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    if (!pageObject || candidateScanCancelled(cancellation)) {
        return;
    }
    const QSizeF &pageSize = request.pageSize;

    const int type = FPDFPageObj_GetType(pageObject);
    if (type == FPDF_PAGEOBJ_FORM) {
        FS_MATRIX formTransform = kIdentityMatrix;
        if (!FPDFPageObj_GetMatrix(pageObject, &formTransform)) {
            formTransform = kIdentityMatrix;
        }
        const FS_MATRIX childTransform = combinedTransform(containerTransform, formTransform);
        const int count = FPDFFormObj_CountObjects(pageObject);
        for (int index = 0;
             index < count && !candidateScanCancelled(cancellation);
             ++index) {
            QVector<int> childPath = objectPath;
            childPath.append(index);
            appendPageObjectCandidates(
                page,
                FPDFFormObj_GetObject(pageObject, static_cast<unsigned long>(index)),
                childPath,
                textPage,
                textObjectLayouts,
                childTransform,
                request,
                candidates,
                fontResolutionCache,
                embeddedFontDataBytes,
                cancellation);
        }
        return;
    }

    if (type != FPDF_PAGEOBJ_TEXT && type != FPDF_PAGEOBJ_IMAGE) {
        return;
    }
    if (type == FPDF_PAGEOBJ_IMAGE
        && pageObjectHasContentMark(
            pageObject,
            QString::fromLatin1(kPreservedPaintMark))) {
        return;
    }

    const FPDF_TEXT_RENDERMODE renderMode = type == FPDF_PAGEOBJ_TEXT
        ? FPDFTextObj_GetTextRenderMode(pageObject)
        : FPDF_TEXTRENDERMODE_UNKNOWN;
    const bool textMarkupHitTestOnly
        = type == FPDF_PAGEOBJ_TEXT
        && renderMode == FPDF_TEXTRENDERMODE_INVISIBLE;
    if (type == FPDF_PAGEOBJ_TEXT
        && (renderMode == FPDF_TEXTRENDERMODE_UNKNOWN
            || renderMode == FPDF_TEXTRENDERMODE_CLIP)) {
        return;
    }

    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    if (!FPDFPageObj_GetBounds(pageObject, &left, &bottom, &right, &top)) {
        return;
    }

    QRectF rect = normalizedRect(
        page,
        left,
        bottom,
        right,
        top,
        containerTransform,
        pageSize);
    const auto textLayout = type == FPDF_PAGEOBJ_TEXT
        ? textObjectLayouts.constFind(pageObject)
        : textObjectLayouts.cend();
    if (textLayout != textObjectLayouts.cend()) {
        if (!textLayout->bounds.isEmpty()) {
            const QRectF glyphRect = normalizedRect(
                page,
                textLayout->bounds.left(),
                textLayout->bounds.top(),
                textLayout->bounds.right(),
                textLayout->bounds.bottom(),
                kIdentityMatrix,
                pageSize);
            if (!glyphRect.isEmpty()) {
                rect = glyphRect;
            }
        }
    }
    if (rect.isEmpty()) {
        return;
    }

    Candidate candidate;
    candidate.objectId = pageObjectId(objectPath);
    candidate.objectPath = objectPath;
    candidate.textMarkupHitTestOnly = textMarkupHitTestOnly;
    setInitialGeometry(candidate, rect);
    if (textLayout != textObjectLayouts.cend()) {
        candidate.textGlyphRects.reserve(textLayout->glyphBounds.size());
        for (const QRectF &glyphBounds : textLayout->glyphBounds) {
            const QRectF glyphRect = normalizedRect(
                page,
                glyphBounds.left(),
                glyphBounds.top(),
                glyphBounds.right(),
                glyphBounds.bottom(),
                kIdentityMatrix,
                pageSize);
            if (!glyphRect.isEmpty()) {
                candidate.textGlyphRects.append(glyphRect);
            }
        }
    }
    if (textLayout != textObjectLayouts.cend() && textLayout->hasOrigin) {
        candidate.hasBaseline = normalizedPagePoint(
            page,
            pageSize,
            textLayout->origin,
            candidate.origin);
    }

    if (textMarkupHitTestOnly) {
        if (!candidate.textGlyphRects.isEmpty()) {
            candidate.kind = QStringLiteral("text");
            candidates.append(std::move(candidate));
        }
        return;
    }

    if (type == FPDF_PAGEOBJ_IMAGE) {
        candidate.kind = QStringLiteral("image");
        //% "Image"
        candidate.label = qtTrId("Image");
        const bool editingAllowed = request.editingAllowed;
        const bool topLevelObject = objectPath.size() == 1;
        candidate.editable = editingAllowed && topLevelObject;
        candidate.renderSuppressible = true;
        candidate.limited = true;
        if (!editingAllowed) {
            //% "This PDF does not permit content editing."
            candidate.reason = qtTrId("PdfEditingNotPermitted");
        } else if (!topLevelObject) {
            //% "Images inside a PDF form object cannot be deleted safely yet."
            candidate.reason = qtTrId("PdfFormImageDeleteUnsupported");
        }
        candidates.append(candidate);
        return;
    }

    const QString text = textLayout != textObjectLayouts.cend()
            && !textLayout->text.isEmpty()
        ? textLayout->text
        : textObjectText(pageObject, textPage);
    if (text.trimmed().isEmpty()) {
        return;
    }

    float fontSize = 0.0F;
    FPDFTextObj_GetFontSize(pageObject, &fontSize);
    FS_MATRIX objectTransform = kIdentityMatrix;
    if (!FPDFPageObj_GetMatrix(pageObject, &objectTransform)) {
        objectTransform = kIdentityMatrix;
    }
    const FS_MATRIX textTransform = combinedTransform(containerTransform, objectTransform);
    const bool suppressionSupported = isSuppressibleTextRenderMode(renderMode);
    const bool inlineLayoutSupported = supportsInlineTextLayout(page, pageSize, textTransform)
        && !text.contains(QLatin1Char('\n'))
        && !text.contains(QLatin1Char('\r'));
    candidate.kind = QStringLiteral("text");
    //% "Text"
    candidate.label = qtTrId("Text");
    candidate.text = text;
    snapshotFontResolutionLocked(
        pageObject,
        candidate,
        request,
        fontResolutionCache,
        embeddedFontDataBytes);
    candidate.fontSizeRatio = qMax<qreal>(
        0.0,
        normalizedFontSize(page, pageSize, textTransform, fontSize));
    candidate.displayFontSizeRatio = candidate.fontSizeRatio;
    candidate.horizontalScale = horizontalScaleForTransform(
        page,
        pageSize,
        textTransform);
    candidate.fontStretch = qBound(
        static_cast<int>(QFont::UltraCondensed),
        qRound(candidate.horizontalScale * QFont::Unstretched),
        static_cast<int>(QFont::UltraExpanded));
    candidate.textRenderMode = renderMode;
    candidate.fillColor = pageObjectColor(pageObject, false);
    candidate.strokeColor = pageObjectColor(pageObject, true);
    float strokeWidth = 1.0F;
    if (FPDFPageObj_GetStrokeWidth(pageObject, &strokeWidth)) {
        candidate.strokeWidth = qMax<qreal>(0.0, strokeWidth);
    }
    candidate.color = textColor(pageObject, renderMode);
    const bool editingAllowed = request.editingAllowed;
    const bool topLevelObject = objectPath.size() == 1;
    candidate.editable = editingAllowed
        && suppressionSupported
        && topLevelObject
        && inlineLayoutSupported;
    candidate.renderSuppressible = suppressionSupported;
    candidate.limited = true;
    if (!editingAllowed) {
        //% "This PDF does not permit content editing."
        candidate.reason = qtTrId("PdfEditingNotPermitted");
    } else if (!suppressionSupported) {
        //% "Text clipping render modes cannot be edited safely yet."
        candidate.reason = qtTrId("PdfTextClippingEditUnsupported");
    } else if (!topLevelObject) {
        //% "Text inside a PDF form object cannot be saved safely yet."
        candidate.reason = qtTrId("PdfFormTextSaveUnsupported");
    } else if (!inlineLayoutSupported) {
        //% "Rotated, transformed, or multiline text is not supported by the inline draft editor yet."
        candidate.reason = qtTrId("InlineEditorUnsupportedTextLayout");
    }

    CandidatePart part;
    part.objectId = candidate.objectId;
    part.objectPath = candidate.objectPath;
    part.sourceRect = candidate.geometry.sourceRect;
    part.origin = candidate.origin;
    part.text = candidate.text;
    part.hasBaseline = candidate.hasBaseline;
    candidate.parts.append(part);
    candidates.append(candidate);
}

QVector<PDFEditController::Candidate> PDFEditController::groupTextLineCandidates(
    QVector<Candidate> candidates,
    const QSizeF &pageSize) const
{
    if (candidates.size() < 2 || pageSize.isEmpty()) {
        return candidates;
    }

    struct IndexedCandidate {
        Candidate candidate;
        qsizetype order = -1;
    };
    struct TextLine {
        QVector<IndexedCandidate> candidates;
        qreal baseline = 0.0;
        qreal emSize = 0.0;
    };

    const auto displayFontSizeRatio = [](const Candidate &candidate) {
        return candidate.displayFontSizeRatio > 0.0
            ? candidate.displayFontSizeRatio
            : candidate.fontSizeRatio;
    };
    const auto emSize = [&](const Candidate &candidate) {
        return displayFontSizeRatio(candidate) * pageSize.height();
    };
    const auto canJoinLine = [&](const Candidate &candidate) {
        return candidate.kind == QLatin1String("text")
            && candidate.editable
            && candidate.hasBaseline
            && !candidate.added
            && candidate.parts.size() == 1
            && emSize(candidate) > 0.0;
    };

    QVector<TextLine> lines;
    QVector<IndexedCandidate> output;
    output.reserve(candidates.size());
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        Candidate candidate = std::move(candidates[index]);
        if (!canJoinLine(candidate)) {
            output.append(IndexedCandidate{std::move(candidate), index});
            continue;
        }

        const qreal candidateBaseline = candidate.origin.y() * pageSize.height();
        const qreal candidateEmSize = emSize(candidate);
        TextLine *bestLine = nullptr;
        qreal bestDistance = -1.0;
        for (TextLine &line : lines) {
            const qreal distance = qAbs(candidateBaseline - line.baseline);
            const qreal tolerance = kLineBaselineToleranceEm
                * qMax(candidateEmSize, line.emSize);
            if (distance <= tolerance
                && (bestDistance < 0.0 || distance < bestDistance)) {
                bestLine = &line;
                bestDistance = distance;
            }
        }

        if (!bestLine) {
            TextLine line;
            line.baseline = candidateBaseline;
            line.emSize = candidateEmSize;
            line.candidates.append(IndexedCandidate{std::move(candidate), index});
            lines.append(std::move(line));
            continue;
        }

        const qsizetype existingCount = bestLine->candidates.size();
        bestLine->baseline = (bestLine->baseline * existingCount + candidateBaseline)
            / (existingCount + 1);
        bestLine->emSize = qMax(bestLine->emSize, candidateEmSize);
        bestLine->candidates.append(IndexedCandidate{std::move(candidate), index});
    }

    const auto sameStyle = [&](const Candidate &left, const Candidate &right) {
        const qreal leftSize = emSize(left);
        const qreal rightSize = emSize(right);
        const qreal sizeTolerance = qMax(
            kLineFontSizeTolerancePoints,
            qMax(leftSize, rightSize) * kLineFontSizeToleranceRatio);
        const QString &leftFont = left.fontIdentity.isEmpty()
            ? left.fontFamily
            : left.fontIdentity;
        const QString &rightFont = right.fontIdentity.isEmpty()
            ? right.fontFamily
            : right.fontIdentity;
        return !leftFont.isEmpty()
            && leftFont.compare(rightFont, Qt::CaseInsensitive) == 0
            && qAbs(leftSize - rightSize) <= sizeTolerance
            && qAbs(left.fontStretch - right.fontStretch) <= kLineFontStretchTolerance
            && left.fontBold == right.fontBold
            && left.fontItalic == right.fontItalic
            && left.fontUnderline == right.fontUnderline
            && left.color == right.color
            && left.fillColor == right.fillColor
            && left.strokeColor == right.strokeColor
            && qFuzzyCompare(left.strokeWidth + 1.0, right.strokeWidth + 1.0)
            && left.textRenderMode == right.textRenderMode;
    };
    const auto horizontalGap = [&](const Candidate &left, const Candidate &right) {
        return (right.geometry.sourceRect.left() - left.geometry.sourceRect.right())
            * pageSize.width();
    };
    const auto areNeighbors = [&](const Candidate &left, const Candidate &right) {
        const qreal gap = horizontalGap(left, right);
        const qreal sharedEmSize = qMax(emSize(left), emSize(right));
        return gap >= kLineMinimumHorizontalGapEm * sharedEmSize
            && gap <= kLineMaximumHorizontalGapEm * sharedEmSize;
    };
    const auto separatorBetween = [&](const Candidate &left, const Candidate &right) {
        if (left.text.isEmpty()
            || right.text.isEmpty()
            || left.text.back().isSpace()
            || right.text.front().isSpace()) {
            return QString();
        }
        const qreal sharedEmSize = qMax(emSize(left), emSize(right));
        return horizontalGap(left, right) > kLineWordSeparatorGapEm * sharedEmSize
            ? QString(QLatin1Char(' '))
            : QString();
    };

    for (TextLine &line : lines) {
        std::stable_sort(
            line.candidates.begin(),
            line.candidates.end(),
            [](const IndexedCandidate &left, const IndexedCandidate &right) {
                if (!qFuzzyCompare(
                        left.candidate.geometry.sourceRect.left() + 1.0,
                        right.candidate.geometry.sourceRect.left() + 1.0)) {
                    return left.candidate.geometry.sourceRect.left()
                        < right.candidate.geometry.sourceRect.left();
                }
                return left.order < right.order;
            });

        QVector<IndexedCandidate> run;
        const auto flushRun = [&]() {
            if (run.isEmpty()) {
                return;
            }
            if (run.size() == 1) {
                output.append(std::move(run.front()));
                run.clear();
                return;
            }

            Candidate grouped = run.front().candidate;
            grouped.objectPath.clear();
            grouped.parts.clear();
            grouped.text.clear();
            grouped.textGlyphRects.clear();
            QRectF groupedSourceRect = run.front().candidate.geometry.sourceRect;
            grouped.renderSuppressible = true;
            grouped.limited = false;
            qsizetype firstOrder = run.front().order;
            QByteArray identity;

            for (qsizetype index = 0; index < run.size(); ++index) {
                const Candidate &source = run[index].candidate;
                firstOrder = qMin(firstOrder, run[index].order);
                groupedSourceRect = groupedSourceRect.united(source.geometry.sourceRect);
                grouped.renderSuppressible = grouped.renderSuppressible
                    && source.renderSuppressible;
                grouped.limited = grouped.limited || source.limited;

                if (index > 0) {
                    const QString separator = separatorBetween(
                        run[index - 1].candidate,
                        source);
                    grouped.parts.last().separatorAfter = separator;
                    grouped.text.append(separator);
                }

                CandidatePart part = source.parts.constFirst();
                part.separatorAfter.clear();
                grouped.parts.append(std::move(part));
                grouped.text.append(source.text);
                grouped.textGlyphRects.append(source.textGlyphRects);
                identity.append(source.objectId.toUtf8());
                identity.append('\0');
            }

            grouped.objectId = QStringLiteral("line:")
                + QString::fromLatin1(QCryptographicHash::hash(
                    identity,
                    QCryptographicHash::Sha256).toHex());
            setInitialGeometry(grouped, groupedSourceRect);
            expandInitialTextFrame(grouped, pageSize);
            output.append(IndexedCandidate{std::move(grouped), firstOrder});
            run.clear();
        };

        for (IndexedCandidate &candidate : line.candidates) {
            if (!run.isEmpty()) {
                const Candidate &previous = run.constLast().candidate;
                if (!sameStyle(previous, candidate.candidate)
                    || !areNeighbors(previous, candidate.candidate)) {
                    flushRun();
                }
            }
            run.append(std::move(candidate));
        }
        flushRun();
    }

    std::stable_sort(
        output.begin(),
        output.end(),
        [](const IndexedCandidate &left, const IndexedCandidate &right) {
            return left.order < right.order;
        });
    QVector<Candidate> groupedCandidates;
    groupedCandidates.reserve(output.size());
    for (IndexedCandidate &candidate : output) {
        groupedCandidates.append(std::move(candidate.candidate));
    }
    return groupedCandidates;
}

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
        QHash<QString, qsizetype> rootsByObjectId;
        QHash<QString, QVector<const Candidate *>> stateHistoriesByRoot;
        for (qsizetype candidateIndex = 0;
             candidateIndex < candidates.size();
             ++candidateIndex) {
            const Candidate &candidate = candidates.at(candidateIndex);
            if (isCommentCandidateKind(candidate.kind)
                && !candidate.commentStateAnnotation
                && !candidate.isReplyAnnotation
                && candidate.replyToObjectId.isEmpty()) {
                rootsByObjectId.insert(candidate.objectId, candidateIndex);
                continue;
            }
            if (candidate.commentStateAnnotation
                && !candidate.replyToObjectId.isEmpty()
                && candidate.commentAuthor.trimmed().compare(
                       requestedStateAuthor,
                       Qt::CaseInsensitive) == 0) {
                stateHistoriesByRoot[candidate.replyToObjectId].append(
                    &candidate);
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

        for (auto rootHistory = stateHistoriesByRoot.cbegin();
             rootHistory != stateHistoriesByRoot.cend();
             ++rootHistory) {
            const auto rootIndex = rootsByObjectId.constFind(
                rootHistory.key());
            if (rootIndex == rootsByObjectId.cend()) {
                continue;
            }

            QVector<AuthorHistory> authorHistories;
            QHash<QString, qsizetype> historyIndexByAuthor;
            // Match environment/profile aliases case-insensitively only long
            // enough to find their histories. From this point onward, exact
            // spelling identifies a reviewer, matching Foxit's behavior.
            for (const Candidate *stateCandidate : rootHistory.value()) {
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

            const AuthorHistory *selectedHistory = &authorHistories.constFirst();
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

            Candidate &rootCandidate = candidates[*rootIndex];
            rootCandidate.commentStateAuthor = selectedHistory->author;
            // A state transition is ordered by /IRT, not by its position in
            // /Annots. Date and annotation index only resolve equal-depth
            // terminal forks in malformed or concurrently edited files.
            const Candidate *reviewState = terminalStateForModel(
                *selectedHistory,
                QLatin1StringView("Review"));
            if (reviewState) {
                rootCandidate.commentStatusAnnotationIndex
                    = reviewState->sourceAnnotationIndex;
                rootCandidate.commentStatus = commentStatusForPdfName(
                    reviewState->commentState).value_or(NoCommentStatus);
            }
            const Candidate *markedState = terminalStateForModel(
                *selectedHistory,
                QLatin1StringView("Marked"));
            if (markedState) {
                rootCandidate.commentMarkedAnnotationIndex
                    = markedState->sourceAnnotationIndex;
                rootCandidate.commentMarked
                    = markedState->commentState.compare(
                          QLatin1String("Marked"),
                          Qt::CaseInsensitive) == 0;
            }
        }
    }
    return succeeded;
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

QVariantList PDFEditController::candidateSourceRects(const Candidate &candidate) const
{
    QVariantList sourceRects;
    const auto appendRect = [&sourceRects](const QRectF &rect) {
        QVariantMap map;
        map.insert(QStringLiteral("x"), rect.x());
        map.insert(QStringLiteral("y"), rect.y());
        map.insert(QStringLiteral("width"), rect.width());
        map.insert(QStringLiteral("height"), rect.height());
        sourceRects.append(map);
    };

    if (!candidate.highlightRects.isEmpty()) {
        sourceRects.reserve(candidate.highlightRects.size());
        for (const QRectF &rect : candidate.highlightRects) {
            appendRect(rect);
        }
    } else if (candidate.parts.isEmpty()) {
        appendRect(candidate.geometry.sourceRect);
    } else {
        sourceRects.reserve(candidate.parts.size());
        for (const CandidatePart &part : candidate.parts) {
            appendRect(part.sourceRect);
        }
    }
    return sourceRects;
}

QVariantList PDFEditController::characterSpanMaps(
    const QVector<CharacterSpan> &spans) const
{
    QVariantList maps;
    maps.reserve(spans.size());
    for (const CharacterSpan &span : spans) {
        QVariantMap map;
        map.insert(QStringLiteral("start"), span.start);
        map.insert(QStringLiteral("length"), span.length);
        map.insert(QStringLiteral("end"), span.start + span.length);
        map.insert(QStringLiteral("fontFamily"), span.format.fontFamily);
        map.insert(QStringLiteral("fontSizeRatio"), span.format.fontSizeRatio);
        map.insert(
            QStringLiteral("displayFontSizeRatio"),
            span.format.displayFontSizeRatio > 0.0
                ? span.format.displayFontSizeRatio
                : span.format.fontSizeRatio);
        map.insert(QStringLiteral("fontStretch"), span.format.fontStretch);
        map.insert(QStringLiteral("fontBold"), span.format.fontBold);
        map.insert(QStringLiteral("fontItalic"), span.format.fontItalic);
        map.insert(QStringLiteral("fontUnderline"), span.format.fontUnderline);
        map.insert(QStringLiteral("color"), span.format.color);
        maps.append(map);
    }
    return maps;
}

QVariantList PDFEditController::paragraphFormatMaps(
    const QVector<ParagraphFormat> &formats) const
{
    const auto alignmentName = [](ParagraphAlignment alignment) {
        switch (alignment) {
        case LeftAlignment:
            return QStringLiteral("left");
        case CenterAlignment:
            return QStringLiteral("center");
        case RightAlignment:
            return QStringLiteral("right");
        case JustifyAlignment:
            return QStringLiteral("justify");
        }
        return QStringLiteral("left");
    };
    const auto listStyleName = [](ParagraphListStyle listStyle) {
        switch (listStyle) {
        case ParagraphListStyle::None:
            return QStringLiteral("none");
        case ParagraphListStyle::Bullet:
            return QStringLiteral("bullet");
        case ParagraphListStyle::Decimal:
            return QStringLiteral("decimal");
        }
        return QStringLiteral("none");
    };

    QVariantList maps;
    maps.reserve(formats.size());
    for (const ParagraphFormat &format : formats) {
        QVariantMap map;
        map.insert(QStringLiteral("start"), format.start);
        map.insert(QStringLiteral("length"), format.length);
        map.insert(QStringLiteral("end"), format.start + format.length);
        map.insert(QStringLiteral("alignment"), alignmentName(format.alignment));
        map.insert(QStringLiteral("listStyle"), listStyleName(format.listStyle));
        map.insert(QStringLiteral("listLevel"), format.listLevel);
        map.insert(QStringLiteral("listStart"), format.listStart);
        maps.append(map);
    }
    return maps;
}

QVariantMap PDFEditController::candidateMap(const Draft &draft) const
{
    const Candidate &candidate = draft.candidate;
    const PDFCommentAnnotation::CommonProperties &commentProperties
        = draft.commentProperties;
    const CharacterFormat format = primaryCharacterFormat(draft);
    const QPointF layoutOrigin = layoutOriginForDraft(draft);
    const QRectF initialFrameRect = candidate.initialDraftFrameRect.isEmpty()
        ? candidate.geometry.sourceRect
        : candidate.initialDraftFrameRect;
    QVariantMap map;
    map.insert(QStringLiteral("objectId"), candidate.objectId);
    map.insert(QStringLiteral("kind"), candidate.kind);
    map.insert(QStringLiteral("label"), candidate.label);
    map.insert(QStringLiteral("sourceRect"), candidate.geometry.sourceRect);
    map.insert(QStringLiteral("frameRect"), draft.frameRect);
    map.insert(QStringLiteral("contentBounds"), draft.contentBounds);
    map.insert(
        QStringLiteral("geometryChanged"),
        draft.frameRect != initialFrameRect);
    map.insert(
        QStringLiteral("styleChanged"),
        isDrawingCommentCandidateKind(candidate.kind)
            && commentProperties != candidate.commentProperties);
    map.insert(QStringLiteral("deleted"), draft.deleted);
    map.insert(QStringLiteral("sourceRects"), candidateSourceRects(candidate));
    map.insert(QStringLiteral("grouped"), candidate.parts.size() > 1);
    map.insert(QStringLiteral("hasBaseline"), candidate.hasBaseline);
    map.insert(QStringLiteral("originX"), layoutOrigin.x());
    map.insert(QStringLiteral("baselineY"), layoutOrigin.y());
    map.insert(QStringLiteral("text"), draft.text);
    map.insert(
        QStringLiteral("commentAuthor"),
        candidate.commentAuthor);
    map.insert(
        QStringLiteral("commentCreationDate"),
        candidate.commentCreationDate);
    map.insert(
        QStringLiteral("commentModifiedDate"),
        candidate.commentModifiedDate);
    map.insert(
        QStringLiteral("commentDate"),
        candidate.commentModifiedDate.isValid()
            ? candidate.commentModifiedDate
            : candidate.commentCreationDate);
    map.insert(
        QStringLiteral("commentStatus"),
        static_cast<int>(draft.commentStatus));
    map.insert(
        QStringLiteral("commentMarked"),
        draft.commentMarked);
    const bool stateEditable = commentStateEditable(candidate);
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
        QStringLiteral("isReply"),
        !candidate.replyToObjectId.isEmpty());
    map.insert(
        QStringLiteral("parentObjectId"),
        candidate.replyToObjectId);
    map.insert(QStringLiteral("replyCount"), 0);
    map.insert(QStringLiteral("characterSpans"), characterSpanMaps(draft.characterSpans));
    map.insert(
        QStringLiteral("paragraphFormats"),
        paragraphFormatMaps(draft.paragraphFormats));
    map.insert(QStringLiteral("fontFamily"), format.fontFamily);
    map.insert(QStringLiteral("fontSizeRatio"), format.fontSizeRatio);
    map.insert(
        QStringLiteral("displayFontSizeRatio"),
        format.displayFontSizeRatio > 0.0
            ? format.displayFontSizeRatio
            : format.fontSizeRatio);
    map.insert(QStringLiteral("fontStretch"), format.fontStretch);
    map.insert(QStringLiteral("fontBold"), format.fontBold);
    map.insert(QStringLiteral("fontItalic"), format.fontItalic);
    map.insert(QStringLiteral("fontUnderline"), format.fontUnderline);
    map.insert(QStringLiteral("color"), format.color);
    map.insert(QStringLiteral("editable"), candidate.editable);
    map.insert(QStringLiteral("deletable"), candidateDeletable(candidate));
    map.insert(QStringLiteral("limited"), candidate.limited);
    map.insert(QStringLiteral("added"), candidate.added);
    map.insert(
        QStringLiteral("sourceTextMoveOnly"),
        isSourceTextMoveOnly(draft));
    map.insert(QStringLiteral("image"), candidate.image);
    map.insert(QStringLiteral("imagePixelWidth"), candidate.image.width());
    map.insert(QStringLiteral("imagePixelHeight"), candidate.image.height());
    map.insert(
        QStringLiteral("shapeType"),
        static_cast<int>(candidate.shapeType));
    map.insert(QStringLiteral("shapeStart"), candidate.shapeStartRatio);
    map.insert(QStringLiteral("shapeEnd"), candidate.shapeEndRatio);
    map.insert(QStringLiteral("shapeFillColor"), candidate.fillColor);
    map.insert(QStringLiteral("shapeStrokeColor"), candidate.strokeColor);
    map.insert(
        QStringLiteral("shapeStrokeWidthRatio"),
        candidate.shapeStrokeWidthRatio);
    const QVector<QRectF> displayedHighlightRects
        = highlightRectsForDraft(draft);
    QVariantList highlightRects;
    highlightRects.reserve(displayedHighlightRects.size());
    for (const QRectF &rect : displayedHighlightRects) {
        QVariantMap rectMap;
        rectMap.insert(QStringLiteral("x"), rect.x());
        rectMap.insert(QStringLiteral("y"), rect.y());
        rectMap.insert(QStringLiteral("width"), rect.width());
        rectMap.insert(QStringLiteral("height"), rect.height());
        highlightRects.append(rectMap);
    }
    map.insert(QStringLiteral("highlightRects"), highlightRects);
    map.insert(QStringLiteral("highlightColor"), candidate.highlightColor);
    map.insert(
        QStringLiteral("stampName"),
        candidate.commentProperties.name);
    map.insert(QStringLiteral("stampText"), candidate.stampText);
    map.insert(QStringLiteral("stampColor"), candidate.highlightColor);
    const QVector<QPointF> displayedDrawingVertices
        = drawingVerticesForDraft(draft);
    QVariantList drawingVertices;
    drawingVertices.reserve(displayedDrawingVertices.size());
    for (const QPointF &vertex : displayedDrawingVertices) {
        drawingVertices.append(vertex);
    }
    map.insert(QStringLiteral("drawingVertices"), drawingVertices);
    map.insert(QStringLiteral("annotationPoints"), drawingVertices);
    const QVector<QVector<QPointF>> displayedInkStrokes
        = inkStrokesForDraft(draft);
    QVariantList inkStrokes;
    inkStrokes.reserve(displayedInkStrokes.size());
    for (const QVector<QPointF> &stroke : displayedInkStrokes) {
        QVariantList strokePoints;
        strokePoints.reserve(stroke.size());
        for (const QPointF &point : stroke) {
            strokePoints.append(point);
        }
        inkStrokes.append(QVariant::fromValue(strokePoints));
    }
    map.insert(QStringLiteral("inkStrokes"), inkStrokes);
    map.insert(QStringLiteral("annotationStrokes"), inkStrokes);
    map.insert(
        QStringLiteral("drawingStrokeColor"),
        commentProperties.strokeColor);
    map.insert(
        QStringLiteral("strokeColor"),
        commentProperties.strokeColor);
    map.insert(
        QStringLiteral("drawingInteriorColor"),
        commentProperties.interiorColor);
    map.insert(
        QStringLiteral("fillColor"),
        commentProperties.interiorColor);
    map.insert(
        QStringLiteral("drawingHasInterior"),
        commentProperties.interiorColor.isValid());
    map.insert(
        QStringLiteral("drawingOpacity"),
        commentProperties.opacity);
    map.insert(
        QStringLiteral("opacity"),
        commentProperties.opacity);
    map.insert(
        QStringLiteral("drawingBorderWidth"),
        commentProperties.borderWidth);
    qreal strokeWidthRatio = 0.0;
    if (m_document
        && candidate.page >= 0
        && candidate.page < m_document->pageCount()) {
        const qreal pageHeight = m_document->pageSize(candidate.page).height();
        if (pageHeight > 0.0) {
            strokeWidthRatio = commentProperties.borderWidth / pageHeight;
        }
    }
    map.insert(QStringLiteral("strokeWidthRatio"), strokeWidthRatio);
    map.insert(
        QStringLiteral("drawingBorderStyle"),
        static_cast<int>(commentProperties.borderStyle));
    map.insert(
        QStringLiteral("borderStyle"),
        static_cast<int>(commentProperties.borderStyle));
    QVariantList dashPattern;
    dashPattern.reserve(commentProperties.dashPattern.size());
    for (qreal dash : commentProperties.dashPattern) {
        dashPattern.append(dash);
    }
    map.insert(QStringLiteral("drawingDashPattern"), dashPattern);
    map.insert(QStringLiteral("dashPattern"), dashPattern);
    map.insert(
        QStringLiteral("drawingBorderEffect"),
        static_cast<int>(commentProperties.borderEffect));
    map.insert(
        QStringLiteral("borderEffect"),
        static_cast<int>(commentProperties.borderEffect));
    map.insert(
        QStringLiteral("drawingStartLineEnding"),
        static_cast<int>(commentProperties.startLineEnding));
    map.insert(
        QStringLiteral("lineStartStyle"),
        static_cast<int>(commentProperties.startLineEnding));
    map.insert(
        QStringLiteral("drawingEndLineEnding"),
        static_cast<int>(commentProperties.endLineEnding));
    map.insert(
        QStringLiteral("lineEndStyle"),
        static_cast<int>(commentProperties.endLineEnding));
    map.insert(QStringLiteral("reason"), candidate.reason);
    return map;
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

void PDFEditController::setInitialGeometry(Candidate &candidate,
                                           const QRectF &sourceRect)
{
    candidate.geometry.sourceRect = sourceRect;
    candidate.initialDraftFrameRect = sourceRect;
}

void PDFEditController::expandInitialTextFrame(Candidate &candidate,
                                               const QSizeF &pageSize) const
{
    const QRectF &sourceRect = candidate.geometry.sourceRect;
    if (candidate.kind != QLatin1String("text")
        || !candidate.hasBaseline
        || candidate.text.isEmpty()
        || sourceRect.isEmpty()
        || pageSize.isEmpty()) {
        return;
    }

    const qreal displayFontSizeRatio = candidate.displayFontSizeRatio > 0.0
        ? candidate.displayFontSizeRatio
        : candidate.fontSizeRatio;
    const qreal fontPixelSize = displayFontSizeRatio * pageSize.height();
    if (fontPixelSize <= 0.0) {
        return;
    }

    const QFont displayFont = fontForDisplay(
        candidate.fontFamily,
        candidate.fontStretch,
        candidate.fontBold,
        candidate.fontItalic,
        candidate.fontUnderline,
        fontPixelSize);
    const QFontMetricsF metrics(displayFont);
    const qreal measuredWidth = qMax(
        metrics.horizontalAdvance(candidate.text),
        metrics.boundingRect(candidate.text).width());
    const qreal layoutLeft = qBound<qreal>(
        0.0,
        qMin(sourceRect.left(), candidate.origin.x()),
        1.0);
    const qreal measuredRight = candidate.origin.x()
        + (measuredWidth + kInitialTextFrameSafetyPoints) / pageSize.width();
    const qreal layoutRight = qBound<qreal>(
        layoutLeft,
        qMax(sourceRect.right(), measuredRight),
        1.0);
    candidate.initialDraftFrameRect = QRectF(
        layoutLeft,
        sourceRect.y(),
        layoutRight - layoutLeft,
        sourceRect.height());
}

PDFEditController::CharacterFormat PDFEditController::characterFormatForCandidate(
    const Candidate &candidate)
{
    CharacterFormat format;
    format.fontFamily = candidate.fontFamily;
    format.fontSizeRatio = candidate.fontSizeRatio;
    format.displayFontSizeRatio = candidate.displayFontSizeRatio;
    format.fontStretch = candidate.fontStretch;
    format.fontBold = candidate.fontBold;
    format.fontItalic = candidate.fontItalic;
    format.fontUnderline = candidate.fontUnderline;
    format.color = candidate.color;
    return format;
}

PDFEditController::CharacterFormat PDFEditController::primaryCharacterFormat(
    const Draft &draft)
{
    return draft.characterSpans.isEmpty()
        ? characterFormatForCandidate(draft.candidate)
        : draft.characterSpans.constFirst().format;
}

PDFEditController::CharacterFormat PDFEditController::characterFormatAtPosition(
    const Draft &draft,
    qsizetype position,
    bool preferPrevious)
{
    if (draft.characterSpans.isEmpty()) {
        return characterFormatForCandidate(draft.candidate);
    }

    const qsizetype textLength = draft.text.size();
    if (textLength <= 0) {
        return draft.characterSpans.constFirst().format;
    }

    const qsizetype boundedPosition = qBound<qsizetype>(0, position, textLength);
    const qsizetype characterIndex = preferPrevious && boundedPosition > 0
        ? boundedPosition - 1
        : qMin(boundedPosition, textLength - 1);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, textLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            textLength);
        if (characterIndex >= spanStart && characterIndex < spanEnd) {
            return span.format;
        }
    }

    return primaryCharacterFormat(draft);
}

PDFEditController::CharacterFormat
PDFEditController::selectedCharacterFormatForDisplay() const
{
    if (!m_hasSelection) {
        return CharacterFormat();
    }

    const Draft draft = selectedDraftForDisplay();
    if (!m_textEditActive) {
        return primaryCharacterFormat(draft);
    }

    const qsizetype textLength = draft.text.size();
    const qsizetype rangeStart = qBound<qsizetype>(
        0,
        m_selectedTextRangeStart,
        textLength);
    const qsizetype rangeLength = qBound<qsizetype>(
        0,
        m_selectedTextRangeLength,
        textLength - rangeStart);
    if (rangeLength == 0
        && m_typingCharacterFormatActive
        && m_typingCharacterFormatPosition == rangeStart) {
        return m_typingCharacterFormat;
    }

    return characterFormatAtPosition(draft, rangeStart, rangeLength == 0);
}

QVector<PDFEditController::ParagraphFormat> PDFEditController::paragraphFormatsForText(
    const QString &text,
    const ParagraphFormat &prototype)
{
    QVector<ParagraphFormat> formats;
    qsizetype start = 0;
    while (start <= text.size()) {
        const qsizetype newline = text.indexOf(QLatin1Char('\n'), start);
        const qsizetype end = newline < 0 ? text.size() : newline;
        ParagraphFormat format = prototype;
        format.start = start;
        format.length = end - start;
        formats.append(format);
        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
    return formats;
}

PDFEditController::Draft PDFEditController::initialDraftForCandidate(
    const Candidate &candidate)
{
    Draft draft;
    draft.candidate = candidate;
    draft.commentProperties = candidate.commentProperties;
    draft.text = candidate.text;
    draft.characterSpans.append(CharacterSpan{
        0,
        candidate.text.size(),
        characterFormatForCandidate(candidate)});
    draft.paragraphFormats = paragraphFormatsForText(candidate.text, ParagraphFormat());
    draft.frameRect = candidate.initialDraftFrameRect.isEmpty()
        ? candidate.geometry.sourceRect
        : candidate.initialDraftFrameRect;
    draft.contentBounds = draft.frameRect;
    draft.commentStatus = candidate.commentStatus;
    draft.commentMarked = candidate.commentMarked;
    draft.commentStateAuthor = candidate.commentStateAuthor.trimmed().isEmpty()
        ? currentCommentAuthor()
        : candidate.commentStateAuthor;
    return draft;
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

QPointF PDFEditController::layoutOriginForDraft(const Draft &draft)
{
    const Candidate &candidate = draft.candidate;
    if (!candidate.hasBaseline) {
        return candidate.origin;
    }

    const QRectF initialFrameRect = candidate.initialDraftFrameRect.isEmpty()
        ? candidate.geometry.sourceRect
        : candidate.initialDraftFrameRect;
    return candidate.origin + QPointF(
        draft.frameRect.x() - initialFrameRect.x(),
        draft.frameRect.y() - initialFrameRect.y());
}

bool PDFEditController::sameCharacterFormat(const CharacterFormat &left,
                                            const CharacterFormat &right)
{
    return sameFontFamily(left.fontFamily, right.fontFamily)
        && left.fontSizeRatio == right.fontSizeRatio
        && left.displayFontSizeRatio == right.displayFontSizeRatio
        && left.fontStretch == right.fontStretch
        && left.fontBold == right.fontBold
        && left.fontItalic == right.fontItalic
        && left.fontUnderline == right.fontUnderline
        && left.color == right.color;
}

bool PDFEditController::sameCharacterSpans(const QVector<CharacterSpan> &left,
                                           const QVector<CharacterSpan> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        const CharacterSpan &leftSpan = left.at(index);
        const CharacterSpan &rightSpan = right.at(index);
        if (leftSpan.start != rightSpan.start
            || leftSpan.length != rightSpan.length
            || !sameCharacterFormat(leftSpan.format, rightSpan.format)) {
            return false;
        }
    }
    return true;
}

bool PDFEditController::sameParagraphFormats(
    const QVector<ParagraphFormat> &left,
    const QVector<ParagraphFormat> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        const ParagraphFormat &leftFormat = left.at(index);
        const ParagraphFormat &rightFormat = right.at(index);
        if (leftFormat.start != rightFormat.start
            || leftFormat.length != rightFormat.length
            || leftFormat.alignment != rightFormat.alignment
            || leftFormat.listStyle != rightFormat.listStyle
            || leftFormat.listLevel != rightFormat.listLevel
            || leftFormat.listStart != rightFormat.listStart) {
            return false;
        }
    }
    return true;
}

bool PDFEditController::sameDraftContent(const Draft &left, const Draft &right)
{
    return left.text == right.text
        && left.commentProperties == right.commentProperties
        && left.commentStatus == right.commentStatus
        && left.commentMarked == right.commentMarked
        && sameCharacterSpans(left.characterSpans, right.characterSpans)
        && sameParagraphFormats(left.paragraphFormats, right.paragraphFormats)
        && left.frameRect == right.frameRect
        && left.contentBounds == right.contentBounds
        && left.deleted == right.deleted;
}

bool PDFEditController::isSourceTextMoveOnly(const Draft &draft)
{
    const Candidate &candidate = draft.candidate;
    if (candidate.kind != QLatin1String("text")
        || candidate.added
        || draft.deleted
        || !candidate.editable
        || !candidate.renderSuppressible) {
        return false;
    }

    const auto hasTopLevelSourcePath = [](const QVector<int> &objectPath) {
        return objectPath.size() == 1;
    };
    if (candidate.parts.isEmpty()) {
        if (!hasTopLevelSourcePath(candidate.objectPath)
            || candidate.geometry.sourceRect.isEmpty()) {
            return false;
        }
    } else {
        for (const CandidatePart &part : candidate.parts) {
            if (!hasTopLevelSourcePath(part.objectPath)
                || part.sourceRect.isEmpty()) {
                return false;
            }
        }
    }

    constexpr qreal geometryEpsilon = 1e-9;
    const auto nearlyEqual = [](qreal left, qreal right) {
        return qAbs(left - right) <= geometryEpsilon;
    };
    const QRectF initialFrameRect
        = (candidate.initialDraftFrameRect.isEmpty()
               ? candidate.geometry.sourceRect
               : candidate.initialDraftFrameRect)
              .normalized();
    if (initialFrameRect.isEmpty()
        || !nearlyEqual(draft.frameRect.width(), initialFrameRect.width())
        || !nearlyEqual(
            draft.frameRect.height(),
            initialFrameRect.height())) {
        return false;
    }

    const QPointF translation
        = draft.frameRect.topLeft() - initialFrameRect.topLeft();
    if (nearlyEqual(translation.x(), 0.0)
        && nearlyEqual(translation.y(), 0.0)) {
        return false;
    }

    const Draft initialDraft = initialDraftForCandidate(candidate);
    if (draft.text != initialDraft.text
        || !sameCharacterSpans(
            draft.characterSpans,
            initialDraft.characterSpans)
        || !sameParagraphFormats(
            draft.paragraphFormats,
            initialDraft.paragraphFormats)) {
        return false;
    }

    const QRectF expectedContentBounds
        = initialDraft.contentBounds.translated(translation);
    return nearlyEqual(
               draft.contentBounds.x(),
               expectedContentBounds.x())
        && nearlyEqual(
               draft.contentBounds.y(),
               expectedContentBounds.y())
        && nearlyEqual(
               draft.contentBounds.width(),
               expectedContentBounds.width())
        && nearlyEqual(
               draft.contentBounds.height(),
               expectedContentBounds.height());
}

PDFEditController::TextChange PDFEditController::setDraftText(
    Draft &draft,
    const QString &text,
    const CharacterFormat *typingFormat,
    qsizetype typingPosition,
    qsizetype cursorAfterEdit)
{
    if (draft.text == text) {
        return TextChange();
    }

    const qsizetype oldLength = draft.text.size();
    const qsizetype newLength = text.size();
    qsizetype commonPrefix = 0;
    while (commonPrefix < oldLength
           && commonPrefix < newLength
           && draft.text.at(commonPrefix) == text.at(commonPrefix)) {
        ++commonPrefix;
    }

    qsizetype commonSuffix = 0;
    while (commonSuffix < oldLength - commonPrefix
           && commonSuffix < newLength - commonPrefix
           && draft.text.at(oldLength - commonSuffix - 1)
               == text.at(newLength - commonSuffix - 1)) {
        ++commonSuffix;
    }

    if (typingFormat
        && typingPosition >= 0
        && cursorAfterEdit >= 0
        && cursorAfterEdit <= newLength) {
        const qsizetype lengthDelta = newLength - oldLength;
        if (lengthDelta > 0) {
            const qsizetype anchoredStart = cursorAfterEdit - lengthDelta;
            if (anchoredStart == typingPosition
                && anchoredStart >= 0
                && draft.text.first(anchoredStart) == text.first(anchoredStart)
                && draft.text.sliced(anchoredStart)
                    == text.sliced(cursorAfterEdit)) {
                commonPrefix = anchoredStart;
                commonSuffix = oldLength - anchoredStart;
            }
        } else if (lengthDelta < 0) {
            const qsizetype removedLength = -lengthDelta;
            const qsizetype anchoredStart = cursorAfterEdit;
            if ((anchoredStart == typingPosition
                 || anchoredStart + removedLength == typingPosition)
                && anchoredStart + removedLength <= oldLength
                && draft.text.first(anchoredStart) == text.first(anchoredStart)
                && draft.text.sliced(anchoredStart + removedLength)
                    == text.sliced(anchoredStart)) {
                commonPrefix = anchoredStart;
                commonSuffix = oldLength - anchoredStart - removedLength;
            }
        }
    }

    TextChange change;
    change.start = commonPrefix;
    change.removedLength = oldLength - commonPrefix - commonSuffix;
    change.insertedLength = newLength - commonPrefix - commonSuffix;

    const QVector<ParagraphFormat> oldParagraphFormats = draft.paragraphFormats;
    const CharacterFormat fallbackFormat = primaryCharacterFormat(draft);
    QVector<CharacterFormat> oldFormats(oldLength, fallbackFormat);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, oldLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            oldLength);
        for (qsizetype index = spanStart; index < spanEnd; ++index) {
            oldFormats[index] = span.format;
        }
    }

    CharacterFormat insertedFormat = fallbackFormat;
    if (commonPrefix > 0 && !oldFormats.isEmpty()) {
        insertedFormat = oldFormats.at(commonPrefix - 1);
    } else if (commonPrefix < oldFormats.size()) {
        insertedFormat = oldFormats.at(commonPrefix);
    }
    if (typingFormat
        && change.insertedLength > 0
        && (change.start == typingPosition
            || change.start + change.removedLength == typingPosition)) {
        insertedFormat = *typingFormat;
    }

    QVector<CharacterFormat> newFormats;
    newFormats.reserve(newLength);
    for (qsizetype index = 0; index < commonPrefix; ++index) {
        newFormats.append(oldFormats.at(index));
    }
    for (qsizetype index = 0; index < change.insertedLength; ++index) {
        newFormats.append(insertedFormat);
    }
    const qsizetype oldSuffixStart = oldLength - commonSuffix;
    for (qsizetype index = oldSuffixStart; index < oldLength; ++index) {
        newFormats.append(oldFormats.at(index));
    }

    QVector<CharacterSpan> newSpans;
    if (newFormats.isEmpty()) {
        newSpans.append(CharacterSpan{0, 0, insertedFormat});
    } else {
        qsizetype spanStart = 0;
        CharacterFormat spanFormat = newFormats.constFirst();
        for (qsizetype index = 1; index <= newFormats.size(); ++index) {
            if (index < newFormats.size()
                && sameCharacterFormat(spanFormat, newFormats.at(index))) {
                continue;
            }
            newSpans.append(CharacterSpan{spanStart, index - spanStart, spanFormat});
            if (index < newFormats.size()) {
                spanStart = index;
                spanFormat = newFormats.at(index);
            }
        }
    }

    QVector<ParagraphFormat> newParagraphFormats = paragraphFormatsForText(
        text,
        ParagraphFormat());
    const auto oldParagraphAt = [&oldParagraphFormats](qsizetype position) {
        for (const ParagraphFormat &format : oldParagraphFormats) {
            if (position >= format.start
                && position <= format.start + format.length) {
                return format;
            }
        }
        return oldParagraphFormats.isEmpty()
            ? ParagraphFormat()
            : oldParagraphFormats.constLast();
    };
    for (ParagraphFormat &format : newParagraphFormats) {
        qsizetype oldPosition = change.start;
        if (format.start < change.start) {
            oldPosition = format.start;
        } else if (format.start >= change.start + change.insertedLength) {
            oldPosition = format.start
                - change.insertedLength
                + change.removedLength;
        }
        ParagraphFormat inheritedFormat = oldParagraphAt(
            qBound<qsizetype>(0, oldPosition, oldLength));
        inheritedFormat.start = format.start;
        inheritedFormat.length = format.length;
        format = inheritedFormat;
    }
    draft.text = text;
    draft.characterSpans = std::move(newSpans);
    draft.paragraphFormats = std::move(newParagraphFormats);
    return change;
}

void PDFEditController::applyUnicodeFontFallback(Draft &draft)
{
    if (draft.text == draft.candidate.text || !containsNonAscii(draft.text)) {
        return;
    }

    const CharacterFormat originalFormat = characterFormatForCandidate(
        draft.candidate);
    const QString bundledArial = matchingBundledFamilyName(
        QStringLiteral("Arial"));
    for (CharacterSpan &span : draft.characterSpans) {
        const qsizetype start = qBound<qsizetype>(
            0,
            span.start,
            draft.text.size());
        const qsizetype length = qBound<qsizetype>(
            0,
            span.length,
            draft.text.size() - start);
        if (!containsNonAscii(draft.text.sliced(start, length))) {
            continue;
        }

        const QString bundledFamily = matchingBundledFamilyName(
            span.format.fontFamily);
        const QString systemFamily = matchingSystemFamilyName(
            span.format.fontFamily);
        if (!bundledFamily.isEmpty()) {
            span.format.fontFamily = bundledFamily;
        } else if (!systemFamily.isEmpty()
                   && !isGenericArialCompatibleFontFamily(
                       span.format.fontFamily)) {
            span.format.fontFamily = systemFamily;
        } else if (!bundledArial.isEmpty()
                   && equivalentFontFamilyName(
                       span.format.fontFamily,
                       originalFormat.fontFamily)
                   && isArialCompatibleFontFamily(span.format.fontFamily)) {
            span.format.fontFamily = bundledArial;
        } else if (!systemFamily.isEmpty()) {
            span.format.fontFamily = systemFamily;
        }
    }
}

bool PDFEditController::updateCharacterFormats(
    Draft &draft,
    qsizetype start,
    qsizetype length,
    const std::function<void(CharacterFormat &)> &update)
{
    const qsizetype textLength = draft.text.size();
    const CharacterFormat fallbackFormat = primaryCharacterFormat(draft);
    if (textLength == 0) {
        CharacterFormat updatedFormat = fallbackFormat;
        update(updatedFormat);
        if (sameCharacterFormat(fallbackFormat, updatedFormat)) {
            return false;
        }
        draft.characterSpans = {CharacterSpan{0, 0, updatedFormat}};
        return true;
    }

    const qsizetype rangeStart = qBound<qsizetype>(0, start, textLength);
    const qsizetype rangeEnd = qBound<qsizetype>(
        rangeStart,
        rangeStart + length,
        textLength);
    if (rangeStart == rangeEnd) {
        return false;
    }

    QVector<CharacterFormat> formats(textLength, fallbackFormat);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, textLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            textLength);
        for (qsizetype index = spanStart; index < spanEnd; ++index) {
            formats[index] = span.format;
        }
    }

    bool changed = false;
    for (qsizetype index = rangeStart; index < rangeEnd; ++index) {
        const CharacterFormat previousFormat = formats.at(index);
        update(formats[index]);
        changed = changed
            || !sameCharacterFormat(previousFormat, formats.at(index));
    }
    if (!changed) {
        return false;
    }

    QVector<CharacterSpan> spans;
    qsizetype spanStart = 0;
    CharacterFormat spanFormat = formats.constFirst();
    for (qsizetype index = 1; index <= formats.size(); ++index) {
        if (index < formats.size()
            && sameCharacterFormat(spanFormat, formats.at(index))) {
            continue;
        }
        spans.append(CharacterSpan{spanStart, index - spanStart, spanFormat});
        if (index < formats.size()) {
            spanStart = index;
            spanFormat = formats.at(index);
        }
    }
    draft.characterSpans = std::move(spans);
    return true;
}

QRectF PDFEditController::expandedTextEditFrame(const Draft &draft)
{
    const Candidate &selectedCandidate = draft.candidate;
    const QRectF frameRect = draft.frameRect.normalized();
    if (!m_document
        || selectedCandidate.kind != QLatin1String("text")
        || selectedCandidate.added
        || frameRect.isEmpty()) {
        return draft.frameRect;
    }

    const QSizeF pageSize = m_document->pageSize(selectedCandidate.page);
    if (pageSize.isEmpty()) {
        return draft.frameRect;
    }

    const qreal minimumPageMargin = qBound<qreal>(
        0.0,
        kTextEditMinimumPageMarginPoints / pageSize.width(),
        kTextEditMaximumPageMarginRatio);
    const qreal minimumExpansion = kTextEditMinimumExpansionPoints
        / pageSize.width();
    const qreal currentRight = qBound<qreal>(
        frameRect.left(),
        frameRect.right(),
        1.0);
    const QRectF selectedSourceRect
        = selectedCandidate.geometry.sourceRect.normalized();
    const QRectF initialFrameRect
        = (selectedCandidate.initialDraftFrameRect.isEmpty()
               ? selectedSourceRect
               : selectedCandidate.initialDraftFrameRect)
              .normalized();
    const qreal boundaryAnchorRight = qBound<qreal>(
        frameRect.left(),
        qMax(selectedSourceRect.right(), initialFrameRect.right()),
        1.0);
    qreal inferredLeftMargin = 1.0;
    qreal observedRight = currentRight;
    qreal nearestRowObstacle = 2.0;

    const auto considerCandidate = [&](const Candidate &candidate) {
        if (candidate.textMarkupHitTestOnly
            || (candidate.kind != QLatin1String("text")
             && candidate.kind != QLatin1String("addedText"))
            || candidate.geometry.sourceRect.isEmpty()) {
            return;
        }

        const QRectF sourceRect = candidate.geometry.sourceRect.normalized();
        QRectF effectiveRect = sourceRect;
        const auto candidateDraft = m_drafts.constFind(
            draftKey(candidate.page, candidate.objectId));
        if (candidateDraft != m_drafts.cend()) {
            if (candidateDraft->text.isEmpty()) {
                return;
            }
            effectiveRect = candidateDraft->frameRect.normalized();
        } else if (candidate.added && candidate.text.isEmpty()) {
            return;
        }

        inferredLeftMargin = qMin(inferredLeftMargin, effectiveRect.left());
        observedRight = qMax(observedRight, effectiveRect.right());
        if (candidate.page != selectedCandidate.page
            || candidate.objectId == selectedCandidate.objectId
            || effectiveRect.left()
                <= selectedSourceRect.left() + minimumExpansion) {
            return;
        }

        const qreal rowTolerance = kTextEditRowToleranceRatio
            * qMax(selectedSourceRect.height(), effectiveRect.height());
        const bool sharesVisualRow
            = effectiveRect.top() <= selectedSourceRect.bottom() + rowTolerance
            && effectiveRect.bottom() >= selectedSourceRect.top() - rowTolerance;
        if (sharesVisualRow) {
            nearestRowObstacle = qMin(
                nearestRowObstacle,
                effectiveRect.left());
        }
    };

    const QVector<Candidate> &pageCandidates = ensurePageCandidates(
        selectedCandidate.page);
    for (const Candidate &candidate : pageCandidates) {
        considerCandidate(candidate);
    }
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page == selectedCandidate.page) {
            considerCandidate(candidate);
        }
    }

    const qreal inferredMargin = inferredLeftMargin < 1.0
        ? qBound<qreal>(
              minimumPageMargin,
              inferredLeftMargin,
              kTextEditMaximumPageMarginRatio)
        : minimumPageMargin;
    qreal pageRight = qMax(1.0 - inferredMargin, observedRight);
    pageRight = qBound<qreal>(currentRight, pageRight, 1.0);

    qreal targetRight = pageRight;
    if (nearestRowObstacle <= 1.0) {
        const qreal fixedColumnRight = nearestRowObstacle <= boundaryAnchorRight
            ? boundaryAnchorRight
            : boundaryAnchorRight
                + (nearestRowObstacle - boundaryAnchorRight) / 2.0;
        targetRight = qMax(
            currentRight,
            qMin(pageRight, fixedColumnRight));
    }
    if (targetRight - currentRight < minimumExpansion) {
        return draft.frameRect;
    }

    return QRectF(
        frameRect.x(),
        frameRect.y(),
        targetRight - frameRect.x(),
        frameRect.height());
}

PDFEditController::Draft PDFEditController::selectedDraftForDisplay() const
{
    if (!m_hasSelection) {
        return Draft();
    }

    if (m_textEditActive
        && m_textEditDisplayDraftActive
        && m_textEditDisplayDraft.candidate.page == m_selectedCandidate.page
        && m_textEditDisplayDraft.candidate.objectId
            == m_selectedCandidate.objectId) {
        return m_textEditDisplayDraft;
    }

    const auto draft = m_drafts.constFind(
        draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId));
    return draft == m_drafts.cend()
        ? initialDraftForCandidate(m_selectedCandidate)
        : *draft;
}

void PDFEditController::selectedTextRangeForDraft(const Draft &draft,
                                                  qsizetype &start,
                                                  qsizetype &length) const
{
    const qsizetype textLength = draft.text.size();
    if (m_textEditActive && m_selectedTextRangeLength > 0) {
        start = qBound<qsizetype>(0, m_selectedTextRangeStart, textLength);
        length = qBound<qsizetype>(
            0,
            m_selectedTextRangeLength,
            textLength - start);
        return;
    }

    start = 0;
    length = textLength;
}

void PDFEditController::selectedParagraphRangeForDraft(
    const Draft &draft,
    qsizetype &first,
    qsizetype &last) const
{
    first = -1;
    last = -1;
    if (draft.paragraphFormats.isEmpty()) {
        return;
    }
    if (!m_textEditActive) {
        first = 0;
        last = draft.paragraphFormats.size() - 1;
        return;
    }

    const qsizetype textLength = draft.text.size();
    const qsizetype rangeStart = qBound<qsizetype>(
        0,
        m_selectedTextRangeStart,
        textLength);
    const qsizetype rangeLength = qBound<qsizetype>(
        0,
        m_selectedTextRangeLength,
        textLength - rangeStart);
    const qsizetype rangeEnd = rangeLength > 0
        ? rangeStart + rangeLength - 1
        : rangeStart;
    const auto paragraphAt = [&draft](qsizetype position) {
        for (qsizetype index = 0;
             index < draft.paragraphFormats.size();
             ++index) {
            const ParagraphFormat &format = draft.paragraphFormats.at(index);
            if (position >= format.start
                && position <= format.start + format.length) {
                return index;
            }
        }
        return draft.paragraphFormats.size() - 1;
    };

    first = paragraphAt(rangeStart);
    last = paragraphAt(rangeEnd);
    if (last < first) {
        std::swap(first, last);
    }
}

void PDFEditController::resetTextEditDisplayState()
{
    resetTypingFormat();
    m_textEditDisplayDraft = Draft();
    m_textEditDisplayDraftActive = false;
    m_textEditFrameAutomaticallyExpanded = false;
    m_textEditGeometryExplicitlyChanged = false;
    m_textEditInitialDisplayFrameRect = QRectF();
}

void PDFEditController::resetTypingFormat()
{
    m_typingCharacterFormat = CharacterFormat();
    m_typingCharacterFormatPosition = -1;
    m_typingCharacterFormatActive = false;
}

void PDFEditController::rebaseTypingFormat(const Draft &draft)
{
    if (!m_typingCharacterFormatActive) {
        return;
    }

    const bool fontBold = m_typingCharacterFormat.fontBold;
    const bool fontItalic = m_typingCharacterFormat.fontItalic;
    const bool fontUnderline = m_typingCharacterFormat.fontUnderline;
    m_typingCharacterFormat = characterFormatAtPosition(
        draft,
        m_typingCharacterFormatPosition,
        true);
    m_typingCharacterFormat.fontBold = fontBold;
    m_typingCharacterFormat.fontItalic = fontItalic;
    m_typingCharacterFormat.fontUnderline = fontUnderline;
}

void PDFEditController::resetFrameResizeState()
{
    m_frameResizeActive = false;
    m_frameResizeOwnsTextEdit = false;
    m_frameResizeStartGeometryExplicitlyChanged = false;
    m_frameResizeStartFrameRect = QRectF();
    m_frameResizeStartContentBounds = QRectF();
}

void PDFEditController::updateSelectedDraft(const Draft &draft)
{
    if (!m_hasSelection
        || draft.candidate.page != m_selectedCandidate.page
        || draft.candidate.objectId != m_selectedCandidate.objectId) {
        return;
    }

    Draft normalizedDraft = draft;
    const QDateTime commentModifiedDate
        = normalizedDraft.candidate.commentModifiedDate;
    const QString commentThreadId
        = normalizedDraft.candidate.commentThreadId;
    normalizedDraft.candidate = m_selectedCandidate;
    if (commentModifiedDate.isValid()) {
        normalizedDraft.candidate.commentModifiedDate
            = commentModifiedDate;
    }
    if (!commentThreadId.isEmpty()) {
        normalizedDraft.candidate.commentThreadId = commentThreadId;
    }
    if (m_textEditActive && m_textEditDisplayDraftActive) {
        m_textEditDisplayDraft = normalizedDraft;
    }
    const Draft originalDraft = initialDraftForCandidate(m_selectedCandidate);
    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool matchesOriginal
        = m_selectedCandidate.kind != QLatin1String("addedImage")
        && !isShapeCandidateKind(m_selectedCandidate.kind)
        && !(m_selectedCandidate.added
             && m_selectedCandidate.kind == QLatin1String("freeText"))
        && sameDraftContent(normalizedDraft, originalDraft);
    bool changed = false;
    if (matchesOriginal) {
        changed = m_drafts.remove(key) > 0;
    } else if (existingDraft == m_drafts.cend()
               || !sameDraftContent(*existingDraft, normalizedDraft)
               || existingDraft->candidate.commentModifiedDate
                   != normalizedDraft.candidate.commentModifiedDate) {
        m_drafts.insert(key, normalizedDraft);
        changed = true;
    }

    m_selectedText = normalizedDraft.text;
    if (changed) {
        const bool reusesSourceCommentSuppressionTexture
            = m_frameResizeActive
            && isGeometryCommentCandidateKind(
                normalizedDraft.candidate.kind)
            && !normalizedDraft.candidate.added;
        if (reusesSourceCommentSuppressionTexture) {
            emit pageDataChanged(m_selectedCandidate.page);
        } else {
            const bool notifyComments = !m_frameResizeActive
                || !isCommentCandidateKind(
                    normalizedDraft.candidate.kind);
            markDraftStateChanged(
                m_selectedCandidate.page,
                false,
                notifyComments);
        }
        emit historyStateChanged();
    }
    emit selectionChanged();
}

void PDFEditController::markDraftStateChanged(int page,
                                              bool immediate,
                                              bool notifyComments)
{
    const quint64 revision = ++m_nextDraftRevision;
    m_pageDraftRevisions.insert(page, revision);
    emit draftRenderStateChanged(page, revision, immediate);
    emit pageDataChanged(page);
    if (notifyComments) {
        emit commentsChanged();
    }
}

void PDFEditController::requestDraftRender(int page, bool immediate)
{
    emit draftRenderStateChanged(page, draftRevisionForPage(page), immediate);
}

void PDFEditController::applyDraftState(const Command &command,
                                        bool useAfterState,
                                        bool immediate)
{
    if (command.kind == Command::Kind::PageStructure) {
        applyPageState(
            useAfterState
                ? command.pageStateAfter
                : command.pageStateBefore);
        return;
    }

    const bool shouldExist = useAfterState ? command.hadAfter : command.hadBefore;
    Draft draft = useAfterState ? command.after : command.before;
    Candidate candidate = draft.candidate;
    if (candidate.objectId.isEmpty()) {
        const Candidate *currentCandidate = findCandidate(command.page, command.objectId);
        if (currentCandidate) {
            candidate = *currentCandidate;
            draft.candidate = candidate;
        }
    }
    if (candidate.objectId.isEmpty()) {
        return;
    }

    bool candidateChanged = false;
    if (candidate.added && shouldExist && !findCandidate(command.page, command.objectId)) {
        const auto insertionPoint = std::lower_bound(
            m_addedCandidates.begin(),
            m_addedCandidates.end(),
            candidate.addedOrder,
            [](const Candidate &existing, quint64 addedOrder) {
                return existing.addedOrder < addedOrder;
            });
        m_addedCandidates.insert(insertionPoint, candidate);
        candidateChanged = true;
    }

    const auto existingDraft = m_drafts.constFind(command.key);
    const bool draftChanged = shouldExist
        ? existingDraft == m_drafts.cend()
            || !sameDraftContent(*existingDraft, draft)
        : existingDraft != m_drafts.cend();

    if (shouldExist && draftChanged) {
        m_drafts.insert(command.key, draft);
    } else if (draftChanged) {
        m_drafts.remove(command.key);
    }

    if (candidate.added && !shouldExist) {
        candidateChanged = removeAddedCandidate(command.page, command.objectId)
            || candidateChanged;
    }

    const bool selectedCandidateChanged = m_hasSelection
        && m_selectedCandidate.page == command.page
        && m_selectedCandidate.objectId == command.objectId;
    const bool selectionWasRemoved = candidate.added && !shouldExist;
    const bool selectionWasDeleted = shouldExist
        && draftRepresentsDeletion(candidate.kind, draft.text, draft.deleted);
    const bool selectedReplyWasHidden = m_hasSelection
        && isCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.replyToObjectId.isEmpty()
        && !commentReplyIsVisible(m_selectedCandidate);
    if ((selectedCandidateChanged
         && (selectionWasRemoved || selectionWasDeleted))
        || selectedReplyWasHidden) {
        setSelection(nullptr);
    } else {
        updateSelectedTextFromDraft();
    }

    const bool changed = candidateChanged || draftChanged;
    if (changed) {
        markDraftStateChanged(command.page, immediate);
    } else {
        requestDraftRender(command.page, immediate);
    }
}

PDFEditController::PageState PDFEditController::capturePageState() const
{
    PageState state;
    state.deletedSourcePages = m_deletedSourcePages;
    state.pageInsertions = m_pageInsertions;
    state.addedCandidates = m_addedCandidates;
    state.drafts = m_drafts;
    state.currentPage = m_document ? m_document->currentPage() : 0;
    state.hasSelection = m_hasSelection;
    state.selectedCandidate = m_selectedCandidate;
    return state;
}

void PDFEditController::applyPageState(const PageState &state)
{
    if (!m_document || !m_document->isLoaded()) {
        return;
    }

    const int deferredSourcePage
        = m_document->sourcePageIndex(m_deferredCandidateScanPage);
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_deferredCandidateScanPage = -1;
    m_pageCandidates.clear();
    m_pageCandidateLru.clear();
    resetCommentIndex();
    m_pageDraftRevisions.clear();

    m_deletedSourcePages = state.deletedSourcePages;
    m_pageInsertions = state.pageInsertions;
    m_addedCandidates = state.addedCandidates;
    m_drafts = state.drafts;
    m_document->setTransientPageStructure(
        m_deletedSourcePages,
        m_pageInsertions,
        state.currentPage);
    if (deferredSourcePage >= 0
        && m_deferredCandidateScanPage < 0) {
        m_deferredCandidateScanPage
            = m_document->displayPageIndexForSourcePage(deferredSourcePage);
    }

    QVector<PendingSquigglyCreation> remappedPendingCreations;
    remappedPendingCreations.reserve(m_pendingSquigglyCreations.size());
    for (PendingSquigglyCreation creation
         : std::as_const(m_pendingSquigglyCreations)) {
        const int remappedPage
            = m_document->displayPageIndexForSourcePage(creation.sourcePage);
        if (remappedPage < 0) {
            // Deleting the source page cancels its unfinished annotation.
            continue;
        }

        const QSizeF remappedPageSize = m_document->pageSize(remappedPage);
        if (remappedPageSize.isEmpty()) {
            continue;
        }
        creation.page = remappedPage;
        creation.pageSize = remappedPageSize;
        if (!pendingSquigglyCreationIsCurrent(creation)) {
            continue;
        }
        remappedPendingCreations.append(std::move(creation));
    }
    m_pendingSquigglyCreations = std::move(remappedPendingCreations);

    if (state.hasSelection) {
        const Candidate selectedCandidate = state.selectedCandidate;
        setSelection(&selectedCandidate);
    } else {
        setSelection(nullptr);
    }
    emit pageDataChanged(-1);
    emit commentsChanged();
    startPendingCandidateScan();
}

bool PDFEditController::removeAddedCandidate(int page, const QString &objectId)
{
    for (qsizetype index = 0; index < m_addedCandidates.size(); ++index) {
        const Candidate &candidate = m_addedCandidates.at(index);
        if (candidate.page == page && candidate.objectId == objectId) {
            m_addedCandidates.removeAt(index);
            return true;
        }
    }
    return false;
}

void PDFEditController::updateSelectedTextFromDraft()
{
    if (!m_hasSelection) {
        return;
    }

    const auto draft = m_drafts.constFind(
        draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId));
    m_selectedText = draft == m_drafts.cend() ? m_selectedCandidate.text : draft->text;
    emit selectionChanged();
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
