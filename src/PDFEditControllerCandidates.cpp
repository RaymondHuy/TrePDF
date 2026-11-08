#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFFontAliases.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFShapeUtils.h"
#include "PDFTableCellMetadata.h"
#include "PDFSourceImageGeometry.h"
#include "PDFSourceTextGeometry.h"
#include "PDFSymbolFont.h"
#include "PDFHeaderMetadata.h"
#include "PDFiumSynchronization.h"
#include "PDFiumDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QRawFont>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMutexLocker>
#include <QPolygonF>
#include <QQuickTextDocument>
#include <QRunnable>
#include <QSet>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextBoundaryFinder>
#include <QGlyphRun>
#include <QTextCursor>
#include <QTextLayout>
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

template<typename Candidate>
bool sameHeaderGroup(const Candidate &left, const Candidate &right)
{
    return !left.headerMetadataInvalid && !right.headerMetadataInvalid
        && left.headerGroupId == right.headerGroupId && left.headerRole == right.headerRole;
}

template<typename Candidate>
bool containsShearedText(const Candidate &candidate)
{
    return candidate.textShear != 0.0
        || std::any_of(candidate.sourceCharacterSpans.cbegin(),
                       candidate.sourceCharacterSpans.cend(),
                       [](const auto &span) { return span.format.textShear != 0.0; });
}

QByteArray headerImageDigest(FPDF_DOCUMENT document, FPDF_PAGE page,
                             FPDF_PAGEOBJECT object, qint64 &remainingPixels)
{
    return PDFHeaderMetadata::imageDigest(
        PDFHeaderMetadata::nativeImage(document, page, object, remainingPixels));
}

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

    FPDF_DOCUMENT documentLocked(const QString &filePath, const QUuid &documentId, int revision,
                                 const QByteArray &password)
    {
        assertPDFiumProcessLockHeld();
        if (m_document && m_filePath == filePath && m_documentId == documentId
            && m_revision == revision
            && m_password == password) {
            return m_document;
        }

        closeLocked();
        m_document = PDFiumDocument::open(filePath, password);
        if (m_document) {
            m_filePath = filePath;
            m_documentId = documentId;
            m_revision = revision;
            m_password = password;
        }
        return m_document;
    }

    Q_DISABLE_COPY_MOVE(CandidateScanDocument)

private:
    void closeLocked()
    {
        assertPDFiumProcessLockHeld();
        if (m_document) {
            m_document.reset();
        }
        m_filePath.clear();
        m_documentId = QUuid{};
        m_password.clear();
        m_revision = -1;
    }

    PDFiumDocument m_document;
    QString m_filePath;
    QUuid m_documentId;
    QByteArray m_password;
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

bool supportsFormTextLayout(FPDF_PAGE page,
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

// Return the horizontal displacement per unit of ascent (positive means
// leaning right). Work with the PDF matrix before PageToDevice rounding:
// otherwise a small font or a fractional origin changes the apparent angle.
std::optional<qreal> horizontalTextShear(FPDF_PAGE page, const FS_MATRIX &matrix)
{
    if (!qIsFinite(matrix.a) || !qIsFinite(matrix.b) || !qIsFinite(matrix.c)
        || !qIsFinite(matrix.d) || !qIsFinite(matrix.e) || !qIsFinite(matrix.f))
        return std::nullopt;

    // Express both axes in the displayed page's right/up basis. Intrinsic
    // page rotation can make a vertical PDF baseline horizontal on screen.
    QPointF baseline(matrix.a, matrix.b);
    QPointF up(matrix.c, matrix.d);
    switch (FPDFPage_GetRotation(page)) {
    case 0: break;
    case 1:
        baseline = {matrix.b, -matrix.a};
        up = {matrix.d, -matrix.c};
        break;
    case 2:
        baseline = -baseline;
        up = -up;
        break;
    case 3:
        baseline = {-matrix.b, matrix.a};
        up = {-matrix.d, matrix.c};
        break;
    default: return std::nullopt;
    }
    if (baseline.x() <= 0.0 || up.y() <= 0.0
        || std::abs(baseline.y()) > 1e-6 * baseline.x())
        return std::nullopt;
    const qreal shear = up.x() / up.y();
    // Up to about 26.6 degrees in either direction. Reflections, rotated
    // baselines and more extreme oblique artwork stay outside this editor.
    if (std::abs(shear) > 0.5)
        return std::nullopt;
    return std::abs(shear) <= 1e-6 ? 0.0 : shear;
}

qreal normalizedFontSize(FPDF_PAGE page,
                         const QSizeF &pageSize,
                         const FS_MATRIX &transform,
                         qreal fontSize,
                         qreal textShear)
{
    if (textShear != 0.0) {
        // This transform has a horizontal baseline in displayed page space.
        // Project the up vector onto its normal before device rounding.
        const qreal up = (FPDFPage_GetRotation(page) & 1) ? transform.c : transform.d;
        return pageSize.height() > 0.0 ? fontSize * qAbs(up) / pageSize.height() : 0.0;
    }
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF fontTop;
    if (fontSize <= 0.0
        || deviceSize.isEmpty()
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, fontSize), fontTop)) {
        return 0.0;
    }

    const QPointF up = fontTop - origin;
    return std::hypot(up.x(), up.y()) / deviceSize.height();
}

