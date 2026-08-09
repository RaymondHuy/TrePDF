#include "PDFEditController.h"
#include "ApplicationFonts.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
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
#include <QUuid>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

namespace {

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
constexpr int kHighlightRed = 255;
constexpr int kHighlightGreen = 235;
constexpr int kHighlightBlue = 59;
constexpr int kHighlightAlpha = 96;
constexpr qint64 kMaximumAddedImageFileBytes = 64 * 1024 * 1024;
constexpr qint64 kMaximumAddedImagePixels = 40 * 1000 * 1000;
constexpr qreal kLineBaselineToleranceEm = 0.25;
constexpr qreal kLineMinimumHorizontalGapEm = -0.35;
constexpr qreal kLineMaximumHorizontalGapEm = 1.25;
constexpr qreal kLineWordSeparatorGapEm = 0.20;
constexpr qreal kLineFontSizeToleranceRatio = 0.08;
constexpr qreal kLineFontSizeTolerancePoints = 0.5;
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
        || tool == PDFEditController::StrikeoutTool;
}

bool isTextMarkupCandidateKind(const QString &kind)
{
    return kind == QLatin1String("highlight")
        || kind == QLatin1String("underline")
        || kind == QLatin1String("strikeout");
}

bool isCommentCandidateKind(const QString &kind)
{
    return isTextMarkupCandidateKind(kind)
        || kind == QLatin1String("stickyNote");
}

