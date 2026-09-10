#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFShapeUtils.h"
#include "PDFSourceImageGeometry.h"
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

constexpr qsizetype kMaximumCachedCandidatePages = 12;

// This is a conservative estimate of heap storage retained by the
// whole-document source-comment index. Per-page interaction caches have their
// own LRU bound and are not charged twice for implicitly shared payloads.
constexpr qsizetype kMaximumCommentIndexBytes = 16 * 1024 * 1024;

constexpr qsizetype kEstimatedHeapAllocationOverhead = 32;

constexpr qsizetype kEstimatedIndexedCandidateOverhead = 128;

constexpr unsigned long kMaximumAnnotationStringBytes
    = 4UL * 1024UL * 1024UL;

constexpr qreal kLineBaselineToleranceEm = 0.25;

constexpr qreal kLineMinimumHorizontalGapEm = -0.35;

constexpr qreal kLineMaximumHorizontalGapEm = 1.25;

constexpr qreal kLineWordSeparatorGapEm = 0.20;

constexpr qreal kLineFontSizeToleranceRatio = 0.08;

constexpr qreal kLineFontSizeTolerancePoints = 0.5;

constexpr qreal kParagraphMinimumBaselineGapEm = 0.8;

constexpr qreal kParagraphMaximumBaselineGapEm = 1.6;

constexpr qreal kParagraphLeftAlignmentToleranceEm = 0.4;

constexpr qsizetype kMaximumTextGroupingComparisons = 200000;

constexpr qsizetype kMaximumParagraphLines = 64;

constexpr qsizetype kMaximumParagraphParts = 1024;

constexpr qsizetype kMaximumParagraphCharacters = 32768;

constexpr qreal kTextMarkupMaximumAngleRadians = 0.017453292519943295;

constexpr qreal kFullTurnRadians = 6.2831853071795864769;

constexpr int kMaximumTextMarkupCharactersPerPage = 50000;

constexpr int kLineFontStretchTolerance = 5;

constexpr int kPdfFontFlagItalic = 1 << 6;

constexpr int kPdfFontFlagForceBold = 1 << 18;

constexpr qsizetype kMaximumEmbeddedFontBytes = 32 * 1024 * 1024;

constexpr qsizetype kMaximumRegisteredFontBytes = 64 * 1024 * 1024;

constexpr qsizetype kMaximumRegisteredFontCount = 128;

constexpr qsizetype kMaximumRejectedFontFingerprints = 128;

constexpr char kPreservedPaintMark[] = "TrePDFPreservedPaint";

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

qreal horizontalScaleForTransform(const FS_MATRIX &transform)
{
    // PageToDevice rounds to integer coordinates, making identical transforms
    // appear to have different stretches at fractional text origins. Page
    // rotation and uniform scaling preserve this ratio of the source axes.
    const qreal baselineLength = std::hypot(qreal(transform.a), qreal(transform.b));
    const qreal verticalLength = std::hypot(qreal(transform.c), qreal(transform.d));
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

// Empty means an unmarked legacy object. Reject malformed/duplicate ownership
// marks instead of falling back to geometry and adopting an unrelated path.
bool textUnderlineId(FPDF_PAGEOBJECT object, QString &id)
{
    const int count = FPDFPageObj_CountMarks(object);
    if (count < 0 || count > 64) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(object, index);
        std::array<FPDF_WCHAR, 64> buffer{};
        unsigned long length = 0;
        if (!mark
            || !FPDFPageObjMark_GetName(mark, buffer.data(), sizeof(buffer), &length)
            || length < sizeof(FPDF_WCHAR) || length > sizeof(buffer)
            || length % sizeof(FPDF_WCHAR) != 0) {
            return false;
        }
        const auto stringValue = [&buffer](unsigned long byteLength) {
            qsizetype size = byteLength / sizeof(FPDF_WCHAR);
            while (size > 0 && buffer[size - 1] == 0) {
                --size;
            }
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(buffer.data()), size);
        };
        if (stringValue(length) != QLatin1String("TrePDFUnderline")) {
            continue;
        }
        if (!id.isEmpty()
            || !FPDFPageObjMark_GetParamStringValue(
                mark, "Id", buffer.data(), sizeof(buffer), &length)
            || length < sizeof(FPDF_WCHAR) || length > sizeof(buffer)
            || length % sizeof(FPDF_WCHAR) != 0) {
            return false;
        }
        id = stringValue(length);
        if (id.size() != 36 || QUuid(id).isNull()) {
            return false;
        }
    }
    return true;
}