qreal horizontalScaleForTransform(const FS_MATRIX &transform, qreal textShear)
{
    // PageToDevice rounds to integer coordinates, making identical transforms
    // appear to have different stretches at fractional text origins. Page
    // rotation and uniform scaling preserve this ratio. Project the up axis
    // onto the normal to the baseline so slant does not also compress text.
    const qreal baselineLength = std::hypot(qreal(transform.a), qreal(transform.b));
    const qreal verticalLength = std::hypot(qreal(transform.c), qreal(transform.d))
        / std::hypot(1.0, textShear);
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

bool fontSupportsText(const QString &family, bool bold, bool italic, const QString &text)
{
    if (family.isEmpty()) {
        return false;
    }

    QFont font(family);
    font.setBold(bold);
    font.setItalic(italic);
    font.setStyleStrategy(QFont::NoFontMerging);
    const QRawFont raw = QRawFont::fromFont(font);
    if (!raw.isValid() || raw.familyName() != family
        || (raw.weight() >= QFont::Bold) != bold
        || (raw.style() != QFont::StyleNormal) != italic)
        return false;
    for (uint character : text.toUcs4()) {
        if (!QChar::isSpace(static_cast<char32_t>(character))
            && !raw.supportsCharacter(character)) {
            return false;
        }
    }
    return true;
}

QString compatibleSourceFontFamily(const QString &identity, bool bold, bool italic,
                                   const QString &text)
{
    // A Type 1 subset may register in Qt while its custom encoding cannot map
    // punctuation back from Unicode. Termes uses Times-compatible metrics;
    // prefer a complete compatible face over dropping the whole run to the
    // application default. Keep this allowlist explicit: a similar family name
    // or the PDF Serif flag alone does not establish compatible metrics.
    if (fontFamilyMatchKey(identity) != QLatin1String("texgyretermes")) {
        return {};
    }
    for (const QString &family : {QStringLiteral("Times New Roman"),
                                  QStringLiteral("Liberation Serif"),
                                  QStringLiteral("Nimbus Roman")}) {
        if (ApplicationFonts::fontFaceSupportsText(family, bold, italic, text)) {
            return family;
        }
    }
    return {};
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
        if (m_headerSelectionOnly && m_tool == SelectTool
            && !candidateMatchesSelectionScope(candidate)) {
            continue;
        }
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
        if (m_headerSelectionOnly && m_tool == SelectTool
            && !candidateMatchesSelectionScope(candidate)) {
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
                    const QByteArray mapped = PDFSymbolFont::withUnicodeBullets(
                        font, snapshot.embeddedFontData);
                    if (mapped.size() <= remainingSnapshotBytes)
                        snapshot.embeddedFontData = mapped;
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
    // Every embedded face gets a private family name: a PDF subset must not
    // shadow the complete same-name system or cached font in Qt's registry.
    QString embeddedFamily;
    const auto resolvedFamily = resolvedEmbeddedFamilies.constFind(
        candidate.pendingFontSnapshotIndex);
    if (resolvedFamily != resolvedEmbeddedFamilies.cend()) {
        embeddedFamily = resolvedFamily.value();
    } else {
        embeddedFamily = registerEmbeddedFont(candidate.pendingEmbeddedFontData,
                                             candidate.pendingFontExpectedFamily);
        resolvedEmbeddedFamilies.insert(candidate.pendingFontSnapshotIndex, embeddedFamily);
    }
    QString cachedFamily;
    const QString expectedKey = fontFamilyMatchKey(candidate.pendingFontExpectedFamily);
    const QString identityKey = fontFamilyMatchKey(candidate.fontIdentity);
    for (const QString &family : ApplicationFonts::downloadedFontFamilies()) {
        const QString key = fontFamilyMatchKey(family);
        if ((!expectedKey.isEmpty() && key == expectedKey)
            || (!identityKey.isEmpty() && key == identityKey)) {
            cachedFamily = family;
            break;
        }
    }
    if (fontSupportsText(embeddedFamily, candidate.fontBold, candidate.fontItalic, candidate.text)) {
        candidate.fontFamily = embeddedFamily;
    } else if (ApplicationFonts::fontFaceSupportsText(
                   candidate.pendingSystemFontFamily,
                   candidate.fontBold,
                   candidate.fontItalic,
                   candidate.text, ApplicationFonts::FontSource::System)) {
        candidate.fontFamily = candidate.pendingSystemFontFamily;
    } else if (!cachedFamily.isEmpty()
               && ApplicationFonts::fontFaceSupportsText(cachedFamily,
                   candidate.fontBold, candidate.fontItalic, candidate.text,
                   ApplicationFonts::FontSource::Downloaded)) {
        candidate.fontFamily = cachedFamily;
    } else if (ApplicationFonts::fontFaceSupportsText(
                   candidate.pendingBundledFontFamily,
                   candidate.fontBold,
                   candidate.fontItalic,
                   candidate.text, ApplicationFonts::FontSource::Bundled)) {
        candidate.fontFamily = candidate.pendingBundledFontFamily;
    } else {
        candidate.fontFamily.clear();
    }

    if (candidate.fontFamily.isEmpty()) {
        const QString fallbackFamily = compatibleSourceFontFamily(candidate.fontIdentity,
            candidate.fontBold, candidate.fontItalic, candidate.text);
        if (!fallbackFamily.isEmpty()) {
            QByteArray bytes = ApplicationFonts::systemFontFace(fallbackFamily,
                candidate.fontBold, candidate.fontItalic).data;
            if (bytes.isEmpty()) {
                bytes = ApplicationFonts::bundledFontData(fallbackFamily,
                    candidate.fontBold, candidate.fontItalic);
            }
            // This compatibility preview must not turn a missing TeX font
            // into a Times request. Save still resolves/downloads the actual
            // source family before accepting any final substitution.
            candidate.fontFamily = registerEmbeddedFont(bytes,
                QStringLiteral("TeX Gyre Termes"), true);
        }
    }

    if (candidate.fontFamily.isEmpty()) {
        // Qt already uses its ordinary display fallback for an unknown family.
        // Retain the requested source name so Save can try reviewed providers
        // instead of losing it when the embedded program cannot be registered.
        candidate.fontFamily = candidate.pendingFontExpectedFamily;
        if (!candidate.fontIdentity.isEmpty()
            && (candidate.fontFamily.isEmpty() || identityKey != expectedKey)) {
            // PDFium can report a generic substitute (e.g. Chrome Sans MM) as
            // the family when no usable program exists. Preserve /BaseFont.
            candidate.fontFamily = candidate.fontIdentity;
            static const QRegularExpression styleSuffix(QStringLiteral(
                "[-,](Regular|BoldItalic|BoldOblique|Bold|Italic|Oblique)$"),
                QRegularExpression::CaseInsensitiveOption);
            candidate.fontFamily.remove(styleSuffix);
        }
        static const QStringList texFamilies{
            QStringLiteral("TeX Gyre Termes"), QStringLiteral("TeX Gyre Heros"),
            QStringLiteral("TeX Gyre Pagella"), QStringLiteral("TeX Gyre Schola"),
            QStringLiteral("TeX Gyre Cursor")};
        for (const QString &family : texFamilies) {
            const QString key = fontFamilyMatchKey(family);
            if (key == expectedKey || key == identityKey) {
                candidate.fontFamily = family;
                break;
            }
        }
    }

    candidate.pendingFontSnapshotIndex = -1;
    candidate.pendingFontExpectedFamily.clear();
    candidate.pendingBundledFontFamily.clear();
    candidate.pendingSystemFontFamily.clear();
    candidate.pendingEmbeddedFontData.clear();
}

QString PDFEditController::registerEmbeddedFont(
    const QByteArray &fontData,
    const QString &expectedFamily,
    bool substitute)
{
    assertPDFiumProcessLockNotHeld();
    if (fontData.isEmpty()) {
        return {};
    }

    QByteArray fingerprint = QCryptographicHash::hash(
        fontData,
        QCryptographicHash::Sha256);
    if (substitute) {
        fingerprint = QCryptographicHash::hash(fingerprint + '\0' + expectedFamily.toUtf8(),
            QCryptographicHash::Sha256);
    }
    const auto registeredFamily = m_registeredFontFamilies.constFind(fingerprint);
    if (registeredFamily != m_registeredFontFamilies.cend()) {
        return registeredFamily.value();
    }
    if (m_rejectedFontFingerprints.contains(fingerprint)) {
        return {};
    }
    if (m_registeredFontFamilies.size() >= kMaximumRegisteredFontCount
        || fontData.size() > kMaximumRegisteredFontBytes - m_registeredFontBytes) {
        rememberRejectedFontFingerprint(fingerprint);
        return {};
    }

    // Mark the fingerprint before calling Qt. Adding an application font emits
    // fontDatabaseChanged synchronously; a re-entrant candidate finalization
    // must not try to register the same bytes recursively.
    rememberRejectedFontFingerprint(fingerprint);

    const PDFFontAliases::AliasData alias = PDFFontAliases::isolate(fontData, expectedFamily, substitute);
    if (alias.data.isEmpty()
        || alias.data.size() > kMaximumRegisteredFontBytes - m_registeredFontBytes
        || !PDFFontAliases::retain(alias)) {
        return {};
    }
    const QString family = alias.family;

    m_registeredFontBytes += alias.data.size();
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
    QHash<QByteArray, QString> families;
    families.swap(m_registeredFontFamilies);
    m_rejectedFontFingerprints.clear();
    m_rejectedFontFingerprintLru.clear();
    m_registeredFontBytes = 0;
    // Removing the last owner can synchronously emit fontDatabaseChanged.
    // Detach our ownership before a callback can reset the controller again.
    for (const QString &family : std::as_const(families))
        PDFFontAliases::release(family);
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
        request.documentId = m_document->instanceId();
        request.password = m_document->documentPassword();
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
                        request.documentId,
                        revision,
                        request.password);
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
        && m_document->instanceId() == request.documentId
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
    addString(candidate.stampDetail);
    addString(candidate.stampArtwork);
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
    request.documentId = m_document->instanceId();
    request.password = m_document->documentPassword();
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
        request.documentId,
        request.revision,
        request.password);
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
            // A TJ displacement can represent a word separator without a
            // literal space glyph. PDFium exposes that separator as generated
            // text. Keep it only inside the same object: generated row/object
            // separators belong to the grouping stage, not to either run.
            if (generated && FPDFText_GetUnicode(textPage, index) == ' '
                && index > 0 && index + 1 < characterCount
                && FPDFText_IsGenerated(textPage, index - 1) == 0
                && FPDFText_IsGenerated(textPage, index + 1) == 0
                && FPDFText_GetTextObject(textPage, index - 1) == textObject
                && FPDFText_GetTextObject(textPage, index + 1) == textObject
                && !objectLayout->text.isEmpty()
                && !objectLayout->text.back().isSpace()
                && !QChar::isSpace(static_cast<char32_t>(
                    FPDFText_GetUnicode(textPage, index + 1)))) {
                objectLayout->text.append(QLatin1Char(' '));
                if (!objectLayout->sourcePositions.isEmpty()) {
                    const auto &previous = objectLayout->sourcePositions.last();
                    objectLayout->sourcePositions.append(SourceTextPosition{
                        objectLayout->text.size() - 1, 1, previous.x + previous.advance,
                        0.0, previous.y});
                }
                // Its reported origin is the next glyph's origin, not a real
                // space advance. Do not treat the TJ word gap as letter spacing.
                objectLayout->hasSpacingOrigin = false;
            }
            if (!generated) {
                if (!objectLayout->hasOrigin) {
                    double originX = 0.0;
                    double originY = 0.0;
                    if (FPDFText_GetCharOrigin(textPage, index, &originX, &originY)) {
                        objectLayout->origin = QPointF(originX, originY);
                        objectLayout->hasOrigin = true;
                    }
                }

                unsigned int unicode = FPDFText_GetUnicode(textPage, index);
                // PDFium replaces a visible end-of-line hyphen with its
                // internal U+0002 marker for text extraction. That marker is
                // not an encodable source glyph: saving it with a simple font
                // can silently emit 0xFF and a missing-glyph box. Keep the
                // visible hyphen in editable text, using PDFium's flag so an
                // unrelated control character is never guessed to be a dash.
                if (unicode == 0x0002U && FPDFText_IsHyphen(textPage, index) == 1) {
                    unicode = '-';
                }
                if (unicode > 0
                    && unicode <= 0x10FFFF
                    && !QChar::isSurrogate(static_cast<char32_t>(unicode))) {
                    const char32_t codePoint = static_cast<char32_t>(unicode);
                    hasUsableUnicode = true;
                    whitespace = QChar::isSpace(codePoint);
                    objectLayout->text.append(QString::fromUcs4(&codePoint, 1));
                    objectLayout->encodedText.append(QString::fromUcs4(&codePoint, 1));
                    if (!objectLayout->positionsUnsupported) {
                        FS_MATRIX matrix{};
                        double x = 0.0, y = 0.0;
                        float advance = 0.0F;
                        QPointF origin, end;
                        const auto script = QChar::script(codePoint);
                        const qreal size = FPDFText_GetFontSize(textPage, index);
                        const bool supported = collectTextMarkupGlyphRects
                            && objectLayout->sourcePositions.size() < 4096
                            && (script == QChar::Script_Latin || script == QChar::Script_Common)
                            && QChar::isPrint(codePoint) && !QChar::isMark(codePoint)
                            && (!QChar::isSpace(codePoint) || codePoint == U' ')
                            && FPDFText_HasUnicodeMapError(textPage, index) == 0
                            && FPDFText_GetCharOrigin(textPage, index, &x, &y)
                            && FPDFText_GetMatrix(textPage, index, &matrix)
                            && qIsFinite(size) && size > 0.0
                            && FPDFFont_GetGlyphWidth(FPDFTextObj_GetFont(textObject),
                                                     unicode, size, &advance)
                            && qIsFinite(advance) && advance > 0.0
                            && normalizedPagePoint(page, request.pageSize, QPointF(x, y), origin)
                            && normalizedPagePoint(page, request.pageSize,
                                QPointF(x + advance * matrix.a, y + advance * matrix.b), end)
                            && qAbs(end.y() - origin.y()) * request.pageSize.height() < 0.05
                            && end.x() > origin.x();
                        const qreal pointX = origin.x() * request.pageSize.width();
                        const qreal pointY = origin.y() * request.pageSize.height();
                        if (!supported || (!objectLayout->sourcePositions.isEmpty()
                                && (pointX < objectLayout->sourcePositions.last().x
                                    || qAbs(pointY - objectLayout->sourcePositions.first().y)
                                        > 0.05))) {
                            objectLayout->positionsUnsupported = true;
                            objectLayout->sourcePositions.clear();
                        } else {
                            objectLayout->sourcePositions.append(SourceTextPosition{
                                objectLayout->text.size() - (codePoint > 0xffff ? 2 : 1),
                                codePoint > 0xffff ? 2 : 1, pointX,
                                (end.x() - origin.x()) * request.pageSize.width(), pointY});
                        }
                    }
                    // Infer only uniform spacing in simple horizontal runs.
                    // Origins and font advances are PDF geometry, so this does
                    // not mistake font substitution or ink side bearings for
                    // character spacing. Irregular TJ/complex shaping falls back.
                    if (!objectLayout->spacingUnsupported) {
                        FS_MATRIX matrix{};
                        double x = 0.0;
                        double y = 0.0;
                        float advance = 0.0F;
                        const qreal fontSize = FPDFText_GetFontSize(textPage, index);
                        const QChar::Script script = QChar::script(codePoint);
                        const bool simpleCharacter = codePoint == U' '
                            || ((script == QChar::Script_Latin
                                 || script == QChar::Script_Common)
                                && QChar::isPrint(codePoint)
                                && !QChar::isMark(codePoint)
                                && !QChar::isSpace(codePoint));
                        QPointF normalizedOrigin;
                        QPointF normalizedAdvance;
                        const bool usable = collectTextMarkupGlyphRects
                            && simpleCharacter
                            && objectLayout->spacingSamples.size() < 4096
                            && FPDFText_HasUnicodeMapError(textPage, index) == 0
                            && FPDFText_GetCharOrigin(textPage, index, &x, &y)
                            && FPDFText_GetMatrix(textPage, index, &matrix)
                            && qIsFinite(fontSize) && fontSize > 0.0
                            && FPDFFont_GetGlyphWidth(FPDFTextObj_GetFont(textObject),
                                unicode, fontSize, &advance)
                            && qIsFinite(advance) && advance > 0.0
                            && normalizedPagePoint(page, request.pageSize,
                                QPointF(x, y), normalizedOrigin)
                            && normalizedPagePoint(page, request.pageSize,
                                QPointF(x + advance * matrix.a,
                                        y + advance * matrix.b), normalizedAdvance);
                        const qreal width = (normalizedAdvance.x() - normalizedOrigin.x())
                            * request.pageSize.width();
                        if (!usable || width <= 0.0
                            || qAbs(normalizedAdvance.y() - normalizedOrigin.y())
                                * request.pageSize.height() > 0.05) {
                            objectLayout->spacingUnsupported = true;
                            objectLayout->spacingSamples.clear();
                        } else {
                            const QPointF origin(normalizedOrigin.x() * request.pageSize.width(),
                                                 normalizedOrigin.y() * request.pageSize.height());
                            objectLayout->spacingEmSize = fontSize * std::hypot(matrix.c, matrix.d);
                            if (objectLayout->hasSpacingOrigin) {
                                const QPointF delta = origin - objectLayout->previousSpacingOrigin;
                                if (delta.x() <= 0.0 || qAbs(delta.y()) > 0.05) {
                                    objectLayout->spacingUnsupported = true;
                                    objectLayout->spacingSamples.clear();
                                } else {
                                    objectLayout->spacingSamples.append(TextSpacingSample{
                                        delta.x() - objectLayout->previousSpacingAdvance,
                                        objectLayout->previousSpacingWasSpace});
                                }
                            }
                            objectLayout->previousSpacingOrigin = origin;
                            objectLayout->previousSpacingAdvance = width;
                            objectLayout->previousSpacingWasSpace = codePoint == U' ';
                            objectLayout->hasSpacingOrigin = true;
                        }
                    }
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
    PDFTableCellMetadata::PageIndex tableCellIndex;
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
            tableCellIndex,
            cancellation);
    }
    qint64 remainingHeaderImagePixels = PDFHeaderMetadata::kMaximumDigestPixels;
    for (Candidate &candidate : candidates) {
        if (candidateScanCancelled(cancellation))
            break;
        const QRectF rect = candidate.geometry.sourceRect;
        if (candidate.kind == QLatin1String("image") && candidate.editable
            && candidate.imageResizable && !candidate.headerMetadataInvalid
            && candidate.objectPath.size() == 1 && rect.left() >= 0.0 && rect.top() >= 0.0
            && rect.right() <= 1.0 && rect.bottom() <= 1.0
            && (rect.bottom() * request.pageSize.height() <= 100.001
                || rect.top() * request.pageSize.height() >= request.pageSize.height() - 100.001)) {
            candidate.headerImageDigest = headerImageDigest(document, page,
                FPDFPage_GetObject(page, candidate.objectPath.constFirst()), remainingHeaderImagePixels);
        }
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
            preserveSourceTextPositions(candidate, pageSize);
            expandInitialTextFrame(candidate, pageSize);
        }
        candidate.page = page;
    }
    candidates = restoreSourceParagraphLayouts(std::move(candidates), pageSize);
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
    PDFTableCellMetadata::PageIndex &tableCellIndex,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    if (!pageObject || candidateScanCancelled(cancellation)
        || PDFWatermarks::hasMark(pageObject)
        || PDFTableCellMetadata::hasMark(pageObject)) {
        return;
    }
    const QSizeF &pageSize = request.pageSize;

    const int type = FPDFPageObj_GetType(pageObject);
    const bool imageOnlyForm = objectPath.size() == 1
        && PDFSourceImageGeometry::isImageOnlyForm(pageObject);
    if (type == FPDF_PAGEOBJ_FORM && !imageOnlyForm) {
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
                tableCellIndex,
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
        PDFTableCellMetadata::Group tableCells;
        if (!PDFTableCellMetadata::readGroup(page, pageSize, metadata, textPage, tableCells,
                                             &tableCellIndex, cancellation))
            return;
        Candidate candidate;
        candidate.objectId = pageObjectId(objectPath);
        candidate.objectPath = objectPath;
        candidate.kind = QStringLiteral("sourceShape");
        //% "Shapes"
        candidate.label = qtTrId("Shapes");
        setInitialGeometry(candidate, metadata.frame);
        candidate.shapeType = metadata.type;
        candidate.tableRows = metadata.tableRows;
        candidate.tableColumns = metadata.tableColumns;
        candidate.tableRowStops = metadata.tableRowStops;
        candidate.tableColumnStops = metadata.tableColumnStops;
        candidate.tableCellTexts = metadata.tableCellTexts;
        candidate.tableFontFamily = metadata.tableFontFamily;
        candidate.tableFontSize = metadata.tableFontSize;
        candidate.tableId = metadata.tableId;
        candidate.tableCellObjectPaths = std::move(tableCells.objectPaths);
        candidate.tableCellFingerprint = std::move(tableCells.fingerprint);
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

    if (type != FPDF_PAGEOBJ_TEXT && type != FPDF_PAGEOBJ_IMAGE && !imageOnlyForm) {
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
    const auto headerMetadata = PDFHeaderMetadata::read(pageObject);
    candidate.headerMetadataInvalid = headerMetadata.invalid
        || (!headerMetadata.group.isEmpty()
            && ((type == FPDF_PAGEOBJ_TEXT && (headerMetadata.role == QLatin1String("image")
                || headerMetadata.role == QLatin1String("footerImage")))
                || (type != FPDF_PAGEOBJ_TEXT && headerMetadata.role != QLatin1String("image")
                    && headerMetadata.role != QLatin1String("footerImage"))));
    if (!candidate.headerMetadataInvalid) {
        candidate.headerGroupId = headerMetadata.group;
        candidate.headerRole = headerMetadata.role;
    }
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

    if (type == FPDF_PAGEOBJ_IMAGE || imageOnlyForm) {
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
    const bool topLevelObject = objectPath.size() == 1;
    bool geometrySupported = false;
    if (topLevelObject) {
        const auto shear = horizontalTextShear(page, textTransform);
        geometrySupported = shear.has_value();
        if (shear && *shear != 0.0) {
            // A regular face transformed about a horizontal baseline can be
            // reproduced exactly. Additional clipping or an already italic
            // font would require independent paint or inclination handling.
            const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(pageObject);
            geometrySupported = candidate.hasBaseline && !candidate.fontItalic
                && renderMode == FPDF_TEXTRENDERMODE_FILL
                && clip && FPDFClipPath_CountPaths(clip) == -1;
            if (geometrySupported) {
                candidate.textShear = *shear;
                candidate.preservePhysicalFontSizes = true;
            }
        }
    } else {
        geometrySupported = supportsFormTextLayout(page, pageSize, textTransform);
    }
    const bool inlineLayoutSupported = geometrySupported
        && !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r'));
    candidate.fontSizeRatio = qMax<qreal>(
        0.0,
        normalizedFontSize(page, pageSize, textTransform, fontSize, candidate.textShear));
    candidate.displayFontSizeRatio = candidate.fontSizeRatio;
    candidate.horizontalScale = horizontalScaleForTransform(textTransform, candidate.textShear);
    candidate.fontStretch = qBound(
        static_cast<int>(QFont::UltraCondensed),
        qRound(candidate.horizontalScale * static_cast<qreal>(QFont::Unstretched)),
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
    const FPDF_PAGEOBJECT outerObject = !topLevelObject && !objectPath.isEmpty()
        ? FPDFPage_GetObject(page, objectPath.constFirst())
        : nullptr;
    const bool textOnlyForm = outerObject
        && PDFSourceTextGeometry::isTextOnlyForm(outerObject)
        && PDFSourceTextGeometry::textObject(outerObject) == pageObject;
    const bool editableOccurrence = topLevelObject || textOnlyForm;
    if (textOnlyForm) {
        // Keep the leaf's text, font and composed geometry, but own the whole
        // page-level occurrence. Suppression, movement and replacement must
        // never mutate a child of a potentially shared Form stream.
        candidate.objectPath = QVector<int>{objectPath.constFirst()};
        candidate.objectId = pageObjectId(candidate.objectPath);
    }
    candidate.editable = editingAllowed
        && suppressionSupported
        && editableOccurrence
        && inlineLayoutSupported;
    candidate.renderSuppressible = suppressionSupported;
    candidate.limited = true;
    if (!editingAllowed) {
        //% "This PDF does not permit content editing."
        candidate.reason = qtTrId("PdfEditingNotPermitted");
    } else if (!suppressionSupported) {
        //% "Text clipping render modes cannot be edited safely yet."
        candidate.reason = qtTrId("PdfTextClippingEditUnsupported");
    } else if (!editableOccurrence) {
        //% "Text inside a PDF form object cannot be saved safely yet."
        candidate.reason = qtTrId("PdfFormTextSaveUnsupported");
    } else if (!inlineLayoutSupported) {
        //% "Rotated, transformed, or multiline text is not supported by the inline draft editor yet."
        candidate.reason = qtTrId("InlineEditorUnsupportedTextLayout");
    }

    if (candidate.editable && textLayout != textObjectLayouts.cend()) {
        inferSourceTextSpacing(candidate, *textLayout, pageSize);
        readSourceTextSpacing(pageObject, candidate, pageSize, textLayout->encodedText);
        readSourceParagraphLayout(page, pageObject, candidate, pageSize, textLayout->encodedText);
        if (!textLayout->positionsUnsupported && !candidate.invalidSpacingMetadata
            && candidate.spacingGroup.isEmpty() && candidate.paragraphLayoutGroup.isEmpty()) {
            // Compare complete origins after resolving the display face. Small
            // TJ displacements can accumulate, and even an unspaced Tj run can
            // differ from Qt's kerning. Neither case has a reliable per-gap
            // threshold. Uniform spacing remains a separate semantic value.
            candidate.pendingSourcePositions = textLayout->sourcePositions;
        }
    }

    CandidatePart part;
    part.objectId = candidate.objectId;
    part.objectPath = candidate.objectPath;
    part.sourceRect = candidate.geometry.sourceRect;
    part.origin = candidate.origin;
    // Generated word separators are editable text, but they do not prove
    // that the original subset font can encode a space during Save.
    part.text = textLayout != textObjectLayouts.cend() && !textLayout->text.isEmpty()
        ? textLayout->encodedText : candidate.text;
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

void PDFEditController::preserveSourceTextPositions(Candidate &candidate,
                                                   const QSizeF &pageSize) const
{
    const auto positions = std::exchange(candidate.pendingSourcePositions, {});
    if (positions.size() < 2 || positions.size() > 1024 || !candidate.editable
        || candidate.text.size() > 4096 || candidate.fontFamily.isEmpty()
        || pageSize.isEmpty())
        return;
    QSet<qsizetype> graphemes{0, candidate.text.size()};
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, candidate.text);
    for (int p = finder.toNextBoundary(); p >= 0; p = finder.toNextBoundary())
        graphemes.insert(p);
    const CharacterFormat base = characterFormatForCandidate(candidate);
    const qreal height = pageSize.height();
    const qreal fontSize = base.displayFontSizeRatio > 0.0
        ? base.displayFontSizeRatio : base.fontSizeRatio;
    const QFont font = fontForDisplay(base.fontFamily, base.fontStretch,
        base.fontBold, base.fontItalic, base.fontUnderline,
        fontSize * height, base.letterSpacingRatio * height,
        base.wordSpacingRatio * height);
    QVector<CharacterSpan> spans;
    QVector<qreal> expected;
    QSet<qsizetype> originalGlyphStarts;
    auto measure = [&](const QVector<CharacterSpan> &formats, QVector<qreal> &origins,
                       QSet<qsizetype> &glyphStarts) {
        QTextLayout layout(candidate.text, font);
        QList<QTextLayout::FormatRange> ranges;
        for (const auto &span : formats) {
            QFont face = font;
            face.setLetterSpacing(QFont::AbsoluteSpacing,
                (base.letterSpacingRatio + span.format.sourceAdvanceRatio) * height);
            // The correction already accounts for the original pair spacing.
            // Do not apply Qt kerning again if equal spans merge after reopen.
            face.setKerning(qFuzzyIsNull(span.format.sourceAdvanceRatio));
            QTextCharFormat format;
            format.setFont(face);
            ranges.append({int(span.start), int(span.length), format});
        }
        layout.setFormats(ranges);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            layout.endLayout();
            return false;
        }
        line.setLineWidth(1000000.0);
        layout.endLayout();
        for (const auto &run : line.glyphRuns(0, candidate.text.size(),
                                            QTextLayout::RetrieveStringIndexes)) {
            if (run.isRightToLeft())
                return false;
            for (qsizetype p : run.stringIndexes())
                glyphStarts.insert(p);
        }
        for (const auto &p : positions)
            origins.append(line.cursorToX(p.start));
        origins.append(line.cursorToX(candidate.text.size()));
        return true;
    };
    QVector<qreal> measured;
    if (!measure({}, measured, originalGlyphStarts))
        return;
    // Admit only whole grapheme/shaping clusters. A PDF that exposes multiple
    // characters inside a ligature needs its glyph mapping, not guessed tracking.
    for (qsizetype i = 0; i < positions.size(); ++i) {
        const auto &p = positions[i];
        if (!graphemes.contains(p.start) || !graphemes.contains(p.start + p.length)
            || !originalGlyphStarts.contains(p.start)
            || p.start != (i ? positions[i - 1].start + positions[i - 1].length : 0))
            return;
        expected.append(p.x - positions.first().x);
        spans.append({p.start, p.length, base});
    }
    if (positions.last().start + positions.last().length != candidate.text.size())
        return;
    expected.append(expected.last() + positions.last().advance);
    bool originsMatch = true;
    for (qsizetype i = 0; i < positions.size(); ++i) {
        if (!qIsFinite(expected[i]) || !qIsFinite(measured[i]))
            return;
        originsMatch = originsMatch && qAbs(measured[i] - expected[i]) <= 0.08;
    }
    // Avoid introducing per-cluster format boundaries when Qt already matches
    // the source; a terminal tracking advance does not move a painted glyph.
    if (originsMatch)
        return;
    for (int pass = 0; pass < 3; ++pass) {
        for (qsizetype i = 0; i < spans.size(); ++i) {
            auto &correction = spans[i].format.sourceAdvanceRatio;
            correction += ((expected[i + 1] - expected[i])
                - (measured[i + 1] - measured[i])) / height;
            if (!qIsFinite(correction) || qAbs(correction * height) > 512.0)
                return;
        }
        measured.clear();
        QSet<qsizetype> glyphStarts;
        if (!measure(spans, measured, glyphStarts) || glyphStarts != originalGlyphStarts)
            return;
    }
    for (qsizetype i = 0; i < positions.size(); ++i) {
        if (qAbs(measured[i] - expected[i]) > 0.08)
            return;
    }
    for (const CharacterSpan &span : std::as_const(spans)) {
        if (!candidate.sourceCharacterSpans.isEmpty()
            && sameCharacterFormat(candidate.sourceCharacterSpans.last().format, span.format)) {
            candidate.sourceCharacterSpans.last().length += span.length;
        } else {
            candidate.sourceCharacterSpans.append(span);
        }
    }
}

void PDFEditController::inferSourceTextSpacing(
    Candidate &candidate, const TextObjectLayout &layout, const QSizeF &pageSize)
{
    if (layout.spacingUnsupported || pageSize.isEmpty()) {
        return;
    }
    QVector<qreal> letters;
    QVector<qreal> spaces;
    for (const TextSpacingSample &sample : layout.spacingSamples) {
        (sample.followsSpace ? spaces : letters).append(sample.residual);
    }
    // Short runs and irregular TJ positioning cannot establish a reliable
    // uniform Tc/Tw value. Retain the existing layout fallback in those cases.
    if (letters.size() < 4) {
        return;
    }
    const auto median = [](QVector<qreal> values) {
        std::sort(values.begin(), values.end());
        const qsizetype middle = values.size() / 2;
        return values.size() % 2 ? values[middle]
            : (values[middle - 1] + values[middle]) / 2.0;
    };
    const qreal tolerance = qMax(0.05, layout.spacingEmSize * 0.004);
    const auto uniform = [&](const QVector<qreal> &values, qreal expected) {
        return std::all_of(values.cbegin(), values.cend(), [&](qreal value) {
            return qIsFinite(value) && qAbs(value - expected) <= tolerance;
        });
    };
    qreal letter = median(letters);
    qreal word = spaces.size() >= 2 ? median(spaces) - letter : 0.0;
    if (!uniform(letters, letter) || !uniform(spaces, letter + word)
        || letter < -10.0 || letter > 50.0 || word < -10.0 || word > 50.0) {
        return;
    }
    // The page/device conversion has finite precision. Do not materialize its
    // rounding noise as a source style or a no-op editor change.
    letter = qAbs(letter) < 0.02 ? 0.0 : qRound64(letter * 1000.0) / 1000.0;
    word = qAbs(word) < 0.02 ? 0.0 : qRound64(word * 1000.0) / 1000.0;
    candidate.letterSpacingRatio = letter / pageSize.height();
    candidate.wordSpacingRatio = word / pageSize.height();
}

void PDFEditController::readSourceTextSpacing(
    FPDF_PAGEOBJECT object, Candidate &candidate, const QSizeF &pageSize,
    const QString &encodedSourceText)
{
    assertPDFiumProcessLockHeld();
    if (pageSize.isEmpty()) {
        return;
    }
    const int count = FPDFPageObj_CountMarks(object);
    if (count < 1 || count > 64) {
        candidate.invalidSpacingMetadata = count > 64;
        return;
    }
    FPDF_PAGEOBJECTMARK spacingMark = nullptr;
    for (int index = 0; index < count; ++index) {
        const auto mark = FPDFPageObj_GetMark(object, index);
        FPDF_WCHAR name[64]{};
        unsigned long size = 0;
        if (!mark || !FPDFPageObjMark_GetName(mark, name, sizeof(name), &size)
            || size < 2 || size > sizeof(name) || size % 2 != 0) {
            continue;
        }
        const QString value = QString::fromUtf16(
            reinterpret_cast<const char16_t *>(name), size / 2 - 1);
        if (value != QLatin1String("TrePDFTextSpacing")) {
            continue;
        }
        // A present but invalid mark must not become an unmarked source:
        // paragraph restoration and source calibration would otherwise silently
        // replace a failed spacing record with newly inferred/default values.
        candidate.invalidSpacingMetadata = true;
        if (spacingMark) {
            return;
        }
        spacingMark = mark;
    }
    if (!spacingMark) {
        return;
    }
    int version = 0;
    int index = -1;
    int parts = 0;
    int start = -1;
    float letter = 0.0F;
    float word = 0.0F;
    float sourceAdvance = 0.0F;
    FPDF_WCHAR group[64]{};
    unsigned long groupSize = 0;
    unsigned long textSize = 0;
    if (!FPDFPageObjMark_GetParamIntValue(spacingMark, "Version", &version)
        || (version != 1 && version != 2)
        || !FPDFPageObjMark_GetParamIntValue(spacingMark, "Index", &index)
        || !FPDFPageObjMark_GetParamIntValue(spacingMark, "Count", &parts)
        || !FPDFPageObjMark_GetParamIntValue(spacingMark, "Start", &start)
        || parts < 1 || parts > 1024 || index < 0 || index >= parts
        || start < 0 || start >= 32768
        || !FPDFPageObjMark_GetParamFloatValue(spacingMark, "LetterSpacing", &letter)
        || !FPDFPageObjMark_GetParamFloatValue(spacingMark, "WordSpacing", &word)
        || (version == 2 && (!FPDFPageObjMark_GetParamFloatValue(spacingMark, "SourceAdvance", &sourceAdvance)
            || !qIsFinite(sourceAdvance) || qAbs(sourceAdvance) > 512.0F))
        || !qIsFinite(letter) || letter < -10.0F || letter > 50.0F
        || !qIsFinite(word) || word < -10.0F || word > 50.0F
        || !FPDFPageObjMark_GetParamStringValue(spacingMark, "Group", group,
                                               sizeof(group), &groupSize)
        || groupSize < 2 || groupSize > sizeof(group) || groupSize % 2 != 0
        || !FPDFPageObjMark_GetParamBlobValue(spacingMark, "Text", nullptr, 0, &textSize)
        || textSize == 0 || textSize > 131072) {
        return;
    }
    const QString groupId = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(group), groupSize / 2 - 1);
    if (QUuid(groupId).isNull()) {
        return;
    }
    QByteArray bytes(static_cast<qsizetype>(textSize), Qt::Uninitialized);
    unsigned long actualSize = 0;
    if (!FPDFPageObjMark_GetParamBlobValue(spacingMark, "Text",
            reinterpret_cast<unsigned char *>(bytes.data()), textSize, &actualSize)
        || actualSize != textSize) {
        return;
    }
    const QString text = QString::fromUtf8(bytes);
    // PDFium collapses repeated boundary spaces in extracted object text.
    // The mark retains their exact count. Compare encoded source characters:
    // PDFium may insert a generated space for a large TJ/cluster advance, even
    // when the saved logical text has no space at that boundary.
    const auto visibleText = [](const QString &value) { return value.trimmed(); };
    QString encodedText = text;
    encodedText.remove(QLatin1Char('\t'));
    if (text.toUtf8() != bytes || text.size() > 32768
        || text.contains(QChar::Null) || text.contains(QLatin1Char('\n'))
        || text.contains(QLatin1Char('\r'))
        || visibleText(encodedText) != visibleText(encodedSourceText)) {
        return;
    }
    float originOffset = 0.0F;
    if (FPDFPageObjMark_GetParamFloatValue(spacingMark, "OriginOffset", &originOffset)
        && (!qIsFinite(originOffset) || qAbs(originOffset) > pageSize.width())) {
        return;
    }
    QVector<CharacterSpan> runs;
    unsigned long runsSize = 0;
    if (FPDFPageObjMark_GetParamBlobValue(spacingMark, "SpacingRuns", nullptr, 0, &runsSize)) {
        if (!runsSize || runsSize > 131072) {
            return;
        }
        QByteArray runBytes(runsSize, Qt::Uninitialized);
        unsigned long actualRunsSize = 0;
        if (!FPDFPageObjMark_GetParamBlobValue(spacingMark, "SpacingRuns",
                reinterpret_cast<unsigned char *>(runBytes.data()), runsSize, &actualRunsSize)
            || actualRunsSize != runsSize) {
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(runBytes);
        const QJsonArray array = document.array();
        if (!document.isArray() || array.isEmpty() || array.size() > 1024) {
            return;
        }
        int end = 0;
        for (const QJsonValue &value : array) {
            const QJsonArray run = value.toArray();
            if (run.size() != (version == 2 ? 5 : 4)
                || !run[0].isDouble() || !run[1].isDouble()
                || !run[2].isDouble() || !run[3].isDouble()
                || run[0].toDouble() != end
                || run[1].toDouble() != run[1].toInt()
                || run[1].toInt() <= 0 || run[1].toInt() > text.size() - end
                || !qIsFinite(run[2].toDouble()) || !qIsFinite(run[3].toDouble())
                || run[2].toDouble() < -10.0 || run[2].toDouble() > 50.0
                || (version == 2 && (!run[4].isDouble() || !qIsFinite(run[4].toDouble())
                    || qAbs(run[4].toDouble()) > 512.0))
                || run[3].toDouble() < -10.0 || run[3].toDouble() > 50.0) {
                return;
            }
            CharacterFormat format;
            format.letterSpacingRatio = run[2].toDouble() / pageSize.height();
            format.wordSpacingRatio = run[3].toDouble() / pageSize.height();
            format.sourceAdvanceRatio = version == 2 ? run[4].toDouble() / pageSize.height() : 0.0;
            runs.append(CharacterSpan{end, run[1].toInt(), format});
            end += run[1].toInt();
        }
        if (end != text.size()) {
            return;
        }
    }
    // Apply only after the complete marked line has passed group validation.
    candidate.spacingGroup = groupId;
    candidate.spacingText = text;
    candidate.spacingIndex = index;
    candidate.spacingCount = parts;
    candidate.spacingStart = start;
    candidate.spacingLetterPoints = letter;
    candidate.spacingWordPoints = word;
    candidate.spacingSourceAdvancePoints = sourceAdvance;
    candidate.spacingOriginOffset = originOffset;
    candidate.spacingRuns = std::move(runs);
    candidate.invalidSpacingMetadata = false;
}

void PDFEditController::readSourceParagraphLayout(
    FPDF_PAGE page, FPDF_PAGEOBJECT object, Candidate &candidate, const QSizeF &pageSize,
    const QString &encodedSourceText)
{
    assertPDFiumProcessLockHeld();
    const int markCount = FPDFPageObj_CountMarks(object);
    if (markCount < 1 || markCount > 64 || pageSize.isEmpty()) {
        return;
    }
    FPDF_PAGEOBJECTMARK found = nullptr;
    for (int index = 0; index < markCount; ++index) {
        const auto mark = FPDFPageObj_GetMark(object, index);
        FPDF_WCHAR name[64]{};
        unsigned long size = 0;
        if (mark && FPDFPageObjMark_GetName(mark, name, sizeof(name), &size)
            && size >= 2 && size <= sizeof(name) && size % 2 == 0
            && QString::fromUtf16(reinterpret_cast<const char16_t *>(name), size / 2 - 1)
                == QLatin1String("TrePDFParagraphLayout")) {
            if (found) {
                return;
            }
            found = mark;
        }
    }
    if (!found) {
        return;
    }
    const auto readBlob = [&](const char *key, unsigned long maximum, QByteArray &value) {
        unsigned long size = 0;
        if (!FPDFPageObjMark_GetParamBlobValue(found, key, nullptr, 0, &size)
            || !size || size > maximum) {
            return false;
        }
        value.resize(size);
        unsigned long actual = 0;
        return FPDFPageObjMark_GetParamBlobValue(found, key,
            reinterpret_cast<unsigned char *>(value.data()), size, &actual) && actual == size;
    };
    int version = 0, index = -1, count = 0, start = -1, length = 0;
    FPDF_WCHAR group[64]{};
    unsigned long groupSize = 0;
    QByteArray bytes, model;
    FS_MATRIX matrix{};
    QPointF anchor;
    if (!FPDFPageObjMark_GetParamIntValue(found, "Version", &version) || version != 1
        || !FPDFPageObjMark_GetParamIntValue(found, "Index", &index)
        || !FPDFPageObjMark_GetParamIntValue(found, "Count", &count)
        || !FPDFPageObjMark_GetParamIntValue(found, "Start", &start)
        || !FPDFPageObjMark_GetParamIntValue(found, "Length", &length)
        || count < 1 || count > 32768 || index < 0 || index >= count
        || start < 0 || start >= 32768 || length < 1 || length > 32768 - start
        || !FPDFPageObjMark_GetParamStringValue(found, "Group", group, sizeof(group), &groupSize)
        || groupSize < 2 || groupSize > sizeof(group) || groupSize % 2 != 0
        || !readBlob("Text", 131072, bytes)
        || (index == 0 && !readBlob("Model", 1024 * 1024, model))
        || !FPDFPageObj_GetMatrix(object, &matrix)
        || !normalizedPagePoint(page, pageSize, QPointF(matrix.e, matrix.f), anchor)) {
        return;
    }
    const QString groupId = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(group), groupSize / 2 - 1);
    const QString text = QString::fromUtf8(bytes);
    QString encoded = text;
    encoded.remove(QLatin1Char('\t'));
    if (QUuid(groupId).isNull() || text.toUtf8() != bytes || text.size() != length
        || text.contains(QChar::Null) || text.contains(QLatin1Char('\n'))
        || text.contains(QLatin1Char('\r'))
        || encoded.simplified() != encodedSourceText.simplified()) {
        return;
    }
    candidate.paragraphLayoutGroup = groupId;
    candidate.paragraphLayoutIndex = index;
    candidate.paragraphLayoutCount = count;
    candidate.paragraphLayoutStart = start;
    candidate.paragraphLayoutLength = length;
    candidate.paragraphLayoutText = text;
    candidate.paragraphLayoutModel = model;
    candidate.paragraphLayoutAnchor = anchor;
}

QVector<PDFEditController::Candidate> PDFEditController::restoreSourceParagraphLayouts(
    QVector<Candidate> candidates, const QSizeF &pageSize) const
{
    if (pageSize.isEmpty()) {
        return candidates;
    }
    QHash<QString, QVector<qsizetype>> groups;
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].paragraphLayoutGroup.isEmpty()) {
            groups[candidates[index].paragraphLayoutGroup].append(index);
        }
    }
    QSet<qsizetype> consumed;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        QVector<qsizetype> indices = it.value();
        std::sort(indices.begin(), indices.end(), [&](qsizetype a, qsizetype b) {
            return candidates[a].paragraphLayoutIndex < candidates[b].paragraphLayoutIndex;
        });
        const Candidate &first = candidates[indices.first()];
        if (indices.size() != first.paragraphLayoutCount || first.paragraphLayoutIndex != 0) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(first.paragraphLayoutModel);
        if (!document.isObject()) {
            continue;
        }
        const QJsonObject model = document.object();
        const QJsonValue physicalSizes = model.value(QStringLiteral("preservePhysicalFontSizes"));
        if (!physicalSizes.isUndefined() && !physicalSizes.isBool())
            continue;
        const QString text = model.value(QStringLiteral("text")).toString();
        const QJsonArray paragraphs = model.value(QStringLiteral("paragraphs")).toArray();
        if (text.isEmpty() || text.size() > 32768 || text.contains(QChar::Null)
            || text.contains(QLatin1Char('\r')) || paragraphs.isEmpty() || paragraphs.size() > 1024) {
            continue;
        }
        const auto finiteArray = [&](const char *key, int count) {
            const QJsonArray array = model.value(QLatin1String(key)).toArray();
            if (array.size() != count) {
                return QJsonArray();
            }
            for (const QJsonValue &value : array) {
                if (!value.isDouble() || !qIsFinite(value.toDouble()) || qAbs(value.toDouble()) > 4.0) {
                    return QJsonArray();
                }
            }
            return array;
        };
        const QJsonArray originOffset = finiteArray("originOffset", 2);
        const QJsonArray frameOffset = finiteArray("frameOffset", 4);
        if (originOffset.isEmpty() || frameOffset.isEmpty()
            || frameOffset[2].toDouble() <= 0.0 || frameOffset[3].toDouble() <= 0.0) {
            continue;
        }
        QVector<ParagraphFormat> formats;
        bool valid = true;
        qsizetype expectedStart = 0;
        for (const QJsonValue &value : paragraphs) {
            const QJsonObject p = value.toObject();
            const auto number = [&](const char *key) {
                const QJsonValue value = p.value(QLatin1String(key));
                return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
            };
            const qsizetype newline = text.indexOf(QLatin1Char('\n'), expectedStart);
            const qsizetype end = newline < 0 ? text.size() : newline;
            const qreal factor = number("lineSpacing");
            const qreal before = number("spaceBeforeRatio");
            const qreal after = number("spaceAfterRatio");
            const qreal indent = p.contains(QStringLiteral("firstLineIndentRatio"))
                ? number("firstLineIndentRatio") : 0.0;
            const QString alignment = p.value(QStringLiteral("alignment")).toString();
            if (expectedStart > text.size() || number("start") != expectedStart
                || number("length") != end - expectedStart
                || !qIsFinite(factor) || factor < 0.5 || factor > 3.0
                || !qIsFinite(before) || before < 0.0 || before * pageSize.height() > 144.001
                || !qIsFinite(after) || after < 0.0 || after * pageSize.height() > 144.001
                || !qIsFinite(indent) || indent < 0.0 || indent * pageSize.height() > 144.001
                || (alignment != QLatin1String("left") && alignment != QLatin1String("center")
                    && alignment != QLatin1String("right") && alignment != QLatin1String("justify"))) {
                valid = false;
                break;
            }
            ParagraphFormat format;
            format.start = expectedStart;
            format.length = end - expectedStart;
            format.lineSpacing = factor;
            format.spaceBeforeRatio = before;
            format.spaceAfterRatio = after;
            format.firstLineIndentRatio = indent;
            format.alignment = alignment == QLatin1String("center") ? CenterAlignment
                : alignment == QLatin1String("right") ? RightAlignment
                : alignment == QLatin1String("justify") ? JustifyAlignment : LeftAlignment;
            formats.append(format);
            expectedStart = end + 1;
        }
        if (!valid || expectedStart != text.size() + 1) {
            continue;
        }
        QVector<qsizetype> sourceBreaks;
        for (const QJsonValue &value : model.value(QStringLiteral("sourceLineBreaks")).toArray()) {
            const int position = value.toInt(-1);
            if (!value.isDouble() || value.toDouble() != position || position < 0
                || position >= text.size() || text[position] != QLatin1Char('\n')
                || (!sourceBreaks.isEmpty() && position <= sourceBreaks.last())) {
                valid = false;
                break;
            }
            sourceBreaks.append(position);
        }
        int previousEnd = 0;
        QRectF bounds;
        for (qsizetype order = 0; valid && order < indices.size(); ++order) {
            const Candidate &part = candidates[indices[order]];
            const int start = part.paragraphLayoutStart;
            if (!part.editable || !part.hasBaseline || part.parts.size() != 1
                || part.invalidSpacingMetadata || !sameHeaderGroup(first, part)
                || part.paragraphLayoutIndex != order || part.paragraphLayoutCount != indices.size()
                || start < previousEnd || start > text.size()
                || part.paragraphLayoutLength > text.size() - start
                || !text.sliced(previousEnd, start - previousEnd).trimmed().isEmpty()
                || text.sliced(start, part.paragraphLayoutLength) != part.paragraphLayoutText) {
                valid = false;
                break;
            }
            previousEnd = start + part.paragraphLayoutLength;
            bounds = bounds.isEmpty() ? part.geometry.sourceRect : bounds.united(part.geometry.sourceRect);
        }
        if (!valid || !text.sliced(previousEnd).trimmed().isEmpty()) {
            continue;
        }
        QVector<CharacterSpan> whitespaceFormats;
        const QJsonValue whitespaceValue = model.value(QStringLiteral("whitespaceFormats"));
        const QJsonArray whitespaceArray = whitespaceValue.toArray();
        if ((!whitespaceValue.isUndefined() && !whitespaceValue.isArray())
            || whitespaceArray.size() > 32768) {
            continue;
        }
        qsizetype whitespaceEnd = 0;
        const auto familyStyleKey = [](const QString &family, bool bold, bool italic) {
            return PDFFontAliases::semanticFamily(family).toCaseFolded()
                + QLatin1Char('|') + QString::number(bold) + QString::number(italic);
        };
        QHash<QString, QString> paragraphDisplayFamilies;
        for (qsizetype index : indices) {
            const Candidate &part = candidates[index];
            paragraphDisplayFamilies.insert(
                familyStyleKey(part.fontFamily, part.fontBold, part.fontItalic), part.fontFamily);
        }
        for (const QJsonValue &value : whitespaceArray) {
            const QJsonObject object = value.toObject();
            const auto number = [&](const char *key) {
                const QJsonValue value = object.value(QLatin1String(key));
                return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
            };
            const int start = object.value(QStringLiteral("start")).toInt(-1);
            const int length = object.value(QStringLiteral("length")).toInt(-1);
            CharacterFormat format;
            format.fontFamily = object.value(QStringLiteral("fontFamily")).toString();
            format.fontSizeRatio = number("fontSizeRatio");
            format.displayFontSizeRatio = number("displayFontSizeRatio");
            format.fontStretch = object.value(QStringLiteral("fontStretch")).toInt(-1);
            format.textShear = object.contains(QStringLiteral("textShear"))
                ? number("textShear") : 0.0;
            format.letterSpacingRatio = number("letterSpacingRatio");
            format.wordSpacingRatio = number("wordSpacingRatio");
            const auto sourceAdvance = object.value(QStringLiteral("sourceAdvanceRatio"));
            format.sourceAdvanceRatio = sourceAdvance.isUndefined() ? 0.0 : number("sourceAdvanceRatio");
            format.fontBold = object.value(QStringLiteral("fontBold")).toBool();
            format.fontItalic = object.value(QStringLiteral("fontItalic")).toBool();
            format.fontUnderline = object.value(QStringLiteral("fontUnderline")).toBool();
            format.color = QColor(object.value(QStringLiteral("color")).toString());
            const auto bounded = [](qreal value, qreal minimum, qreal maximum) {
                return qIsFinite(value) && value >= minimum && value <= maximum;
            };
            if (start < whitespaceEnd || start > text.size() || length < 1
                || length > text.size() - start || number("start") != start
                || number("length") != length || !text.sliced(start, length).trimmed().isEmpty()
                || format.fontFamily.isEmpty() || format.fontFamily.size() > 256
                || format.fontFamily.contains(QChar::Null)
                || !bounded(format.fontSizeRatio, 0.000001, 4.0)
                || !bounded(format.displayFontSizeRatio, 0.0, 4.0)
                || format.fontStretch < QFont::UltraCondensed || format.fontStretch > QFont::UltraExpanded
                || number("fontStretch") != format.fontStretch
                || !bounded(format.textShear, -0.5, 0.5)
                || (format.fontItalic && format.textShear != 0.0)
                || !bounded(format.letterSpacingRatio * pageSize.height(), -10.001, 50.001)
                || !bounded(format.wordSpacingRatio * pageSize.height(), -10.001, 50.001)
                || !bounded(format.sourceAdvanceRatio * pageSize.height(), -512.0, 512.0)
                || !object.value(QStringLiteral("fontBold")).isBool()
                || !object.value(QStringLiteral("fontItalic")).isBool()
                || !object.value(QStringLiteral("fontUnderline")).isBool() || !format.color.isValid()) {
                valid = false;
                break;
            }
            // Metadata stores public names, while the newly reopened PDF's
            // embedded subsets have fresh private display aliases. Recover
            // those aliases for blank-line metrics without persisting them.
            format.fontFamily = paragraphDisplayFamilies.value(
                familyStyleKey(format.fontFamily, format.fontBold, format.fontItalic),
                format.fontFamily);
            whitespaceFormats.append(CharacterSpan{start, length, format});
            whitespaceEnd = start + length;
        }
        if (!valid) {
            continue;
        }
        Candidate restored = first;
        restored.preservePhysicalFontSizes = restored.preservePhysicalFontSizes || physicalSizes.toBool();
        restored.objectPath.clear();
        restored.parts.clear();
        restored.sourceCharacterSpans.clear();
        restored.textGlyphRects.clear();
        restored.text = text;
        restored.sourceParagraphFormats = formats;
        restored.sourceParagraphLineBreaks = sourceBreaks;
        const auto appendSpan = [&](qsizetype start, qsizetype length, const CharacterFormat &format) {
            if (length <= 0) {
                return;
            }
            if (!restored.sourceCharacterSpans.isEmpty()
                && sameCharacterFormat(restored.sourceCharacterSpans.last().format, format)) {
                restored.sourceCharacterSpans.last().length += length;
            } else {
                restored.sourceCharacterSpans.append(CharacterSpan{start, length, format});
            }
        };
        qsizetype whitespaceIndex = 0;
        const auto appendWhitespace = [&](qsizetype start, qsizetype end, const CharacterFormat &fallback) {
            while (start < end && whitespaceIndex < whitespaceFormats.size()) {
                const CharacterSpan &span = whitespaceFormats[whitespaceIndex];
                const qsizetype spanEnd = span.start + span.length;
                if (spanEnd <= start) {
                    ++whitespaceIndex;
                    continue;
                }
                if (span.start >= end) {
                    break;
                }
                const qsizetype overlapStart = qMax(start, span.start);
                const qsizetype overlapEnd = qMin(end, spanEnd);
                appendSpan(start, overlapStart - start, fallback);
                appendSpan(overlapStart, overlapEnd - overlapStart, span.format);
                start = overlapEnd;
                if (spanEnd <= end) {
                    ++whitespaceIndex;
                }
            }
            appendSpan(start, end - start, fallback);
        };
        previousEnd = 0;
        QByteArray identity;
        for (qsizetype index : std::as_const(indices)) {
            const Candidate &part = candidates[index];
            CharacterFormat format = characterFormatForCandidate(part);
            if (!part.spacingGroup.isEmpty()) {
                format.letterSpacingRatio = part.spacingLetterPoints / pageSize.height();
                format.wordSpacingRatio = part.spacingWordPoints / pageSize.height();
                format.sourceAdvanceRatio = part.spacingSourceAdvancePoints / pageSize.height();
            }
            const int start = part.paragraphLayoutStart;
            appendWhitespace(previousEnd, start, format);
            if (part.spacingRuns.isEmpty()) {
                appendWhitespace(start, start + part.paragraphLayoutLength, format);
            } else {
                for (const CharacterSpan &span : part.spacingRuns) {
                    CharacterFormat runFormat = format;
                    runFormat.letterSpacingRatio = span.format.letterSpacingRatio;
                    runFormat.wordSpacingRatio = span.format.wordSpacingRatio;
                    runFormat.sourceAdvanceRatio = span.format.sourceAdvanceRatio;
                    appendWhitespace(start + span.start, start + span.start + span.length, runFormat);
                }
            }
            previousEnd = start + part.paragraphLayoutLength;
            restored.parts.append(part.parts);
            restored.textGlyphRects.append(part.textGlyphRects);
            identity.append(part.objectId.toUtf8());
            identity.append('\0');
        }
        const CharacterFormat trailingFormat = restored.sourceCharacterSpans.last().format;
        appendWhitespace(previousEnd, text.size(), trailingFormat);
        restored.objectId = QStringLiteral("paragraph-layout:") + QString::fromLatin1(
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        restored.origin = first.paragraphLayoutAnchor + QPointF(
            originOffset[0].toDouble(), originOffset[1].toDouble());
        setInitialGeometry(restored, bounds);
        restored.initialDraftFrameRect = QRectF(
            first.paragraphLayoutAnchor + QPointF(frameOffset[0].toDouble(), frameOffset[1].toDouble()),
            QSizeF(frameOffset[2].toDouble(), frameOffset[3].toDouble()));
        restored.paragraphLayoutGroup.clear();
        restored.paragraphLayoutModel.clear();
        restored.paragraphLayoutText.clear();
        restored.spacingGroup.clear();
        restored.spacingText.clear();
        restored.spacingRuns.clear();
        const qsizetype outputIndex = *std::min_element(indices.cbegin(), indices.cend());
        candidates[outputIndex] = std::move(restored);
        for (qsizetype index : std::as_const(indices)) {
            if (index != outputIndex) {
                consumed.insert(index);
            }
        }
    }
    QVector<Candidate> output;
    output.reserve(candidates.size() - consumed.size());
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        if (!consumed.contains(index)) {
            output.append(std::move(candidates[index]));
        }
    }
    return output;
}

