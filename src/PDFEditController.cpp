#include "PDFEditController.h"
#include "ApplicationFonts.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QRunnable>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

namespace {

constexpr unsigned long kModifyContentsPermission = 1UL << 3;
constexpr int kCoordinateMapExtent = 100000;
constexpr qreal kAddedTextWidthRatio = 0.25;
constexpr qreal kAddedTextMinimumWidthRatio = 0.02;
constexpr qreal kAddedTextHeightRatio = 0.04;
constexpr qreal kAddedTextFontSizeRatio = 0.022;
constexpr qreal kAddedImageMaximumDimensionRatio = 0.25;
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
constexpr FS_MATRIX kIdentityMatrix{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};

bool isTextCandidateKind(const QString &kind)
{
    return kind == QLatin1String("text")
        || kind == QLatin1String("addedText");
}

bool draftRepresentsDeletion(const QString &kind, const QString &text)
{
    return isTextCandidateKind(kind) && text.isEmpty();
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
        //% "An original object could not be replaced on page %1."
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
    case PDFSaveWriter::Error::PageContentGenerationFailed:
        //% "Page %1 could not be finalized for saving."
        return qtTrId("PdfSavePageGenerationFailed").arg(result.page + 1);
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
{
}

PDFEditController::~PDFEditController()
{
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
        connect(m_document, &PDFDocument::pageCountChanged, this, &PDFEditController::resetSession);
        connect(m_document, &QObject::destroyed, this, [this]() {
            m_document = nullptr;
            resetSession();
            emit documentChanged();
        });
    }

    resetSession();
    emit documentChanged();
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
    if (m_tool == BrowseTool) {
        clearSelection();
    }
    emit toolChanged();
}

bool PDFEditController::hasDrafts() const
{
    return !m_drafts.isEmpty();
}

bool PDFEditController::canUndo() const
{
    return !m_undoStack.isEmpty();
}

bool PDFEditController::canRedo() const
{
    return !m_redoStack.isEmpty();
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
        ? primaryCharacterFormat(selectedDraftForDisplay()).fontFamily
        : QString();
}

qreal PDFEditController::selectedFontSizeRatio() const
{
    return m_hasSelection
        ? primaryCharacterFormat(selectedDraftForDisplay()).fontSizeRatio
        : 0.0;
}

qreal PDFEditController::selectedDisplayFontSizeRatio() const
{
    if (!m_hasSelection) {
        return 0.0;
    }
    const CharacterFormat format = primaryCharacterFormat(selectedDraftForDisplay());
    return format.displayFontSizeRatio > 0.0
        ? format.displayFontSizeRatio
        : format.fontSizeRatio;
}

int PDFEditController::selectedFontStretch() const
{
    return m_hasSelection
        ? primaryCharacterFormat(selectedDraftForDisplay()).fontStretch
        : QFont::Unstretched;
}

bool PDFEditController::selectedFontBold() const
{
    return m_hasSelection && primaryCharacterFormat(selectedDraftForDisplay()).fontBold;
}

bool PDFEditController::selectedFontItalic() const
{
    return m_hasSelection && primaryCharacterFormat(selectedDraftForDisplay()).fontItalic;
}

bool PDFEditController::selectedFontUnderline() const
{
    return m_hasSelection && primaryCharacterFormat(selectedDraftForDisplay()).fontUnderline;
}

QColor PDFEditController::selectedColor() const
{
    return m_hasSelection
        ? primaryCharacterFormat(selectedDraftForDisplay()).color
        : QColor(Qt::black);
}

bool PDFEditController::selectedEditable() const
{
    return m_hasSelection && m_selectedCandidate.editable;
}

QString PDFEditController::selectedReason() const
{
    return m_hasSelection ? m_selectedCandidate.reason : QString();
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
            && candidate.kind == QLatin1String("text")
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

bool PDFEditController::allDraftedTextObjectsSuppressibleForPage(int page) const
{
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && candidate.kind == QLatin1String("text")
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
        const QString key = draftKey(candidate.page, candidate.objectId);
        const auto draft = m_drafts.constFind(key);
        if (draft != m_drafts.cend()
            && draftRepresentsDeletion(candidate.kind, draft->text)) {
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
            && draftRepresentsDeletion(candidate.kind, draft->text)) {
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
    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    const QPointF point(x, y);
    const Candidate *bestCandidate = nullptr;
    qreal bestArea = 0.0;

    const auto considerCandidate = [&](const Candidate &candidate) {
        const auto draft = m_drafts.constFind(
            draftKey(candidate.page, candidate.objectId));
        if (draft != m_drafts.cend()
            && draftRepresentsDeletion(candidate.kind, draft->text)) {
            return;
        }

        const QRectF &sourceRect = candidate.geometry.sourceRect;
        const QRectF hitRect = sourceRect.adjusted(-0.0025, -0.0025, 0.0025, 0.0025);
        if (!hitRect.contains(point)) {
            return;
        }

        const qreal area = sourceRect.width() * sourceRect.height();
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

    for (auto candidate = candidates.crbegin(); candidate != candidates.crend(); ++candidate) {
        considerCandidate(*candidate);
    }
    for (auto candidate = m_addedCandidates.crbegin();
         candidate != m_addedCandidates.crend();
         ++candidate) {
        if (candidate->page == page) {
            considerCandidate(*candidate);
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

    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
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

QString PDFEditController::resolvedFontFamily(FPDF_PAGEOBJECT textObject,
                                              const QString &text)
{
    FPDF_FONT font = FPDFTextObj_GetFont(textObject);
    if (!font) {
        return {};
    }

    const QString expectedFamily = pdfFontName(font, false);
    const QString baseName = pdfFontName(font, true);
    const QString cacheKey = (baseName.isEmpty() ? expectedFamily : baseName)
        + QLatin1Char('#')
        + QString::number(
            static_cast<qulonglong>(reinterpret_cast<quintptr>(font)),
            16);
    const QString bundledFamily = matchingFontFamily(
        font,
        ApplicationFonts::bundledFontFamilies());
    const QString systemFamily = matchingFontFamily(
        font,
        ApplicationFonts::systemFontFamilies());
    QString embeddedFamily;
    const auto cachedEmbeddedFamily = m_pdfEmbeddedFontFamilyCache.constFind(cacheKey);
    if (cachedEmbeddedFamily != m_pdfEmbeddedFontFamilyCache.cend()) {
        embeddedFamily = cachedEmbeddedFamily.value();
    } else {
        embeddedFamily = registerEmbeddedFont(font, expectedFamily);
        m_pdfEmbeddedFontFamilyCache.insert(cacheKey, embeddedFamily);
    }

    if (fontSupportsText(embeddedFamily, text)) {
        return embeddedFamily;
    }
    if (fontSupportsText(bundledFamily, text)) {
        return bundledFamily;
    }

    return fontSupportsText(systemFamily, text) ? systemFamily : QString();
}

QString PDFEditController::registerEmbeddedFont(FPDF_FONT font,
                                                const QString &expectedFamily)
{
    if (!font || FPDFFont_GetIsEmbedded(font) != 1) {
        return {};
    }

    size_t fontByteCount = 0;
    if (!FPDFFont_GetFontData(font, nullptr, 0, &fontByteCount)
        || fontByteCount == 0
        || fontByteCount > static_cast<size_t>(kMaximumEmbeddedFontBytes)) {
        return {};
    }

    QByteArray fontData(static_cast<qsizetype>(fontByteCount), '\0');
    size_t actualByteCount = fontByteCount;
    if (!FPDFFont_GetFontData(
            font,
            reinterpret_cast<uint8_t *>(fontData.data()),
            fontByteCount,
            &actualByteCount)
        || actualByteCount == 0
        || actualByteCount > fontByteCount) {
        return {};
    }
    fontData.resize(static_cast<qsizetype>(actualByteCount));

    const QByteArray fingerprint = QCryptographicHash::hash(
        fontData,
        QCryptographicHash::Sha256);
    const auto registeredFamily = m_registeredFontFamilies.constFind(fingerprint);
    if (registeredFamily != m_registeredFontFamilies.cend()) {
        return registeredFamily.value();
    }
    if (fontData.size() > kMaximumRegisteredFontBytes - m_registeredFontBytes) {
        m_registeredFontFamilies.insert(fingerprint, QString());
        return {};
    }

    const int fontId = QFontDatabase::addApplicationFontFromData(fontData);
    if (fontId < 0) {
        m_registeredFontFamilies.insert(fingerprint, QString());
        return {};
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (families.isEmpty()) {
        QFontDatabase::removeApplicationFont(fontId);
        m_registeredFontFamilies.insert(fingerprint, QString());
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
    m_registeredFontFamilies.insert(fingerprint, family);
    return family;
}

void PDFEditController::clearRegisteredFonts()
{
    for (auto fontId = m_registeredFontIds.crbegin();
         fontId != m_registeredFontIds.crend();
         ++fontId) {
        QFontDatabase::removeApplicationFont(*fontId);
    }
    m_registeredFontIds.clear();
    m_registeredFontFamilies.clear();
    m_pdfEmbeddedFontFamilyCache.clear();
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
        && draftRepresentsDeletion(candidate->kind, draft->text)) {
        clearSelection();
        return false;
    }

    setSelection(candidate);
    if (!candidate->editable && !candidate->reason.isEmpty()) {
        emit statusMessage(candidate->reason);
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

void PDFEditController::beginSelectedTextEdit()
{
    if (m_hasSelection
        && m_selectedCandidate.kind == QLatin1String("addedImage")) {
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
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditActive = true;
    emit textEditActiveChanged();
    emit selectionChanged();
}

void PDFEditController::updateSelectedTextPreview(const QString &text)
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
    setDraftText(draft, text);
    updateSelectedDraft(draft);
}

void PDFEditController::beginSelectedFrameResize()
{
    if (m_frameResizeActive || !m_hasSelection || !m_selectedCandidate.editable) {
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
    const qreal left = qBound<qreal>(0.0, normalizedRect.left(), 1.0);
    const qreal top = qBound<qreal>(0.0, normalizedRect.top(), 1.0);
    const qreal right = qBound<qreal>(0.0, normalizedRect.right(), 1.0);
    const qreal bottom = qBound<qreal>(0.0, normalizedRect.bottom(), 1.0);
    if (right - left <= 0.0 || bottom - top <= 0.0) {
        return;
    }

    const QRectF boundedRect(left, top, right - left, bottom - top);
    Draft draft = selectedDraftForDisplay();
    const bool imageDraft = draft.candidate.kind == QLatin1String("addedImage");
    const qreal previousContentHeight = qMax(
        draft.frameRect.height(),
        draft.contentBounds.bottom() - draft.frameRect.y());
    const QRectF resizedContentBounds = imageDraft
        ? boundedRect
        : QRectF(
            boundedRect.x(),
            boundedRect.y(),
            boundedRect.width(),
            qMin(
                1.0 - boundedRect.y(),
                qMax(boundedRect.height(), previousContentHeight)));
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
    m_selectedTextRangeStart = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(start),
        textLength);
    m_selectedTextRangeLength = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(length),
        textLength - m_selectedTextRangeStart);
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
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontStyle(FontStyle style, bool enabled)
{
    if (!m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    if (style != FontStyle::Underline) {
        const auto faceRemainsSelectable = [style, enabled](
                                               const CharacterFormat &format) {
            const bool knownFamily = ApplicationFonts::bundledFontFamilies().contains(
                    format.fontFamily,
                    Qt::CaseInsensitive)
                || ApplicationFonts::systemFontFamilies().contains(
                    format.fontFamily,
                    Qt::CaseInsensitive);
            if (!knownFamily) {
                return true;
            }
            const bool bold = style == FontStyle::Bold
                ? enabled
                : format.fontBold;
            const bool italic = style == FontStyle::Italic
                ? enabled
                : format.fontItalic;
            return ApplicationFonts::isFontFaceSelectable(
                format.fontFamily,
                bold,
                italic);
        };

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
            if (!faceRemainsSelectable(span.format)) {
                return;
            }
        }
        if (!checkedSpan
            && !faceRemainsSelectable(primaryCharacterFormat(draft))) {
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
        [style, enabled](CharacterFormat &format) {
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
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
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
        && finalizedDraft.text == baseline.before.text) {
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
    if (!m_hasSelection || !m_selectedCandidate.editable) {
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

void PDFEditController::deleteSelectedObject()
{
    if (!m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }
    if (m_selectedCandidate.kind != QLatin1String("addedImage")) {
        commitSelectedText(QString());
        return;
    }

    finishSelectedTextEdit();
    const QString key = draftKey(
        m_selectedCandidate.page,
        m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
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
}

void PDFEditController::undo()
{
    if (m_textEditActive || m_undoStack.isEmpty()) {
        return;
    }

    const Command command = m_undoStack.takeLast();
    applyDraftState(command, false, true);
    m_redoStack.append(command);
    emit historyStateChanged();
}

void PDFEditController::redo()
{
    if (m_textEditActive || m_redoStack.isEmpty()) {
        return;
    }

    const Command command = m_redoStack.takeLast();
    applyDraftState(command, true, true);
    m_undoStack.append(command);
    emit historyStateChanged();
}

void PDFEditController::discardDrafts()
{
    if (!m_textEditActive
        && m_drafts.isEmpty()
        && m_undoStack.isEmpty()
        && m_redoStack.isEmpty()) {
        return;
    }

    QSet<int> affectedPages;
    for (auto revision = m_pageDraftRevisions.cbegin();
         revision != m_pageDraftRevisions.cend();
         ++revision) {
        affectedPages.insert(revision.key());
    }

    const bool wasEditing = m_textEditActive;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    m_addedCandidates.clear();
    m_drafts.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    setSelection(nullptr);
    for (int page : std::as_const(affectedPages)) {
        markDraftStateChanged(page, true);
    }
    if (wasEditing) {
        emit textEditActiveChanged();
    }
    emit pageDataChanged(-1);
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
    const PDFSaveWriter::Result result = PDFSaveWriter::save(*this, targetFilePath);
    return completeSave(
        result,
        targetFilePath,
        expectedDocument,
        expectedRevision,
        expectedSourceFilePath);
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
    const PDFSaveWriter::Request request = PDFSaveWriter::prepare(
        *this,
        targetFilePath);
    if (!request.isValid()) {
        emit statusMessage(saveFailureText(PDFSaveWriter::Result{
            PDFSaveWriter::Error::SourceUnavailable}));
        return false;
    }

    QPointer<PDFEditController> controller(this);
    PDFDocument *const expectedDocument = m_document.data();
    const int expectedRevision = m_document->renderRevision();
    const QString expectedSourceFilePath = m_document->renderFilePath();
    m_saving = true;
    emit savingChanged();
    QThreadPool::globalInstance()->start(QRunnable::create(
        [controller,
         expectedDocument,
         expectedRevision,
         expectedSourceFilePath,
         request,
         targetFilePath]() {
            PDFSaveWriter::Result result = PDFSaveWriter::save(request);
            if (!controller) {
                return;
            }
            QMetaObject::invokeMethod(
                controller.data(),
                [controller,
                 expectedDocument,
                 expectedRevision,
                 expectedSourceFilePath,
                 result = std::move(result),
                 targetFilePath]() {
                    if (controller) {
                        controller->completeSave(
                            result,
                            targetFilePath,
                            expectedDocument,
                            expectedRevision,
                            expectedSourceFilePath);
                    }
                },
                Qt::QueuedConnection);
        }));
    return true;
}

bool PDFEditController::completeSave(
    const PDFSaveWriter::Result &result,
    const QString &targetFilePath,
    PDFDocument *expectedDocument,
    int expectedRevision,
    const QString &expectedSourceFilePath)
{
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
    m_pageCandidates.clear();
    m_addedCandidates.clear();
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
}

void PDFEditController::ensureCacheRevision()
{
    const int revision = m_document ? m_document->renderRevision() : -1;
    if (m_cachedRevision == revision) {
        return;
    }

    m_cachedRevision = revision;
    m_pageCandidates.clear();
}

const QVector<PDFEditController::Candidate> &PDFEditController::ensurePageCandidates(int page)
{
    static const QVector<Candidate> empty;

    ensureCacheRevision();
    if (!m_document || !m_document->isLoaded() || page < 0 || page >= m_document->pageCount()) {
        return empty;
    }

    auto cached = m_pageCandidates.find(page);
    if (cached == m_pageCandidates.end()) {
        cached = m_pageCandidates.insert(page, scanPage(page));
    }
    return cached.value();
}

QVector<PDFEditController::Candidate> PDFEditController::scanPage(int pageIndex)
{
    QVector<Candidate> candidates;
    if (!m_document || !m_document->m_document) {
        return candidates;
    }

    const QSizeF pageSize = m_document->pageSize(pageIndex);
    if (pageSize.isEmpty()) {
        return candidates;
    }

    FPDF_PAGE page = nullptr;
    FPDF_TEXTPAGE textPage = nullptr;
    {
        QMutexLocker pageLoadLock(&pdfiumPageLoadMutex());
        page = FPDF_LoadPage(m_document->m_document, pageIndex);
        if (page) {
            textPage = FPDFText_LoadPage(page);
        }
    }
    if (!page) {
        return candidates;
    }

    QHash<FPDF_PAGEOBJECT, TextObjectLayout> textObjectLayouts;
    if (textPage) {
        const int characterCount = FPDFText_CountChars(textPage);
        for (int index = 0; index < characterCount; ++index) {
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

    const int objectCount = FPDFPage_CountObjects(page);
    for (int index = 0; index < objectCount; ++index) {
        appendPageObjectCandidates(
            page,
            FPDFPage_GetObject(page, index),
            QVector<int>{index},
            textPage,
            textObjectLayouts,
            kIdentityMatrix,
            pageSize,
            candidates);
    }
    appendAnnotationCandidates(page, pageIndex, pageSize, candidates);

    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    FPDF_ClosePage(page);

    for (Candidate &candidate : candidates) {
        candidate.page = pageIndex;
    }
    candidates = groupTextLineCandidates(std::move(candidates), pageSize);
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        return left.editable < right.editable;
    });
    return candidates;
}

void PDFEditController::appendPageObjectCandidates(FPDF_PAGE page,
                                                   FPDF_PAGEOBJECT pageObject,
                                                   const QVector<int> &objectPath,
                                                   FPDF_TEXTPAGE textPage,
                                                   const QHash<FPDF_PAGEOBJECT, TextObjectLayout> &textObjectLayouts,
                                                   const FS_MATRIX &containerTransform,
                                                   const QSizeF &pageSize,
                                                   QVector<Candidate> &candidates)
{
    if (!pageObject) {
        return;
    }

    const int type = FPDFPageObj_GetType(pageObject);
    if (type == FPDF_PAGEOBJ_FORM) {
        FS_MATRIX formTransform = kIdentityMatrix;
        if (!FPDFPageObj_GetMatrix(pageObject, &formTransform)) {
            formTransform = kIdentityMatrix;
        }
        const FS_MATRIX childTransform = combinedTransform(containerTransform, formTransform);
        const int count = FPDFFormObj_CountObjects(pageObject);
        for (int index = 0; index < count; ++index) {
            QVector<int> childPath = objectPath;
            childPath.append(index);
            appendPageObjectCandidates(
                page,
                FPDFFormObj_GetObject(pageObject, static_cast<unsigned long>(index)),
                childPath,
                textPage,
                textObjectLayouts,
                childTransform,
                pageSize,
                candidates);
        }
        return;
    }

    if (type != FPDF_PAGEOBJ_TEXT && type != FPDF_PAGEOBJ_IMAGE) {
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
        //% "This content is an image. OCR is required to edit its text."
        candidate.reason = qtTrId("ImageTextRequiresOcr");
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
    FPDF_FONT pdfFont = FPDFTextObj_GetFont(pageObject);
    candidate.fontFamily = resolvedFontFamily(pageObject, candidate.text);
    candidate.fontIdentity = normalizedPdfFontIdentity(pdfFont);
    candidate.fontBold = pdfFontIsBold(pdfFont);
    candidate.fontItalic = pdfFontIsItalic(pdfFont);
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
    const bool editingAllowed = documentAllowsEditing();
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
    expandInitialTextFrame(candidate, pageSize);
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
                                                   int pageIndex,
                                                   const QSizeF &pageSize,
                                                   QVector<Candidate> &candidates) const
{
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    for (int index = 0; index < annotationCount; ++index) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
        if (!annotation) {
            continue;
        }

        const int subtype = FPDFAnnot_GetSubtype(annotation);
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
        candidate.page = pageIndex;
        candidate.objectId = QStringLiteral("annotation:") + QString::number(index);
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
            //% "Form field"
            candidate.label = qtTrId("FormField");
            candidate.text = annotationString(annotation, "V");
        }

        if (!documentAllowsEditing()) {
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
        if (candidate.page == page && candidate.objectId == objectId) {
            return &candidate;
        }
    }

    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    for (const Candidate &candidate : candidates) {
        if (candidate.objectId == objectId) {
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

    if (candidate.parts.isEmpty()) {
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
        case ParagraphAlignment::Left:
            return QStringLiteral("left");
        case ParagraphAlignment::Center:
            return QStringLiteral("center");
        case ParagraphAlignment::Right:
            return QStringLiteral("right");
        case ParagraphAlignment::Justify:
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
    map.insert(QStringLiteral("image"), candidate.image);
    map.insert(QStringLiteral("imagePixelWidth"), candidate.image.width());
    map.insert(QStringLiteral("imagePixelHeight"), candidate.image.height());
    map.insert(QStringLiteral("reason"), candidate.reason);
    return map;
}

bool PDFEditController::documentAllowsEditing() const
{
    if (!m_document || !m_document->m_document) {
        return false;
    }

    const unsigned long permissions = FPDF_GetDocPermissions(m_document->m_document);
    return permissions == 0xffffffffUL || (permissions & kModifyContentsPermission) != 0;
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
        && left.contentBounds == right.contentBounds;
}

void PDFEditController::setDraftText(Draft &draft, const QString &text)
{
    if (draft.text == text) {
        return;
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

    QVector<CharacterFormat> newFormats;
    newFormats.reserve(newLength);
    for (qsizetype index = 0; index < commonPrefix; ++index) {
        newFormats.append(oldFormats.at(index));
    }
    const qsizetype insertedLength = newLength - commonPrefix - commonSuffix;
    for (qsizetype index = 0; index < insertedLength; ++index) {
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

    const ParagraphFormat paragraphPrototype = draft.paragraphFormats.isEmpty()
        ? ParagraphFormat()
        : draft.paragraphFormats.constFirst();
    draft.text = text;
    draft.characterSpans = std::move(newSpans);
    draft.paragraphFormats = paragraphFormatsForText(text, paragraphPrototype);
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

void PDFEditController::resetTextEditDisplayState()
{
    m_textEditDisplayDraft = Draft();
    m_textEditDisplayDraftActive = false;
    m_textEditFrameAutomaticallyExpanded = false;
    m_textEditGeometryExplicitlyChanged = false;
    m_textEditInitialDisplayFrameRect = QRectF();
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
}

void PDFEditController::requestDraftRender(int page, bool immediate)
{
    emit draftRenderStateChanged(page, draftRevisionForPage(page), immediate);
}

void PDFEditController::applyDraftState(const Command &command,
                                        bool useAfterState,
                                        bool immediate)
{
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
        && draftRepresentsDeletion(candidate.kind, draft.text);
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