int commentAnnotationSubtype(const QString &kind)
{
    if (kind == QLatin1String("underline")) {
        return FPDF_ANNOT_UNDERLINE;
    }
    if (kind == QLatin1String("strikeout")) {
        return FPDF_ANNOT_STRIKEOUT;
    }
    if (kind == QLatin1String("stickyNote")) {
        return FPDF_ANNOT_TEXT;
    }
    return kind == QLatin1String("highlight")
        ? FPDF_ANNOT_HIGHLIGHT
        : FPDF_ANNOT_UNKNOWN;
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

QString textObjectText(FPDF_PAGEOBJECT object, FPDF_TEXTPAGE textPage)
{
    if (!object || !textPage) {
        return {};
    }

    const unsigned long byteLength = FPDFTextObj_GetText(object, textPage, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
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

QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key)
{
    if (!annotation) {
        return {};
    }

    const unsigned long byteLength = FPDFAnnot_GetStringValue(annotation, key, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    const qsizetype unitCount = static_cast<qsizetype>(
        (byteLength + sizeof(FPDF_WCHAR) - 1) / sizeof(FPDF_WCHAR));
    QVector<FPDF_WCHAR> buffer(unitCount);
    const unsigned long actualLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
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
{
    connect(
        this,
        &PDFEditController::selectionChanged,
        this,
        &PDFEditController::textFormatStateChanged);
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

void PDFEditController::setTool(Tool tool)
{
    if (m_tool == tool) {
        return;
    }

    finishSelectedTextEdit();
    m_tool = tool;
    if (m_tool == BrowseTool
        || m_tool == StickyNoteTool
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

bool PDFEditController::hasDrafts() const
{
    return !m_drafts.isEmpty()
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

QString PDFEditController::selectedReason() const
{
    return m_hasSelection ? m_selectedCandidate.reason : QString();
}

QVariantList PDFEditController::commentItems() const
{
    QVector<const Candidate *> comments;
    for (auto page = m_indexedCommentCandidates.cbegin();
         page != m_indexedCommentCandidates.cend();
         ++page) {
        for (const Candidate &candidate : page.value()) {
            if (isCommentCandidateKind(candidate.kind)) {
                comments.append(&candidate);
            }
        }
    }
    for (const Candidate &candidate : m_addedCandidates) {
        if (isCommentCandidateKind(candidate.kind)) {
            comments.append(&candidate);
        }
    }

    std::stable_sort(
        comments.begin(),
        comments.end(),
        [](const Candidate *left, const Candidate *right) {
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
        });

    QVariantList result;
    result.reserve(comments.size());
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

        const Draft displayDraft = draft == m_drafts.cend()
            ? initialDraftForCandidate(*candidate)
            : *draft;
        QVariantMap map = candidateMap(displayDraft);
        map.insert(QStringLiteral("page"), candidate->page);
        const QColor markerColor = candidate->highlightColor.isValid()
            ? candidate->highlightColor
            : candidate->kind == QLatin1String("stickyNote")
                ? QColor(kHighlightRed, kHighlightGreen, kHighlightBlue)
                : QColor(kHighlightRed,
                         kHighlightGreen,
                         kHighlightBlue,
                         kHighlightAlpha);
        map.insert(QStringLiteral("color"), markerColor);
        result.append(map);
    }
    return result;
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
        if (candidate.page != page
            || candidate.added
            || !draft->deleted
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
        if (isDesignerManagedSourceWidget(candidate)) {
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

QVariantList PDFEditController::draftsForPage(int page) const
{
    QVariantList result;
    const auto pageCandidates = m_pageCandidates.constFind(page);
    if (pageCandidates != m_pageCandidates.cend()) {
        for (const Candidate &candidate : pageCandidates.value()) {
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
    }

    for (const Candidate &candidate : m_addedCandidates) {
        if (candidate.page != page) {
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
        if (isDesignerManagedSourceWidget(candidate)) {
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
        //% "This PDF does not permit adding or deleting annotations."
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
    if (normalizedRect.width() < kMinimumHighlightDimensionRatio
        || normalizedRect.height() < kMinimumHighlightDimensionRatio) {
        return {};
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    switch (m_tool) {
    case UnderlineTool:
        candidate.kind = QStringLiteral("underline");
        //% "Underline"
        candidate.label = qtTrId("Underline");
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
        + QString::number(candidate.addedOrder);
    candidate.editable = true;
    candidate.added = true;
    candidate.highlightRects.append(normalizedRect);
    candidate.highlightColor = m_highlightColor;
    setInitialGeometry(candidate, normalizedRect);

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

    setSelection(nullptr);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
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
        //% "This PDF does not permit adding or deleting annotations."
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
    candidate.kind = QStringLiteral("stickyNote");
    //% "Sticky Note"
    candidate.label = qtTrId("StickyNote");
    candidate.editable = true;
    candidate.added = true;
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

    setTool(SelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
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
    if (!candidate) {
        clearSelection();
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
        emit statusMessage(candidate->reason);
    }
    return true;
}

void PDFEditController::requestCommentIndex()
{
    ensureCacheRevision();
    if (!m_document
        || !m_document->isLoaded()
        || m_commentIndexInFlight
        || m_commentIndexComplete) {
        return;
    }

    const QString filePath = m_document->renderFilePath();
    const int revision = m_document->renderRevision();
    if (filePath.isEmpty()) {
        return;
    }

    QVector<CandidateScanRequest> requests;
    requests.reserve(m_document->pageCount());
    for (int page = 0; page < m_document->pageCount(); ++page) {
        CandidateScanRequest request;
        request.filePath = filePath;
        request.revision = revision;
        request.page = page;
        request.sourcePage = m_document->sourcePageIndex(page);
        request.pageSize = m_document->pageSize(page);
        request.editingAllowed = documentAllowsEditing();
        if (request.sourcePage >= 0 && !request.pageSize.isEmpty()) {
            requests.append(std::move(request));
        }
    }

    m_commentIndexInFlight = !requests.isEmpty();
    m_commentIndexComplete = requests.isEmpty();
    m_commentIndexCancellation
        = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancellation
        = m_commentIndexCancellation;

    for (auto page = m_pageCandidates.cbegin();
         page != m_pageCandidates.cend();
         ++page) {
        cacheCommentCandidates(page.key(), page.value());
    }
    if (!m_indexedCommentCandidates.isEmpty()) {
        emit commentsChanged();
    }
    if (requests.isEmpty()) {
        m_commentIndexCancellation.reset();
        return;
    }

    const QPointer<PDFEditController> controller(this);
    commentIndexThreadPool().start(QRunnable::create(
        [controller,
         requests = std::move(requests),
         filePath,
         revision,
         cancellation]() mutable {
            CandidateScanDocument commentDocument;
            for (const CandidateScanRequest &request : requests) {
                if (candidateScanCancelled(cancellation)) {
                    break;
                }
                QVector<Candidate> comments;
                {
                    PDFiumProcessLock pdfiumLock;
                    FPDF_DOCUMENT document = commentDocument.documentLocked(
                        filePath,
                        revision);
                    comments = PDFEditController::scanCommentPageLocked(
                        document,
                        request,
                        cancellation);
                }
                if (candidateScanCancelled(cancellation)) {
                    break;
                }
                if (comments.isEmpty()) {
                    continue;
                }
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
                        controller->cacheCommentCandidates(
                            request.page,
                            comments);
                        emit controller->commentsChanged();
                    },
                    Qt::QueuedConnection);
            }

            QObject *app = QCoreApplication::instance();
            if (!app) {
                return;
            }
            QMetaObject::invokeMethod(
                app,
                [controller, filePath, revision, cancellation]() {
                    if (!controller
                        || controller->m_commentIndexCancellation
                            != cancellation) {
                        return;
                    }
                    controller->m_commentIndexInFlight = false;
                    controller->m_commentIndexCancellation.reset();
                    controller->m_commentIndexComplete
                        = !candidateScanCancelled(cancellation)
                        && controller->m_document
                        && controller->m_document->isLoaded()
                        && controller->m_document->renderRevision() == revision
                        && controller->m_document->renderFilePath()
                            == filePath;
                },
                Qt::QueuedConnection);
        }));
}

bool PDFEditController::selectComment(int page, const QString &objectId)
{
    const Candidate *candidate = findCandidate(page, objectId);
    if (candidate && isCommentCandidateKind(candidate->kind)) {
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
        setSelection(&indexedCandidate);
        if (!indexedCandidate.reason.isEmpty()) {
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
    if (pageCandidate && isCommentCandidateKind(pageCandidate->kind)) {
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
                    && isCommentCandidateKind(current.kind);
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

    const bool ownsTextEdit = m_frameResizeOwnsTextEdit;
    resetFrameResizeState();
    if (ownsTextEdit) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::cancelSelectedFrameResize()
{
    if (!m_frameResizeActive) {
        return;
    }

    const bool ownsTextEdit = m_frameResizeOwnsTextEdit;
    const QRectF startFrameRect = m_frameResizeStartFrameRect;
    const QRectF startContentBounds = m_frameResizeStartContentBounds;
    const bool startGeometryExplicitlyChanged
        = m_frameResizeStartGeometryExplicitlyChanged;
    resetFrameResizeState();

    if (ownsTextEdit) {
        cancelSelectedTextEdit();
        return;
    }

    Draft draft = selectedDraftForDisplay();
    draft.frameRect = startFrameRect;
    draft.contentBounds = startContentBounds;
    updateSelectedDraft(draft);
    m_textEditGeometryExplicitlyChanged = startGeometryExplicitlyChanged;
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
    if (!sameDraftContent(finalizedDraft, baseline.before)) {
        applyUnicodeFontFallback(finalizedDraft);
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

void PDFEditController::setSelectedCommentText(const QString &text)
{
    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || m_selectedCandidate.kind != QLatin1String("stickyNote")) {
        return;
    }

    const QString key = draftKey(
        m_selectedCandidate.page,
        m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    if (existingDraft == m_drafts.cend() || existingDraft->text == text) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = true;
    command.before = *existingDraft;
    command.hadAfter = true;
    command.after = command.before;
    command.after.text = text;
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
}

bool PDFEditController::removeCommentCandidate(const Candidate &candidate)
{
    if (!isCommentCandidateKind(candidate.kind)
        || candidate.page < 0
        || candidate.objectId.isEmpty()) {
        return false;
    }
    if (!candidate.added) {
        if (candidate.sourceAnnotationIndex < 0) {
            return false;
        }
        if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit adding or deleting annotations."
            emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
            return false;
        }
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
    if (!m_hasSelection) {
        return;
    }
    if (isCommentCandidateKind(m_selectedCandidate.kind)) {
        removeCommentCandidate(m_selectedCandidate);
        return;
    }
    if (!m_selectedCandidate.editable) {
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
    const bool discardedPageStructure
        = !m_pageInsertions.isEmpty() || !m_deletedSourcePages.isEmpty();

    const bool wasEditing = m_textEditActive;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    m_addedCandidates.clear();
    m_pageInsertions.clear();
    m_deletedSourcePages.clear();
    if (m_document) {
        m_document->setTransientPageStructure(
            m_deletedSourcePages,
            m_pageInsertions);
    }
    if (discardedPageStructure) {
        if (m_candidateScanCancellation) {
            m_candidateScanCancellation->store(
                true,
                std::memory_order_relaxed);
        }
        m_pendingCandidateScanPage = -1;
        m_pageCandidates.clear();
        m_pageCandidateLru.clear();
        resetCommentIndex();
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
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_pendingCandidateScanPage = -1;
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

    m_cachedRevision = revision;
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_pendingCandidateScanPage = -1;
    m_pageCandidates.clear();
    m_pageCandidateLru.clear();
    resetCommentIndex();
    emit commentsChanged();
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
        || page >= m_document->pageCount()
        || m_pageCandidates.contains(page)) {
        return;
    }

    if (m_document->isTransientBlankPage(page)) {
        m_pageCandidates.insert(page, {});
        touchPageCandidateCache(page);
        prunePageCandidateCache(page);
        emit pageCandidatesReady(page);
        return;
    }
    if (m_document->pageSize(page).isEmpty()) {
        return;
    }

    if (m_candidateScanInFlight) {
        if (m_candidateScanInFlightPage != page
            || candidateScanCancelled(m_candidateScanCancellation)) {
            // Keep exactly one running request plus the latest requested page.
            m_pendingCandidateScanPage = page;
            if (m_candidateScanCancellation) {
                m_candidateScanCancellation->store(
                    true,
                    std::memory_order_relaxed);
            }
        }
        return;
    }

    m_pendingCandidateScanPage = page;
    startPendingCandidateScan();
}

void PDFEditController::startPendingCandidateScan()
{
    if (m_candidateScanInFlight || m_pendingCandidateScanPage < 0) {
        return;
    }

    const int page = m_pendingCandidateScanPage;
    m_pendingCandidateScanPage = -1;
    if (m_pageCandidates.contains(page)) {
        return;
    }

    const CandidateScanRequest request = candidateScanRequest(page);
    if (request.page < 0) {
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
        cacheCommentCandidates(request.page, finalizedCandidates);
        m_pageCandidates.insert(
            request.page,
            std::move(finalizedCandidates));
        touchPageCandidateCache(request.page);
        prunePageCandidateCache(request.page);
        emit pageCandidatesReady(request.page);
        emit commentsChanged();
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
    if (page == m_pendingCandidateScanPage
        || (m_hasSelection
            && !m_selectedCandidate.added
            && m_selectedCandidate.page == page)) {
        return true;
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
    m_commentIndexInFlight = false;
    m_commentIndexComplete = false;
    m_indexedCommentCandidates.clear();
}

void PDFEditController::cacheCommentCandidates(
    int page,
    const QVector<Candidate> &candidates)
{
    QVector<Candidate> comments;
    for (const Candidate &candidate : candidates) {
        if (isCommentCandidateKind(candidate.kind)) {
            comments.append(candidate);
        }
    }

    if (comments.isEmpty()) {
        m_indexedCommentCandidates.remove(page);
    } else {
        m_indexedCommentCandidates.insert(page, std::move(comments));
    }
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

QVector<PDFEditController::Candidate>
PDFEditController::scanCommentPageLocked(
    FPDF_DOCUMENT document,
    const CandidateScanRequest &request,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    QVector<Candidate> candidates;
    if (!document
        || candidateScanCancelled(cancellation)
        || request.sourcePage < 0
        || request.pageSize.isEmpty()) {
        return candidates;
    }

    FPDF_PAGE page = FPDF_LoadPage(document, request.sourcePage);
    if (!page) {
        return candidates;
    }
    appendAnnotationCandidates(page, request, candidates, cancellation);
    FPDF_ClosePage(page);

    candidates.erase(
        std::remove_if(
            candidates.begin(),
            candidates.end(),
            [](const Candidate &candidate) {
                return !isCommentCandidateKind(candidate.kind);
            }),
        candidates.end());
    return candidates;
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
        appendAnnotationCandidates(page, request, candidates, cancellation);
    }

    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    FPDF_ClosePage(page);

    return candidates;
}

QVector<PDFEditController::Candidate> PDFEditController::finalizePageCandidates(
    QVector<Candidate> candidates,
    int page,
    const QSizeF &pageSize)
{
    assertPDFiumProcessLockNotHeld();
    QHash<int, QString> resolvedEmbeddedFamilies;
    for (Candidate &candidate : candidates) {
        if (candidate.kind == QLatin1String("text")) {
            resolveCandidateFontFamily(candidate, resolvedEmbeddedFamilies);
            expandInitialTextFrame(candidate, pageSize);
        }
        candidate.page = page;
    }
    candidates = groupTextLineCandidates(std::move(candidates), pageSize);
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
    if (type == FPDF_PAGEOBJ_TEXT
        && (renderMode == FPDF_TEXTRENDERMODE_UNKNOWN
            || renderMode == FPDF_TEXTRENDERMODE_INVISIBLE
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
    setInitialGeometry(candidate, rect);
    if (textLayout != textObjectLayouts.cend() && textLayout->hasOrigin) {
        candidate.hasBaseline = normalizedPagePoint(
            page,
            pageSize,
            textLayout->origin,
            candidate.origin);
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

void PDFEditController::appendAnnotationCandidates(FPDF_PAGE page,
                                                   const CandidateScanRequest &request,
                                                   QVector<Candidate> &candidates,
                                                   const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    const QSizeF &pageSize = request.pageSize;
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    for (int index = 0;
         index < annotationCount && !candidateScanCancelled(cancellation);
         ++index) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
        if (!annotation) {
            continue;
        }

        const int subtype = FPDFAnnot_GetSubtype(annotation);
        const bool textMarkupSubtype
            = subtype == FPDF_ANNOT_HIGHLIGHT
            || subtype == FPDF_ANNOT_UNDERLINE
            || subtype == FPDF_ANNOT_STRIKEOUT;
        const bool stickyNoteSubtype = subtype == FPDF_ANNOT_TEXT;
        if (textMarkupSubtype || stickyNoteSubtype) {
            QVector<QRectF> annotationRects;
            if (textMarkupSubtype) {
                const size_t quadCount
                    = FPDFAnnot_CountAttachmentPoints(annotation);
                annotationRects.reserve(static_cast<qsizetype>(quadCount));
                for (size_t quadIndex = 0;
                     quadIndex < quadCount;
                     ++quadIndex) {
                    FS_QUADPOINTSF quad{};
                    if (!FPDFAnnot_GetAttachmentPoints(
                            annotation,
                            quadIndex,
                            &quad)) {
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
            if (annotationRects.isEmpty()) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            QRectF geometry = annotationRects.constFirst();
            for (qsizetype rectIndex = 1;
                 rectIndex < annotationRects.size();
                 ++rectIndex) {
                geometry = geometry.united(annotationRects.at(rectIndex));
            }

            Candidate candidate;
            candidate.page = request.page;
            candidate.objectId = QStringLiteral("annotation:")
                + QString::number(index);
            candidate.sourceAnnotationIndex = index;
            candidate.text = annotationString(annotation, "Contents");
            candidate.editable = false;
            candidate.limited = true;
            setInitialGeometry(candidate, geometry);
            if (textMarkupSubtype) {
                candidate.highlightRects = annotationRects;
            }

            QColor defaultColor = stickyNoteSubtype
                ? QColor(kHighlightRed, kHighlightGreen, kHighlightBlue)
                : subtype == FPDF_ANNOT_HIGHLIGHT
                    ? QColor(kHighlightRed,
                             kHighlightGreen,
                             kHighlightBlue,
                             kHighlightAlpha)
                    : QColor(Qt::black);
            unsigned int red = 0;
            unsigned int green = 0;
            unsigned int blue = 0;
            unsigned int alpha = 0;
            if (FPDFAnnot_GetColor(
                    annotation,
                    FPDFANNOT_COLORTYPE_Color,
                    &red,
                    &green,
                    &blue,
                    &alpha)) {
                defaultColor = QColor::fromRgb(red, green, blue, alpha);
            }
            candidate.highlightColor = defaultColor;

            switch (subtype) {
            case FPDF_ANNOT_UNDERLINE:
                candidate.kind = QStringLiteral("underline");
                //% "Underline"
                candidate.label = qtTrId("Underline");
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
            default:
                candidate.kind = QStringLiteral("highlight");
                //% "Highlight"
                candidate.label = qtTrId("Highlight");
                break;
            }
            //% "Editing the contents of comments that already exist in the PDF is not available yet."
            candidate.reason = qtTrId("ExistingCommentEditingUnavailable");
            candidates.append(std::move(candidate));
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_WIDGET) {
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

        if (subtype == FPDF_ANNOT_FREETEXT) {
            candidate.kind = QStringLiteral("freeText");
            //% "Free text annotation"
            candidate.label = qtTrId("FreeTextAnnotation");
            candidate.text = annotationString(annotation, "Contents");
        } else {
            candidate.kind = QStringLiteral("widget");
            candidate.sourceWidgetId = annotationString(annotation, "NM");
            //% "Form field"
            candidate.label = qtTrId("FormField");
            candidate.text = annotationString(annotation, "V");
        }

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
            && !isDesignerManagedSourceWidget(candidate)) {
            return &candidate;
        }
    }
    return nullptr;
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
    const CharacterFormat format = primaryCharacterFormat(draft);
    const QPointF layoutOrigin = layoutOriginForDraft(draft);
    QVariantMap map;
    map.insert(QStringLiteral("objectId"), candidate.objectId);
    map.insert(QStringLiteral("kind"), candidate.kind);
    map.insert(QStringLiteral("label"), candidate.label);
    map.insert(QStringLiteral("sourceRect"), candidate.geometry.sourceRect);
    map.insert(QStringLiteral("frameRect"), draft.frameRect);
    map.insert(QStringLiteral("contentBounds"), draft.contentBounds);
    map.insert(QStringLiteral("deleted"), draft.deleted);
    map.insert(QStringLiteral("sourceRects"), candidateSourceRects(candidate));
    map.insert(QStringLiteral("grouped"), candidate.parts.size() > 1);
    map.insert(QStringLiteral("hasBaseline"), candidate.hasBaseline);
    map.insert(QStringLiteral("originX"), layoutOrigin.x());
    map.insert(QStringLiteral("baselineY"), layoutOrigin.y());
    map.insert(QStringLiteral("text"), draft.text);
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
    map.insert(
        QStringLiteral("highlightRects"),
        candidate.highlightRects.isEmpty()
            ? QVariantList()
            : candidateSourceRects(candidate));
    map.insert(QStringLiteral("highlightColor"), candidate.highlightColor);
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
    return draft;
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
        if ((candidate.kind != QLatin1String("text")
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
    normalizedDraft.candidate = m_selectedCandidate;
    if (m_textEditActive && m_textEditDisplayDraftActive) {
        m_textEditDisplayDraft = normalizedDraft;
    }
    const Draft originalDraft = initialDraftForCandidate(m_selectedCandidate);
    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool matchesOriginal
        = m_selectedCandidate.kind != QLatin1String("addedImage")
        && !isShapeCandidateKind(m_selectedCandidate.kind)
        && sameDraftContent(normalizedDraft, originalDraft);
    bool changed = false;
    if (matchesOriginal) {
        changed = m_drafts.remove(key) > 0;
    } else if (existingDraft == m_drafts.cend()
               || !sameDraftContent(*existingDraft, normalizedDraft)) {
        m_drafts.insert(key, normalizedDraft);
        changed = true;
    }

    m_selectedText = normalizedDraft.text;
    if (changed) {
        markDraftStateChanged(m_selectedCandidate.page, false);
        emit historyStateChanged();
    }
    emit selectionChanged();
}

void PDFEditController::markDraftStateChanged(int page, bool immediate)
{
    const quint64 revision = ++m_nextDraftRevision;
    m_pageDraftRevisions.insert(page, revision);
    emit draftRenderStateChanged(page, revision, immediate);
    emit pageDataChanged(page);
    emit commentsChanged();
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
    if (selectedCandidateChanged
        && (selectionWasRemoved || selectionWasDeleted)) {
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

    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_pendingCandidateScanPage = -1;
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

    if (state.hasSelection) {
        const Candidate selectedCandidate = state.selectedCandidate;
        setSelection(&selectedCandidate);
    } else {
        setSelection(nullptr);
    }
    emit pageDataChanged(-1);
    emit commentsChanged();
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
    if (!candidate) {
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