QVector<PDFEditController::Candidate> PDFEditController::groupTextLineCandidates(
    QVector<Candidate> candidates,
    const QSizeF &pageSize) const
{
    if (pageSize.isEmpty()) {
        return candidates;
    }

    // Spaced output is a sequence of individually positioned glyph clusters.
    // Its bounded, validated line metadata restores exact text without guessing
    // spaces from ink gaps (negative tracking can legitimately overlap ink).
    QHash<QString, QVector<qsizetype>> spacingGroups;
    QVector<qsizetype> obstacleOrder;
    QVector<QRectF> obstacleRects;
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].spacingGroup.isEmpty()) {
            spacingGroups[candidates[index].spacingGroup].append(index);
        }
    }
    if (!spacingGroups.isEmpty()) {
        obstacleRects.reserve(candidates.size());
        for (qsizetype index = 0; index < candidates.size(); ++index) {
            obstacleRects.append(candidates[index].geometry.sourceRect);
            if (!candidates[index].textMarkupHitTestOnly) {
                obstacleOrder.append(index);
            }
        }
    }
    std::stable_sort(obstacleOrder.begin(), obstacleOrder.end(),
        [&](qsizetype left, qsizetype right) {
            return obstacleRects[left].top() < obstacleRects[right].top();
        });
    QVector<qreal> maximumBottom;
    maximumBottom.reserve(obstacleOrder.size());
    qreal bottom = -std::numeric_limits<qreal>::infinity();
    for (qsizetype index : std::as_const(obstacleOrder)) {
        bottom = qMax(bottom, obstacleRects[index].bottom());
        maximumBottom.append(bottom);
    }
    QSet<qsizetype> consumed;
    qsizetype metadataComparisons = 0;
    for (auto it = spacingGroups.cbegin(); it != spacingGroups.cend(); ++it) {
        QVector<qsizetype> indices = it.value();
        if (indices.isEmpty() || indices.size() > 1024) {
            continue;
        }
        std::sort(indices.begin(), indices.end(), [&](qsizetype a, qsizetype b) {
            return candidates[a].spacingIndex < candidates[b].spacingIndex;
        });
        const Candidate &first = candidates[indices.first()];
        if (indices.size() != first.spacingCount) {
            continue;
        }
        int length = 0;
        QRectF rect;
        bool valid = true;
        for (qsizetype offset = 0; offset < indices.size(); ++offset) {
            const Candidate &part = candidates[indices[offset]];
            const Candidate &previous = candidates[indices[qMax<qsizetype>(0, offset - 1)]];
            // Qt permits negative advances with tight tracking. Bound any
            // backward step by the preceding cluster's declared spacing;
            // ordinary unmarked overprints still follow the overlap guard.
            const qreal backwardAllowance = qMax(0.0, -previous.spacingLetterPoints)
                * previous.spacingText.size()
                + qMax(0.0, -previous.spacingWordPoints)
                    * previous.spacingText.count(QLatin1Char(' ')) + 0.02;
            if (!part.editable || !part.hasBaseline || !part.renderSuppressible
                || !sameHeaderGroup(first, part)
                || part.kind != QLatin1String("text") || part.parts.size() != 1
                || part.spacingIndex != offset || part.spacingCount != indices.size()
                || part.spacingStart != length || part.spacingText.isEmpty()
                || qAbs(part.origin.y() - first.origin.y()) * pageSize.height() > 0.1
                || (offset > 0 && part.origin.x() + backwardAllowance / pageSize.width()
                    < previous.origin.x())) {
                valid = false;
                break;
            }
            length += part.spacingText.size();
            rect = rect.isEmpty() ? part.geometry.sourceRect
                : rect.united(part.geometry.sourceRect);
        }
        if (!valid || length > 32768) {
            continue;
        }
        const QSet<qsizetype> members(indices.cbegin(), indices.cend());
        // Query only overlapping vertical intervals. A spaced page can have
        // thousands of clusters; unrelated rows must not exhaust the budget.
        const qsizetype firstObstacle = std::upper_bound(
            maximumBottom.cbegin(), maximumBottom.cend(), rect.top())
            - maximumBottom.cbegin();
        for (qsizetype position = firstObstacle; position < obstacleOrder.size(); ++position) {
            const qsizetype index = obstacleOrder[position];
            if (obstacleRects[index].top() >= rect.bottom()) {
                break;
            }
            if (++metadataComparisons > kMaximumTextGroupingComparisons) {
                valid = false;
                break;
            }
            if (!members.contains(index) && !consumed.contains(index)
                && !candidates[index].textMarkupHitTestOnly
                && obstacleRects[index].intersects(rect)) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            continue;
        }
        Candidate grouped = first;
        grouped.objectPath.clear();
        grouped.parts.clear();
        grouped.text.clear();
        grouped.sourceCharacterSpans.clear();
        grouped.textGlyphRects.clear();
        grouped.letterSpacingRatio = first.spacingLetterPoints / pageSize.height();
        grouped.wordSpacingRatio = first.spacingWordPoints / pageSize.height();
        QByteArray identity;
        for (qsizetype index : std::as_const(indices)) {
            Candidate part = candidates[index];
            part.letterSpacingRatio = part.spacingLetterPoints / pageSize.height();
            part.wordSpacingRatio = part.spacingWordPoints / pageSize.height();
            CharacterFormat baseFormat = characterFormatForCandidate(part);
            baseFormat.sourceAdvanceRatio = part.spacingSourceAdvancePoints / pageSize.height();
            QVector<CharacterSpan> runs = part.spacingRuns;
            if (runs.isEmpty()) {
                runs.append(CharacterSpan{0, part.spacingText.size(), baseFormat});
            }
            for (const CharacterSpan &run : std::as_const(runs)) {
                CharacterFormat format = baseFormat;
                format.letterSpacingRatio = run.format.letterSpacingRatio;
                format.wordSpacingRatio = run.format.wordSpacingRatio;
                format.sourceAdvanceRatio = run.format.sourceAdvanceRatio;
                if (!grouped.sourceCharacterSpans.isEmpty()
                    && sameCharacterFormat(grouped.sourceCharacterSpans.last().format, format)) {
                    grouped.sourceCharacterSpans.last().length += run.length;
                } else {
                    grouped.sourceCharacterSpans.append(CharacterSpan{
                        grouped.text.size() + run.start, run.length, format});
                }
            }
            grouped.text.append(part.spacingText);
            grouped.parts.append(part.parts);
            grouped.textGlyphRects.append(part.textGlyphRects);
            identity.append(part.objectId.toUtf8());
            identity.append('\0');
        }
        grouped.spacingGroup.clear();
        grouped.spacingText.clear();
        grouped.spacingRuns.clear();
        grouped.origin.rx() -= first.spacingOriginOffset / pageSize.width();
        grouped.objectId = QStringLiteral("line:") + QString::fromLatin1(
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        setInitialGeometry(grouped, rect);
        expandInitialTextFrame(grouped, pageSize);
        const qsizetype outputIndex = *std::min_element(indices.cbegin(), indices.cend());
        candidates[outputIndex] = std::move(grouped);
        for (qsizetype index : std::as_const(indices)) {
            if (index != outputIndex) {
                consumed.insert(index);
            }
        }
    }
    if (!consumed.isEmpty()) {
        QVector<Candidate> restored;
        restored.reserve(candidates.size() - consumed.size());
        for (qsizetype index = 0; index < candidates.size(); ++index) {
            if (!consumed.contains(index)) {
                restored.append(std::move(candidates[index]));
            }
        }
        candidates = std::move(restored);
    }
    if (candidates.size() < 2) {
        return candidates;
    }

    // Older Header saves emitted separate glyphs without paragraph metadata.
    // Their common UUID permits small ink overlaps, but never duplicate origins
    // or arbitrary overprints. Keep the normal grouping/obstacle budgets below.
    const auto legacyHeaderText = [&](const Candidate &candidate) {
        const QRectF rect = candidate.geometry.sourceRect;
        return candidate.kind == QLatin1String("text") && candidate.editable
            && candidate.renderSuppressible && candidate.hasBaseline
            && !candidate.headerGroupId.isEmpty() && !candidate.headerMetadataInvalid
            && !containsShearedText(candidate)
            && candidate.headerRole == QLatin1String("text")
            && !candidate.invalidSpacingMetadata && candidate.spacingGroup.isEmpty()
            && candidate.paragraphLayoutGroup.isEmpty()
            && candidate.sourceParagraphFormats.isEmpty()
            && rect.left() >= 0.0 && rect.right() <= 1.0 && rect.top() >= 0.0
            && rect.bottom() * pageSize.height() <= 100.0;
    };
    const auto legacyHeaderNeighbors = [&](const Candidate &left, const Candidate &right) {
        const qreal em = qMax(left.fontSizeRatio, right.fontSizeRatio) * pageSize.height();
        const qreal gap = (right.geometry.sourceRect.left() - left.geometry.sourceRect.right())
            * pageSize.width();
        return legacyHeaderText(left) && legacyHeaderText(right) && sameHeaderGroup(left, right)
            && qAbs(left.origin.y() - right.origin.y()) * pageSize.height() <= 0.1
            && (right.origin.x() - left.origin.x()) * pageSize.width() > 0.1
            && gap >= -0.25 * em;
    };

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
                if (legacyHeaderNeighbors(left, right) || legacyHeaderNeighbors(right, left))
                    continue;
                overlappingText.insert(left.objectId);
                overlappingText.insert(right.objectId);
            }
        }
    }

    // Snapshot source obstacles before moving candidates into visual lines.
    obstacleOrder.clear();
    obstacleRects.clear();
    maximumBottom.clear();
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        const Candidate &candidate = candidates[index];
        obstacleRects.append(candidate.geometry.sourceRect);
        if (!candidate.textMarkupHitTestOnly
            && (!candidate.objectPath.isEmpty() || !candidate.parts.isEmpty())) {
            obstacleOrder.append(index);
        }
    }
    std::stable_sort(obstacleOrder.begin(), obstacleOrder.end(),
        [&](qsizetype left, qsizetype right) {
            return obstacleRects[left].top() < obstacleRects[right].top();
        });
    bottom = -std::numeric_limits<qreal>::infinity();
    for (qsizetype index : std::as_const(obstacleOrder)) {
        bottom = qMax(bottom, obstacleRects[index].bottom());
        maximumBottom.append(bottom);
    }
    const auto gapObstructed = [&](qsizetype left, qsizetype right) {
        const QRectF &leftRect = obstacleRects[left];
        const QRectF &rightRect = obstacleRects[right];
        const QRectF gap(QPointF(leftRect.right(), qMin(leftRect.top(), rightRect.top())),
                         QPointF(rightRect.left(), qMax(leftRect.bottom(), rightRect.bottom())));
        if (gap.isEmpty()) {
            return false;
        }
        const qsizetype first = std::upper_bound(
            maximumBottom.cbegin(), maximumBottom.cend(), gap.top())
            - maximumBottom.cbegin();
        for (qsizetype position = first; position < obstacleOrder.size(); ++position) {
            const qsizetype index = obstacleOrder[position];
            const QRectF &rect = obstacleRects[index];
            if (rect.top() >= gap.bottom()) {
                break;
            }
            if (++comparisons > kMaximumTextGroupingComparisons) {
                return true;
            }
            if (index != left && index != right && gap.intersects(rect)) {
                return true;
            }
        }
        return false;
    };

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
            && !containsShearedText(candidate)
            && !overlappingText.contains(candidate.objectId)
            && candidate.parts.size() == 1
            && candidate.sourceParagraphFormats.isEmpty()
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

    const auto compatibleLineStyle = [&](const Candidate &left, const Candidate &right) {
        const qreal leftSize = emSize(left);
        const qreal rightSize = emSize(right);
        const qreal sizeTolerance = qMax(
            kLineFontSizeTolerancePoints,
            qMax(leftSize, rightSize) * kLineFontSizeToleranceRatio);
        // Typeface, emphasis and calibrated horizontal stretch belong to each
        // character span. They do not define a new visual line. Keep paint and
        // size checks because replacement still shares the source paint mode.
        return sameHeaderGroup(left, right)
            && !left.fontFamily.isEmpty() && !right.fontFamily.isEmpty()
            && qAbs(leftSize - rightSize) <= sizeTolerance
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
                                     const Candidate *followingLine = nullptr,
                                     const Candidate *followingWord = nullptr) {
        if (followingLine) {
            // QTextDocument reports a paragraph separator using the following
            // block's initial character format. Seed inferred breaks the same
            // way so entering/leaving the editor does not create a style edit.
            CharacterFormat format = followingLine->sourceCharacterSpans.isEmpty()
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
        } else if (!grouped.sourceCharacterSpans.isEmpty() && !separator.isEmpty()) {
            CharacterFormat format = grouped.sourceCharacterSpans.last().format;
            format.sourceAdvanceRatio = 0.0;
            const auto supportsSeparator = [&](const QString &family) {
                QFont font(family);
                font.setBold(format.fontBold);
                font.setItalic(format.fontItalic);
                font.setStyleStrategy(QFont::NoFontMerging);
                const QRawFont raw = QRawFont::fromFont(font);
                return raw.isValid() && raw.familyName() == family
                    && (raw.weight() >= QFont::Bold) == format.fontBold
                    && (raw.style() != QFont::StyleNormal) == format.fontItalic
                    && raw.supportsCharacter(U' ');
            };
            if (separator == QLatin1String(" ") && !supportsSeparator(format.fontFamily)) {
                // A subset can contain both words without a mapped space.
                // Give only the inferred separator a complete local face;
                // implicit Qt fallback can otherwise merge its cluster with
                // the following word and lose that word's glyph boundaries.
                for (const QString &family : {
                         followingWord ? followingWord->fontFamily : QString(),
                         followingWord ? PDFFontAliases::semanticFamily(followingWord->fontFamily) : QString(),
                         PDFFontAliases::semanticFamily(format.fontFamily),
                         QStringLiteral("Arial")}) {
                    if (!family.isEmpty() && supportsSeparator(family)) {
                        format.fontFamily = family;
                        break;
                    }
                }
            }
            grouped.sourceCharacterSpans.append({grouped.text.size(), separator.size(), format});
        }
        grouped.text.append(separator);
    };
    const auto needsOriginPreservation = [](const Candidate &candidate) {
        if (std::any_of(candidate.sourceCharacterSpans.cbegin(),
            candidate.sourceCharacterSpans.cend(), [](const CharacterSpan &span) {
                return !qFuzzyIsNull(span.format.sourceAdvanceRatio);
            }))
            return true;
        // Separate Tm/Tj objects also carry source positioning, even without
        // an irregular advance inside either object. Apply the same bounded
        // join validation to horizontal Latin/Common text.
        const auto characters = candidate.text.toUcs4();
        return !characters.isEmpty()
            && std::all_of(characters.cbegin(), characters.cend(), [](char32_t c) {
                const auto script = QChar::script(c);
                return (script == QChar::Script_Latin || script == QChar::Script_Common)
                    && QChar::isPrint(c) && !QChar::isMark(c)
                    && (!QChar::isSpace(c) || c == U' ');
            });
    };
    const auto preserveJoinedOrigins = [&](Candidate &grouped,
                                           const QVector<IndexedCandidate> &sources,
                                           const QVector<qsizetype> &starts) {
        if (grouped.text.size() > 1024)
            return false;
        struct Glyph {
            quint32 index = 0;
            QRawFont font;
            QPointF position;
        };
        struct Measurement {
            QVector<qreal> cursors;
            QHash<qsizetype, Glyph> glyphs;
        };
        const auto measure = [&](const Candidate &candidate, Measurement &result) {
            const auto face = [&](const CharacterFormat &format) {
                const qreal size = format.displayFontSizeRatio > 0.0
                    ? format.displayFontSizeRatio : format.fontSizeRatio;
                return fontForDisplay(format.fontFamily, format.fontStretch,
                    format.fontBold, format.fontItalic, format.fontUnderline,
                    size * pageSize.height(),
                    (format.letterSpacingRatio + format.sourceAdvanceRatio) * pageSize.height(),
                    format.wordSpacingRatio * pageSize.height(),
                    !qFuzzyIsNull(format.sourceAdvanceRatio));
            };
            QTextLayout layout(candidate.text, face(characterFormatForCandidate(candidate)));
            QList<QTextLayout::FormatRange> formats;
            for (const CharacterSpan &span : candidate.sourceCharacterSpans) {
                QTextCharFormat format;
                format.setFont(face(span.format));
                formats.append({int(span.start), int(span.length), format});
            }
            layout.setFormats(formats);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                layout.endLayout();
                return false;
            }
            line.setLineWidth(1000000.0);
            layout.endLayout();
            result = {};
            for (qsizetype position = 0; position <= candidate.text.size(); ++position)
                result.cursors.append(line.cursorToX(int(position)));
            const auto runs = line.glyphRuns(0, candidate.text.size(),
                QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions
                    | QTextLayout::RetrieveStringIndexes);
            for (const QGlyphRun &run : runs) {
                const auto indexes = run.stringIndexes();
                const auto glyphs = run.glyphIndexes();
                const auto positions = run.positions();
                if (run.isRightToLeft() || indexes.size() != glyphs.size()
                    || indexes.size() != positions.size())
                    return false;
                for (qsizetype i = 0; i < indexes.size(); ++i) {
                    // Multiple glyphs in one shaping cluster need a richer
                    // boundary model. Keep those source objects independent.
                    if (result.glyphs.contains(indexes[i]))
                        return false;
                    result.glyphs.insert(indexes[i], Glyph{glyphs[i], run.rawFont(),
                        positions[i] - QPointF(0.0, line.ascent())});
                }
            }
            return true;
        };
        QHash<qsizetype, Glyph> expected;
        QVector<qreal> desiredOrigins;
        for (qsizetype i = 0; i < sources.size(); ++i) {
            const Candidate &source = sources[i].candidate;
            if (qAbs(source.origin.y() - grouped.origin.y()) * pageSize.height() > 0.05)
                return false;
            Measurement local;
            if (!measure(source, local))
                return false;
            const qreal offset = (source.origin.x() - grouped.origin.x()) * pageSize.width();
            desiredOrigins.append(offset);
            for (auto glyph = local.glyphs.cbegin(); glyph != local.glyphs.cend(); ++glyph) {
                Glyph positioned = glyph.value();
                positioned.position.rx() += offset;
                expected.insert(starts[i] + glyph.key(), positioned);
            }
        }
        Measurement combined;
        if (!measure(grouped, combined))
            return false;
        QVector<qsizetype> precedingClusters;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, grouped.text);
        for (qsizetype i = 1; i < starts.size(); ++i) {
            finder.setPosition(int(starts[i]));
            if (!finder.isAtBoundary())
                return false;
            const qsizetype previous = finder.toPreviousBoundary();
            if (previous < 0 || !combined.glyphs.contains(previous))
                return false;
            precedingClusters.append(previous);
        }
        // Change only the advance immediately before each original object.
        // Subtract the preceding boundary's residual because its correction
        // also translates every subsequent object in this line.
        for (int pass = 0; pass < 3; ++pass) {
            qreal previousError = 0.0;
            for (qsizetype i = 1; i < starts.size(); ++i) {
                const qreal error = desiredOrigins[i] - combined.cursors[starts[i]];
                const qreal delta = error - previousError;
                previousError = error;
                if (qAbs(delta) < 0.001)
                    continue;
                const qsizetype start = precedingClusters[i - 1];
                const qsizetype end = starts[i];
                QVector<CharacterSpan> adjusted;
                for (const CharacterSpan &span : std::as_const(grouped.sourceCharacterSpans)) {
                    const qsizetype overlapStart = qMax(start, span.start);
                    const qsizetype overlapEnd = qMin(end, span.start + span.length);
                    if (overlapStart >= overlapEnd) {
                        adjusted.append(span);
                        continue;
                    }
                    if (span.start < overlapStart)
                        adjusted.append({span.start, overlapStart - span.start, span.format});
                    CharacterFormat format = span.format;
                    format.sourceAdvanceRatio += delta / pageSize.height();
                    if (!qIsFinite(format.sourceAdvanceRatio)
                        || qAbs(format.sourceAdvanceRatio * pageSize.height()) > 512.0)
                        return false;
                    adjusted.append({overlapStart, overlapEnd - overlapStart, format});
                    if (overlapEnd < span.start + span.length)
                        adjusted.append({overlapEnd, span.start + span.length - overlapEnd, span.format});
                }
                grouped.sourceCharacterSpans = std::move(adjusted);
            }
            if (!measure(grouped, combined))
                return false;
        }
        for (auto glyph = expected.cbegin(); glyph != expected.cend(); ++glyph) {
            const auto actual = combined.glyphs.constFind(glyph.key());
            if (actual == combined.glyphs.cend() || actual->index != glyph->index
                || actual->font != glyph->font
                || qAbs(actual->position.x() - glyph->position.x()) > 0.08
                || qAbs(actual->position.y() - glyph->position.y()) > 0.08)
                return false;
        }
        return true;
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
        qsizetype runCharacters = 0;
        bool runPreservesOrigins = false;
        const auto flushRun = [&]() {
            runCharacters = 0;
            const bool preserveOrigins = std::exchange(runPreservesOrigins, false);
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
            QVector<qsizetype> sourceStarts;

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
                    appendSeparator(grouped, separator, nullptr, &source);
                }

                CandidatePart part = source.parts.constFirst();
                part.separatorAfter.clear();
                grouped.parts.append(std::move(part));
                sourceStarts.append(grouped.text.size());
                appendSourceText(grouped, source);
                grouped.textGlyphRects.append(source.textGlyphRects);
                identity.append(source.objectId.toUtf8());
                identity.append('\0');
            }

            if (preserveOrigins && !preserveJoinedOrigins(grouped, run, sourceStarts)) {
                for (IndexedCandidate &source : run)
                    output.append(std::move(source));
                run.clear();
                return;
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
            const bool preserveOrigins = needsOriginPreservation(candidate.candidate);
            if (!run.isEmpty()) {
                const Candidate &previous = run.constLast().candidate;
                if (!compatibleLineStyle(run.constFirst().candidate, candidate.candidate)
                    || !areNeighbors(previous, candidate.candidate)
                    || run.size() >= kMaximumParagraphParts
                    || runCharacters + 1 + candidate.candidate.text.size()
                        > kMaximumParagraphCharacters
                    || ((runPreservesOrigins || preserveOrigins)
                        && runCharacters + 1 + candidate.candidate.text.size() > 1024)
                    || gapObstructed(run.constLast().order, candidate.order)) {
                    flushRun();
                }
            }
            runCharacters += candidate.candidate.text.size() + (run.isEmpty() ? 0 : 1);
            runPreservesOrigins = runPreservesOrigins || preserveOrigins;
            run.append(std::move(candidate));
        }
        flushRun();
    }

    // Work from complete visual lines, then link only unambiguous neighbors
    // within one column. All paths remain attached to the resulting paragraph
    // so suppression, deletion, movement and Save share the existing pipeline.
    const auto hasJoinableParagraphLayout = [&](const Candidate &candidate) {
        if (candidate.sourceParagraphFormats.isEmpty()) {
            return true;
        }
        // A previously edited single line carries paragraph metadata too. It
        // can still continue a neighboring source line, provided joining does
        // not discard explicit paragraph formatting or a wrapped layout.
        if (candidate.sourceParagraphFormats.size() != 1
            || !candidate.sourceParagraphLineBreaks.isEmpty()) {
            return false;
        }
        const ParagraphFormat &format = candidate.sourceParagraphFormats.first();
        if (format.start != 0 || format.length != candidate.text.size()
            || format.alignment != LeftAlignment
            || !qFuzzyCompare(format.lineSpacing, 1.0)
            || !qFuzzyIsNull(format.spaceBeforeRatio)
            || !qFuzzyIsNull(format.spaceAfterRatio)
            || !qFuzzyIsNull(format.firstLineIndentRatio)
            || format.listStyle != ParagraphListStyle::None) {
            return false;
        }
        return std::all_of(candidate.parts.cbegin(), candidate.parts.cend(),
            [&](const CandidatePart &part) {
                return part.hasBaseline
                    && qAbs(part.origin.y() - candidate.origin.y()) * pageSize.height() <= 0.1;
            });
    };
    const auto canJoinParagraph = [&](const Candidate &candidate) {
        return candidate.kind == QLatin1String("text")
            && candidate.editable && candidate.renderSuppressible
            && candidate.hasBaseline && !candidate.added
            && !candidate.parts.isEmpty()
            && !containsShearedText(candidate)
            && hasJoinableParagraphLayout(candidate)
            && !overlappingText.contains(candidate.objectId)
            && !candidate.text.contains(QLatin1Char('\n'))
            && !candidate.text.contains(QLatin1Char('\r'))
            && emSize(candidate) > 0.0;
    };
    const auto paragraphStyleMatches = [&](const Candidate &left, const Candidate &right) {
        // A paragraph can change emphasis without changing its typeface.
        // Preserve each line's format instead of applying the first line's
        // bold/italic/underline and calibrated stretch to the entire paragraph.
        // TeX can expand/contract each justified source row by up to 2%.
        // Compare those rows across the full 98-102% band while retaining
        // their individual spans; larger source scales still need to agree.
        const bool compatibleStretch = qAbs(left.fontStretch - right.fontStretch) <= 2
            || (qAbs(left.fontStretch - 100) <= 2 && qAbs(right.fontStretch - 100) <= 2);
        return sameHeaderGroup(left, right) && !left.fontFamily.isEmpty()
            && sameFontFamily(PDFFontAliases::semanticFamily(left.fontFamily),
                              PDFFontAliases::semanticFamily(right.fontFamily))
            && qAbs(emSize(left) - emSize(right))
                <= qMax<qreal>(0.25, 0.02 * qMax(emSize(left), emSize(right)))
            && (compatibleStretch || (legacyHeaderText(left) && legacyHeaderText(right)))
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
    const auto startsIndentedParagraph = [&](const Candidate &upper, const Candidate &lower) {
        const qreal em = qMax(emSize(upper), emSize(lower));
        const qreal indent = (upper.origin.x() - lower.origin.x()) * pageSize.width();
        // Only a first row may step OUT toward the body margin. The reverse
        // direction starts a new paragraph. Reject short inset headings and
        // large offsets that are more likely separate columns or list layouts.
        return indent >= 0.75 * em && indent <= 2.5 * em && indent <= 144.0
            && upper.geometry.sourceRect.width() >= 0.65 * lower.geometry.sourceRect.width()
            && (lower.geometry.sourceRect.right() - upper.geometry.sourceRect.right())
                * pageSize.width() <= 2.0 * em;
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
                || (!alignedLeft(upper, lower) && !startsIndentedParagraph(upper, lower))
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
                const bool indented = run.size() > 1
                    && startsIndentedParagraph(first, output[run[1]].candidate);
                const Candidate &body = indented ? output[run[1]].candidate : first;
                const bool aligned = run.size() == 1
                    ? alignedLeft(first, source) || startsIndentedParagraph(first, source)
                    : alignedLeft(body, source);
                if (!aligned || !paragraphStyleMatches(first, source)
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
            // Source baselines are independent of the font's default leading.
            // Measure the same point-sized, mixed-format lines as the editor
            // and writer before translating those gaps into paragraph factors.
            QVector<QPair<qreal, qreal>> lineMetrics;
            for (qsizetype member : std::as_const(run)) {
                const Candidate &source = output[member].candidate;
                const auto face = [&](const CharacterFormat &format) {
                    const qreal ratio = format.displayFontSizeRatio > 0.0
                        ? format.displayFontSizeRatio : format.fontSizeRatio;
                    return fontForDisplay(format.fontFamily, format.fontStretch,
                        format.fontBold, format.fontItalic, format.fontUnderline,
                        ratio * pageSize.height());
                };
                QTextLayout layout(source.text, face(characterFormatForCandidate(source)));
                QList<QTextLayout::FormatRange> formats;
                for (const CharacterSpan &span : source.sourceCharacterSpans) {
                    QTextCharFormat format;
                    format.setFont(face(span.format));
                    formats.append({int(span.start), int(span.length), format});
                }
                layout.setFormats(formats);
                layout.beginLayout();
                QTextLine line = layout.createLine();
                if (!line.isValid()) {
                    layout.endLayout();
                    break;
                }
                line.setLineWidth(1000000.0);
                lineMetrics.append({line.height(), line.ascent()});
                layout.endLayout();
            }
            if (lineMetrics.size() != run.size()) {
                continue;
            }
            QVector<qreal> lineSpacing;
            qreal previousTop = 0.0;
            for (qsizetype line = 0; line + 1 < run.size(); ++line) {
                const qreal sourceOffset = (output[run[line + 1]].candidate.origin.y()
                    - first.origin.y()) * pageSize.height();
                // QTextDocument advances in 26.6 fixed-point units. Quantize
                // absolute tops so rounding does not accumulate over 64 rows.
                const qreal nextTop = qRound64((sourceOffset + lineMetrics.first().second
                    - lineMetrics[line + 1].second) * 64.0) / 64.0;
                const qreal advance = nextTop - previousTop;
                qreal factor = advance / lineMetrics[line].first;
                if (std::floor(lineMetrics[line].first * factor * 64.0)
                    < qRound64(advance * 64.0)) {
                    factor = std::nextafter(factor, std::numeric_limits<qreal>::infinity());
                }
                if (!qIsFinite(factor) || factor < 0.5 || factor > 3.0) {
                    break;
                }
                lineSpacing.append(factor);
                previousTop = nextTop;
            }
            if (lineSpacing.size() + 1 != run.size()) {
                continue;
            }
            lineSpacing.append(lineSpacing.constLast());
            Candidate grouped = first;
            const Candidate &body = output[run[1]].candidate;
            const qreal firstIndent = startsIndentedParagraph(first, body)
                ? (first.origin.x() - body.origin.x()) * pageSize.width() / pageSize.height()
                : 0.0;
            if (firstIndent > 0.0) {
                grouped.origin.setX(body.origin.x());
            }
            grouped.objectPath.clear();
            grouped.parts.clear();
            grouped.text.clear();
            grouped.sourceCharacterSpans.clear();
            grouped.textGlyphRects.clear();
            grouped.sourceParagraphFormats.clear();
            grouped.sourceParagraphLineBreaks.clear();
            QRectF sourceRect = first.geometry.sourceRect;
            qsizetype firstOrder = output[index].order;
            QByteArray identity;
            for (qsizetype member : std::as_const(run)) {
                const Candidate &source = output[member].candidate;
                if (!grouped.parts.isEmpty()) {
                    grouped.parts.last().separatorAfter = QStringLiteral("\n");
                    grouped.sourceParagraphLineBreaks.append(grouped.text.size());
                    appendSeparator(grouped, QStringLiteral("\n"), &source);
                }
                ParagraphFormat paragraph;
                paragraph.start = grouped.text.size();
                paragraph.length = source.text.size();
                paragraph.lineSpacing = lineSpacing[grouped.sourceParagraphFormats.size()];
                if (grouped.sourceParagraphFormats.isEmpty()) {
                    paragraph.firstLineIndentRatio = firstIndent;
                }
                grouped.sourceParagraphFormats.append(paragraph);
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