bool sourceTextUnderlineRect(FPDF_PAGE page,
                             FPDF_PAGEOBJECT textObject,
                             FPDF_PAGEOBJECT path,
                             const QSizeF &pageSize,
                             qreal fontSize,
                             const QColor &color,
                             QRectF &rect)
{
    if (!path || FPDFPageObj_GetType(path) != FPDF_PAGEOBJ_PATH
        || FPDFPath_CountSegments(path) != 2) {
        return false;
    }
    QString textId;
    QString pathId;
    if (!textUnderlineId(textObject, textId) || !textUnderlineId(path, pathId)
        || textId != pathId) {
        return false;
    }
    const bool marked = !textId.isEmpty();
    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL stroke = false;
    float strokeWidth = 0.0F;
    FS_MATRIX pathMatrix{};
    if (!FPDFPath_GetDrawMode(path, &fillMode, &stroke)
        || fillMode != FPDF_FILLMODE_NONE || !stroke
        || FPDFPageObj_GetDashCount(path) != 0
        || !FPDFPageObj_GetStrokeWidth(path, &strokeWidth)
        || !std::isfinite(strokeWidth) || strokeWidth <= 0.0F
        || !FPDFPageObj_GetMatrix(path, &pathMatrix)) {
        return false;
    }
    const auto pagePoint = [&](const QPointF &point, QPointF &result) {
        QPointF normalized;
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())
            || !normalizedPagePoint(page, pageSize, point, normalized)) {
            return false;
        }
        result = QPointF(normalized.x() * pageSize.width(),
                         normalized.y() * pageSize.height());
        return true;
    };
    std::array<QPointF, 2> points;
    for (int index = 0; index < 2; ++index) {
        FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(path, index);
        float x = 0.0F;
        float y = 0.0F;
        if (!segment
            || FPDFPathSegment_GetType(segment)
                != (index == 0 ? FPDF_SEGMENT_MOVETO : FPDF_SEGMENT_LINETO)
            || FPDFPathSegment_GetClose(segment)
            || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !pagePoint(mapPoint(pathMatrix, QPointF(x, y)), points[index])) {
            return false;
        }
    }
    constexpr qreal tolerance = 0.08; // PDF serialization and device-map rounding.
    const qreal width = points[1].x() - points[0].x();
    if (width <= 0.0 || qAbs(points[0].y() - points[1].y()) > tolerance) {
        return false;
    }
    const qreal thickness = strokeWidth * std::hypot(pathMatrix.c, pathMatrix.d);
    if (!qIsFinite(thickness) || thickness <= 0.0) {
        return false;
    }
    if (!marked) {
        // Old TrePDF saves emit TEXT immediately followed by a solid underline
        // with this exact origin, offset, width, thickness and paint. Merely
        // being a horizontal rule below some text is not sufficient ownership.
        FS_MATRIX textMatrix{};
        QPointF origin;
        float left = 0.0F;
        float bottom = 0.0F;
        float right = 0.0F;
        float top = 0.0F;
        if (FPDFPageObj_CountMarks(textObject) != 0
            || FPDFPageObj_CountMarks(path) != 0
            || pageObjectColor(path, true) != color
            || FPDFPageObj_GetLineCap(path) != FPDF_LINECAP_BUTT
            || !FPDFPageObj_GetMatrix(textObject, &textMatrix)
            || !pagePoint(QPointF(textMatrix.e, textMatrix.f), origin)
            || qAbs(points[0].x() - origin.x()) > tolerance
            || qAbs(points[0].y() - origin.y() - qMax<qreal>(0.8, fontSize * 0.10))
                > tolerance
            || qAbs(thickness - qMax<qreal>(0.5, fontSize * 0.05)) > tolerance
            || !FPDFPageObj_GetBounds(textObject, &left, &bottom, &right, &top)) {
            return false;
        }
        const QRectF bounds = normalizedRect(
            page, left, bottom, right, top, kIdentityMatrix, pageSize);
        // The writer uses layout advance; PDFium bounds exclude side bearings.
        if (qAbs(bounds.width() * pageSize.width() - width)
            > qMax<qreal>(1.0, fontSize * 0.20)) {
            return false;
        }
    }
    rect = QRectF(points[0], points[1]).normalized()
        .adjusted(-thickness / 2, -thickness / 2, thickness / 2, thickness / 2);
    rect = QRectF(rect.x() / pageSize.width(), rect.y() / pageSize.height(),
                  rect.width() / pageSize.width(), rect.height() / pageSize.height());
    return true;
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

} // namespace

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

void PDFEditController::ensureCacheRevision()
{
    const int revision = m_document ? m_document->renderRevision() : -1;
    if (m_cachedRevision == revision) {
        return;
    }

    const bool hadPendingTextMarkupCreations
        = !m_pendingTextMarkupCreations.isEmpty();
    m_cachedRevision = revision;
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_deferredCandidateScanPage = -1;
    m_pendingTextMarkupCreations.clear();
    m_pageCandidates.clear();
    m_pageCandidateLru.clear();
    resetCommentIndex();
    emit commentsChanged();
    if (hadPendingTextMarkupCreations) {
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

    const int priorityPage = pendingTextMarkupScanPage();
    if (priorityPage < 0) {
        m_deferredCandidateScanPage = page;
    } else if (page != priorityPage) {
        // Preserve the latest ordinary request while unfinished text-markup
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

    const int priorityPage = pendingTextMarkupScanPage();
    const bool pendingTextMarkupPriority = priorityPage >= 0;
    const int page = pendingTextMarkupPriority
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
        completePendingTextMarkupCreations(request.page);
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

    for (const PendingTextMarkupCreation &creation
         : m_pendingTextMarkupCreations) {
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
        addBytes(estimatedContiguousStorageBytes(
            part.underlineObjectPath.capacity(),
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
            // The current previews and writer use upright axis-aligned quads.
            // Do not create misleading horizontal markup for rotated text.
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

    if (type == FPDF_PAGEOBJ_PATH) {
        PDFShapeUtils::Metadata metadata;
        QByteArray bytes;
        if (objectPath.size() != 1
            || !PDFShapeUtils::readEditableShape(page, pageSize, pageObject,
                                                 metadata, bytes)) {
            return;
        }
        Candidate candidate;
        candidate.objectId = pageObjectId(objectPath);
        candidate.objectPath = objectPath;
        candidate.kind = QStringLiteral("sourceShape");
        //% "Shapes"
        candidate.label = qtTrId("Shapes");
        setInitialGeometry(candidate, metadata.frame);
        candidate.shapeType = metadata.type;
        candidate.shapeStartRatio = metadata.start;
        candidate.shapeEndRatio = metadata.end;
        candidate.shapeStrokeWidthRatio = metadata.strokeWidthRatio;
        candidate.fillColor = metadata.fill.isValid()
            ? metadata.fill : QColor(Qt::transparent);
        candidate.strokeColor = metadata.stroke;
        candidate.commentProperties.strokeColor = metadata.stroke;
        candidate.commentProperties.interiorColor = metadata.fill;
        candidate.commentProperties.opacity = metadata.opacity;
        candidate.sourceShapeMetadata = bytes;
        candidate.editable = request.editingAllowed;
        candidate.renderSuppressible = true;
        if (!request.editingAllowed) {
            //% "This PDF does not permit content editing."
            candidate.reason = qtTrId("PdfEditingNotPermitted");
        }
        candidates.append(std::move(candidate));
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
        candidate.imageResizable = candidate.editable
            && PDFSourceImageGeometry::canResize(page, pageObject);
        candidate.renderSuppressible = true;
        candidate.limited = true;
        if (!editingAllowed) {
            //% "This PDF does not permit content editing."
            candidate.reason = qtTrId("PdfEditingNotPermitted");
        } else if (!topLevelObject) {
            //% "Images inside a PDF form object cannot be deleted safely yet."
            candidate.reason = qtTrId("PdfFormImageDeleteUnsupported");
        } else if (!candidate.imageResizable) {
            //% "Images with clipping, unsupported transforms or bounds outside the page cannot be resized."
            candidate.reason = qtTrId("PdfImageResizeUnsupported");
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
    candidate.horizontalScale = horizontalScaleForTransform(textTransform);
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
    if (candidate.editable && objectPath.size() == 1) {
        const int underlineIndex = objectPath.constFirst() + 1;
        if (underlineIndex < FPDFPage_CountObjects(page)
            && sourceTextUnderlineRect(
                page, pageObject, FPDFPage_GetObject(page, underlineIndex),
                pageSize, candidate.fontSizeRatio * pageSize.height(),
                candidate.color, part.underlineRect)) {
            part.underlineObjectPath = QVector<int>{underlineIndex};
            candidate.fontUnderline = true;
        }
    }
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

    // Overprinted or intersecting source text is ambiguous. Keep every
    // involved object independent, including when their baselines coincide.
    QVector<qsizetype> sourceTextOrder;
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        if (candidates[index].kind == QLatin1String("text")
            && !candidates[index].textMarkupHitTestOnly) {
            sourceTextOrder.append(index);
        }
    }
    std::stable_sort(sourceTextOrder.begin(), sourceTextOrder.end(),
        [&](qsizetype left, qsizetype right) {
            return candidates[left].geometry.sourceRect.top()
                < candidates[right].geometry.sourceRect.top();
        });
    QSet<QString> overlappingText;
    qsizetype comparisons = 0;
    for (qsizetype index = 0; index < sourceTextOrder.size(); ++index) {
        const Candidate &left = candidates[sourceTextOrder[index]];
        for (qsizetype next = index + 1; next < sourceTextOrder.size(); ++next) {
            const Candidate &right = candidates[sourceTextOrder[next]];
            if (right.geometry.sourceRect.top() >= left.geometry.sourceRect.bottom()) {
                break;
            }
            if (++comparisons > kMaximumTextGroupingComparisons) {
                return candidates;
            }
            if (left.geometry.sourceRect.intersects(right.geometry.sourceRect)) {
                overlappingText.insert(left.objectId);
                overlappingText.insert(right.objectId);
            }
        }
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
            && !overlappingText.contains(candidate.objectId)
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
    const auto appendSourceText = [&](Candidate &grouped, const Candidate &source) {
        const qsizetype offset = grouped.text.size();
        QVector<CharacterSpan> spans = source.sourceCharacterSpans;
        if (spans.isEmpty()) {
            spans.append(CharacterSpan{0, source.text.size(),
                characterFormatForCandidate(source)});
        }
        for (CharacterSpan span : std::as_const(spans)) {
            span.start += offset;
            if (!grouped.sourceCharacterSpans.isEmpty()) {
                CharacterSpan &previous = grouped.sourceCharacterSpans.last();
                if (previous.start + previous.length == span.start
                    && sameCharacterFormat(previous.format, span.format)) {
                    previous.length += span.length;
                    continue;
                }
            }
            grouped.sourceCharacterSpans.append(std::move(span));
        }
        grouped.text.append(source.text);
    };
    const auto appendSeparator = [&](Candidate &grouped, const QString &separator,
                                     const Candidate *followingLine = nullptr) {
        if (followingLine) {
            // QTextDocument reports a paragraph separator using the following
            // block's initial character format. Seed inferred breaks the same
            // way so entering/leaving the editor does not create a style edit.
            const CharacterFormat format = followingLine->sourceCharacterSpans.isEmpty()
                ? characterFormatForCandidate(*followingLine)
                : followingLine->sourceCharacterSpans.constFirst().format;
            if (!grouped.sourceCharacterSpans.isEmpty()
                && sameCharacterFormat(grouped.sourceCharacterSpans.constLast().format,
                                       format)) {
                grouped.sourceCharacterSpans.last().length += separator.size();
            } else {
                grouped.sourceCharacterSpans.append(CharacterSpan{
                    grouped.text.size(), separator.size(), format});
            }
        } else if (!grouped.sourceCharacterSpans.isEmpty()) {
            grouped.sourceCharacterSpans.last().length += separator.size();
        }
        grouped.text.append(separator);
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
            grouped.sourceCharacterSpans.clear();
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
                    appendSeparator(grouped, separator);
                }

                CandidatePart part = source.parts.constFirst();
                part.separatorAfter.clear();
                grouped.parts.append(std::move(part));
                appendSourceText(grouped, source);
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

    // Work from complete visual lines, then link only unambiguous neighbors
    // within one column. All paths remain attached to the resulting paragraph
    // so suppression, deletion, movement and Save share the existing pipeline.
    const auto canJoinParagraph = [&](const Candidate &candidate) {
        return candidate.kind == QLatin1String("text")
            && candidate.editable && candidate.renderSuppressible
            && candidate.hasBaseline && !candidate.added
            && !candidate.parts.isEmpty()
            && !overlappingText.contains(candidate.objectId)
            && !candidate.text.contains(QLatin1Char('\n'))
            && !candidate.text.contains(QLatin1Char('\r'))
            && emSize(candidate) > 0.0;
    };
    const auto paragraphStyleMatches = [&](const Candidate &left, const Candidate &right) {
        // A paragraph can change emphasis without changing its typeface.
        // Preserve each line's format instead of applying the first line's
        // bold/italic/underline and calibrated stretch to the entire paragraph.
        return !left.fontFamily.isEmpty()
            && sameFontFamily(left.fontFamily, right.fontFamily)
            && qAbs(emSize(left) - emSize(right))
                <= qMax<qreal>(0.25, 0.02 * qMax(emSize(left), emSize(right)))
            && qAbs(left.fontStretch - right.fontStretch) <= 1
            && left.color == right.color
            && left.fillColor == right.fillColor
            && left.strokeColor == right.strokeColor
            && qFuzzyCompare(left.strokeWidth + 1.0, right.strokeWidth + 1.0)
            && left.textRenderMode == right.textRenderMode;
    };
    const auto alignedLeft = [&](const Candidate &left, const Candidate &right) {
        return qAbs(left.origin.x() - right.origin.x()) * pageSize.width()
            <= kParagraphLeftAlignmentToleranceEm * qMax(emSize(left), emSize(right));
    };
    QVector<qsizetype> paragraphOrder;
    for (qsizetype index = 0; index < output.size(); ++index) {
        if (canJoinParagraph(output[index].candidate)) {
            paragraphOrder.append(index);
        }
    }
    std::stable_sort(paragraphOrder.begin(), paragraphOrder.end(),
        [&](qsizetype left, qsizetype right) {
            return output[left].candidate.origin.y() < output[right].candidate.origin.y();
        });
    QVector<qsizetype> successors(output.size(), -1);
    QVector<qsizetype> predecessorCounts(output.size(), 0);
    bool budgetExceeded = false;
    const auto frameObstructed = [&](const QRectF &frame,
                                      const QVector<qsizetype> &members,
                                      qsizetype next) {
        for (qsizetype other = 0; other < output.size(); ++other) {
            if (++comparisons > kMaximumTextGroupingComparisons) {
                budgetExceeded = true;
                return true;
            }
            if (other == next || members.contains(other)) {
                continue;
            }
            const Candidate &obstacle = output[other].candidate;
            if (!obstacle.textMarkupHitTestOnly
                && (!obstacle.objectPath.isEmpty() || !obstacle.parts.isEmpty())
                && frame.intersects(obstacle.geometry.sourceRect)) {
                return true;
            }
        }
        return false;
    };
    for (qsizetype position = 0; position < paragraphOrder.size() && !budgetExceeded;
         ++position) {
        const qsizetype index = paragraphOrder[position];
        const Candidate &upper = output[index].candidate;
        const qreal upperEm = emSize(upper);
        qreal nearestGap = (std::numeric_limits<qreal>::max)();
        for (qsizetype next = position + 1; next < paragraphOrder.size(); ++next) {
            const qsizetype lowerIndex = paragraphOrder[next];
            const Candidate &lower = output[lowerIndex].candidate;
            const qreal gap = (lower.origin.y() - upper.origin.y()) * pageSize.height();
            if (gap > kParagraphMaximumBaselineGapEm * upperEm) {
                break;
            }
            if (++comparisons > kMaximumTextGroupingComparisons) {
                budgetExceeded = true;
                break;
            }
            const QRectF upperRect = upper.geometry.sourceRect;
            const QRectF lowerRect = lower.geometry.sourceRect;
            const qreal sharedWidth = qMin(upperRect.right(), lowerRect.right())
                - qMax(upperRect.left(), lowerRect.left());
            if (gap < kParagraphMinimumBaselineGapEm * upperEm
                || gap >= nearestGap
                || lowerRect.top() < upperRect.bottom()
                || !alignedLeft(upper, lower)
                || !paragraphStyleMatches(upper, lower)
                || sharedWidth < 0.65 * qMin(upperRect.width(), lowerRect.width())) {
                continue;
            }

            const bool obstructed = frameObstructed(
                upperRect.united(lowerRect), {index}, lowerIndex);
            if (budgetExceeded) {
                break;
            }
            if (!obstructed) {
                successors[index] = lowerIndex;
                nearestGap = gap;
            }
        }
        if (successors[index] >= 0) {
            ++predecessorCounts[successors[index]];
        }
    }
    if (!budgetExceeded) {
        QVector<bool> consumed(output.size(), false);
        QVector<IndexedCandidate> paragraphs;
        paragraphs.reserve(output.size());
        for (qsizetype index : std::as_const(paragraphOrder)) {
            if (consumed[index]) {
                continue;
            }
            QVector<qsizetype> run{index};
            const Candidate &first = output[index].candidate;
            qsizetype partCount = first.parts.size();
            qsizetype characterCount = first.text.size();
            qreal previousGap = 0.0;
            QRectF runRect = first.geometry.sourceRect;
            while (run.size() < kMaximumParagraphLines) {
                const qsizetype last = run.constLast();
                const qsizetype next = successors[last];
                if (next < 0 || consumed[next] || predecessorCounts[next] != 1) {
                    break;
                }
                const Candidate &source = output[next].candidate;
                const qreal gap = (source.origin.y() - output[last].candidate.origin.y())
                    * pageSize.height();
                if (!alignedLeft(first, source) || !paragraphStyleMatches(first, source)
                    || (previousGap > 0.0
                        && qAbs(gap - previousGap) > qMax<qreal>(2.0, 0.2 * emSize(first)))
                    || partCount + source.parts.size() > kMaximumParagraphParts
                    || characterCount + 1 + source.text.size() > kMaximumParagraphCharacters) {
                    break;
                }
                // A long first row can cover a neighboring column beside a
                // later, shorter row even when each adjacent pair is clear.
                const QRectF expandedRect = runRect.united(source.geometry.sourceRect);
                if (frameObstructed(expandedRect, run, next)) {
                    break;
                }
                run.append(next);
                runRect = expandedRect;
                partCount += source.parts.size();
                characterCount += 1 + source.text.size();
                previousGap = gap;
            }
            if (budgetExceeded) {
                break;
            }
            if (run.size() < 2) {
                continue;
            }
            Candidate grouped = first;
            grouped.objectPath.clear();
            grouped.parts.clear();
            grouped.text.clear();
            grouped.sourceCharacterSpans.clear();
            grouped.textGlyphRects.clear();
            QRectF sourceRect = first.geometry.sourceRect;
            qsizetype firstOrder = output[index].order;
            QByteArray identity;
            for (qsizetype member : std::as_const(run)) {
                const Candidate &source = output[member].candidate;
                if (!grouped.parts.isEmpty()) {
                    grouped.parts.last().separatorAfter = QStringLiteral("\n");
                    appendSeparator(grouped, QStringLiteral("\n"), &source);
                }
                grouped.parts.append(source.parts);
                appendSourceText(grouped, source);
                grouped.textGlyphRects.append(source.textGlyphRects);
                grouped.limited = grouped.limited || source.limited;
                sourceRect = sourceRect.united(source.geometry.sourceRect);
                firstOrder = qMin(firstOrder, output[member].order);
                for (const CandidatePart &part : source.parts) {
                    identity.append(part.objectId.toUtf8());
                    identity.append('\0');
                }
                consumed[member] = true;
            }
            grouped.objectId = QStringLiteral("paragraph:")
                + QString::fromLatin1(QCryptographicHash::hash(
                    identity, QCryptographicHash::Sha256).toHex());
            setInitialGeometry(grouped, sourceRect);
            expandInitialTextFrame(grouped, pageSize);
            paragraphs.append(IndexedCandidate{std::move(grouped), firstOrder});
        }
        if (!budgetExceeded) {
            for (qsizetype index = 0; index < output.size(); ++index) {
                if (!consumed[index]) {
                    paragraphs.append(std::move(output[index]));
                }
            }
            output = std::move(paragraphs);
        }
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
