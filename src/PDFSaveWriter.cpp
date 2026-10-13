#include "PDFSaveWriter.h"
#include "PDFAtomicSaveFile.h"
#include "PDFShapeUtils.h"
#include "PDFSourceImageGeometry.h"
#include "PDFSourceTextGeometry.h"
#include "PDFHeaderMetadata.h"
#include "PDFWebLinkScanner.h"

#include "ApplicationFonts.h"
#include "PDFAcroFormCanonicalizer.h"
#include "PDFAcroFormWriter.h"
#include "PDFAnnotationUtils.h"
#include "PDFEditController.h"
#include "PDFFontAliases.h"
#include "PDFUnicodeFont.h"
#include "PDFSymbolFont.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFPasswordWriter.h"
#include "PDFOpenTypeFontWriter.h"
#include "PDFImageFormResources.h"
#include "PDFiumSynchronization.h"
#include "PDFiumDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

#include <QFile>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRawFont>
#include <QScopeGuard>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>

namespace {

using PDFAnnotationUtils::annotationString;
using PDFAnnotationUtils::annotationUsesReplyRelation;
using PDFAnnotationUtils::isMarkupAnnotation;
using PDFAnnotationUtils::isSupportedCommentAnnotationSubtype;
using PDFAnnotationUtils::normalizedCommentText;

constexpr qreal kMinimumLayoutWidth = 1.0;
constexpr qreal kMinimumObjectWidth = 0.01;
constexpr qreal kMinimumScale = 0.1;
constexpr qreal kMaximumScale = 10.0;
constexpr int kCoordinateScale = 64;
constexpr qreal kUnsupportedPaintRasterScale = 4.0;
constexpr qint64 kMaximumUnsupportedPaintPixels = 8 * 1024 * 1024;
constexpr int kPaintColorTolerance = 12;
constexpr char kPreservedPaintMark[] = "TrePDFPreservedPaint";
constexpr char kUnderlineMark[] = "TrePDFUnderline";
constexpr char kUnderlineIdKey[] = "Id";
constexpr char kCommentThreadIdKey[] = "TrePDFThreadId";
constexpr char kCommentReplyToKey[] = "TrePDFReplyTo";
constexpr char kFreeTextAppearanceIdKey[]
    = "TrePDFFreeTextAppearanceId";
constexpr int kAnnotationFlagLockedContents = 1 << 9;
constexpr qsizetype kMaximumSpacingLineParts = 1024;
constexpr qsizetype kMaximumSpacingDraftParts = 32768;
constexpr char kTextSpacingMark[] = "TrePDFTextSpacing";
constexpr char kParagraphLayoutMark[] = "TrePDFParagraphLayout";
constexpr qsizetype kMaximumLayoutParagraphs = 1024;
constexpr qsizetype kMaximumParagraphModelBytes = 1024 * 1024;
struct SaveFormat {
    QString fontFamily;
    // Snapshot the public name and exact source identity while the GUI-owned
    // alias is alive. Private display names never enter saved PDF metadata or
    // font-provider requests, including asynchronous saves after tab teardown.
    QString semanticFontFamily;
    QByteArray displaySourceFingerprint;
    QString embeddableFontFamily;
    QString embeddableLayoutFamily;
    QByteArray embeddableFontData;
    // Distinguish separate PDF subsets that retain the same family/style name.
    QByteArray sourceFontFingerprint;
    QVector<char32_t> embeddedFontCharacters;
    // Derived once from the finalized shared character list, never per run.
    QByteArray embeddedCharactersDigest;
    qreal fontSizeRatio = 0.0;
    qreal displayFontSizeRatio = 0.0;
    qreal letterSpacingRatio = 0.0;
    qreal wordSpacingRatio = 0.0;
    // A source cluster's residual advance is independent of the spacing the
    // user chose. Keep it in layout and metadata without stretching glyphs.
    qreal sourceAdvanceRatio = 0.0;
    int fontStretch = QFont::Unstretched;
    bool fontBold = false;
    bool fontItalic = false;
    bool fontUnderline = false;
    bool forceEmbeddedFont = false;
    bool allowOriginalFontReuse = true;
    // Borrowed from this save's page only; populated after the GUI snapshot.
    FPDF_FONT originalType1Font = nullptr;
    QColor color = Qt::black;
};

struct SaveSpan {
    qsizetype start = 0;
    qsizetype length = 0;
    SaveFormat format;
};

struct SaveParagraph {
    qsizetype start = 0;
    qsizetype length = 0;
    Qt::Alignment alignment = Qt::AlignLeft;
    qreal lineSpacing = 1.0;
    qreal spaceBeforeRatio = 0.0;
    qreal spaceAfterRatio = 0.0;
    qreal firstLineIndentRatio = 0.0;
};

struct SaveDraft {
    enum class Kind {
        Text,
        Image,
        Shape,
        TextMarkup,
        StickyNote,
        FreeText,
        Stamp,
        DrawingComment
    };

    Kind kind = Kind::Text;
    int page = -1;
    QString objectId;
    QString headerGroupId;
    QString headerRole;
    QVector<QVector<int>> sourceObjectPaths;
    QVector<QVector<int>> sourceUnderlineObjectPaths;
    QString originalText;
    QString text;
    QString sourceAnnotationName;
    bool sourceAnnotationHasName = false;
    QVector<SaveSpan> characterSpans;
    QVector<SaveParagraph> paragraphFormats;
    QVector<qsizetype> sourceLineBreaks;
    bool sourceParagraphLayout = false;
    QRectF sourceRect;
    QRectF frameRect;
    QVector<QRectF> highlightRects;
    QVector<std::array<QPointF, 4>> textMarkupQuads;
    QColor highlightColor;
    FPDF_ANNOTATION_SUBTYPE annotationSubtype = FPDF_ANNOT_HIGHLIGHT;
    PDFCommentAnnotation::DrawingType drawingType
        = PDFCommentAnnotation::DrawingType::Square;
    QVector<QPointF> drawingVertices;
    QVector<QVector<QPointF>> inkStrokes;
    PDFCommentAnnotation::CommonProperties originalCommentProperties;
    PDFCommentAnnotation::CommonProperties commentProperties;
    QByteArray sourceDrawingAppearanceFingerprint;
    QString stampName;
    QString stampText;
    QString stampDetail;
    QColor stampColor;
    int sourceAnnotationIndex = -1;
    QString commentAuthor;
    QDateTime commentCreationDate;
    QDateTime commentModifiedDate;
    QString commentThreadId;
    QString replyToThreadId;
    int replyToSourceAnnotationIndex = -1;
    PDFEditController::CommentStatus originalCommentStatus
        = PDFEditController::NoCommentStatus;
    PDFEditController::CommentStatus commentStatus
        = PDFEditController::NoCommentStatus;
    bool originalCommentMarked = false;
    bool commentMarked = false;
    int commentStatusParentAnnotationIndex = -1;
    int commentMarkedParentAnnotationIndex = -1;
    QString commentStateAuthor;
    QDateTime commentStateModifiedDate;
    QImage image;
    int imageQuarterTurns = 0;
    PDFEditController::ShapeType shapeType
        = PDFEditController::RectangleShape;
    QPointF shapeStartRatio;
    QPointF shapeEndRatio = QPointF(1.0, 1.0);
    qreal shapeStrokeWidthRatio = 0.0;
    QByteArray sourceShapeMetadata;
    QPointF layoutOrigin;
    SaveFormat originalFormat;
    qreal originalHorizontalScale = 1.0;
    QColor originalFillColor = Qt::black;
    QColor originalStrokeColor = Qt::black;
    qreal originalStrokeWidth = 1.0;
    FPDF_TEXT_RENDERMODE originalRenderMode = FPDF_TEXTRENDERMODE_FILL;
    quint64 addedOrder = 0;
    bool hasBaseline = false;
    bool added = false;
    bool deleted = false;
    bool movesSourceObjects = false;
    bool reply = false;
};

struct LayoutRun {
    QString text;
    SaveFormat format;
    qreal x = 0.0;
    qreal baselineFromTop = 0.0;
    qreal width = 0.0;
    bool positionedWord = false;
    QByteArray spacingGroup;
    QString spacingText;
    int spacingIndex = 0;
    int spacingCount = 0;
    int spacingStart = 0;
    QByteArray spacingRuns;
    QString leadingWhitespace;
    qreal leadingLayoutAdvance = 0.0;
    // Absolute Qt advances from the first encoded character, before the PDF
    // text matrix's horizontal scale. Tabs remain layout-only characters.
    QVector<qreal> characterPositions;
    bool hasCharacterPositions = false;
    qsizetype textStart = 0;
    qsizetype textLength = 0;
    QByteArray paragraphGroup;
    int paragraphIndex = 0;
    int paragraphCount = 0;
    QString paragraphText;
    QJsonObject paragraphModel;
    QPointF paragraphOrigin;
    QRectF paragraphFrame;
};

struct SourceObject {
    FPDF_PAGEOBJECT parentForm = nullptr;
    FPDF_PAGEOBJECT object = nullptr;
    int topLevelIndex = -1;
};

struct PreparedDraft {
    SaveDraft::Kind kind = SaveDraft::Kind::Text;
    QString objectId;
    QVector<SourceObject> sources;
    QVector<FPDF_PAGEOBJECT> objects;
    QVector<FPDF_BITMAP> bitmaps;
    QVector<int> objectInsertionBoundaries;
    int insertionIndex = 0;
    bool append = false;
    bool movesSourceObjects = false;
    QRectF sourceRect;
    QRectF frameRect;
    int imageQuarterTurns = 0;
};

bool pageObjectBounds(FPDF_PAGEOBJECT object, QRectF &bounds)
{
    float left = 0.0F, bottom = 0.0F, right = 0.0F, top = 0.0F;
    if (!FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top)
        || !qIsFinite(left) || !qIsFinite(bottom)
        || !qIsFinite(right) || !qIsFinite(top) || right < left || top < bottom) {
        return false;
    }
    bounds = QRectF(left, bottom, right - left, top - bottom);
    return true;
}

bool prepareTextPaintOrder(FPDF_PAGE page, PreparedDraft &prepared,
                          const QHash<int, QRectF> &changedPaintBounds)
{
    prepared.objectInsertionBoundaries.fill(prepared.insertionIndex, prepared.objects.size());
    if (prepared.kind != SaveDraft::Kind::Text || prepared.sources.size() < 2
        || prepared.objects.isEmpty()) {
        return true;
    }

    struct SourceBounds {
        int index;
        QRectF bounds;
    };
    QVector<SourceBounds> sources;
    QSet<int> sourceIndexes;
    int lastSourceIndex = prepared.insertionIndex;
    for (const SourceObject &source : std::as_const(prepared.sources)) {
        QRectF bounds;
        if (!pageObjectBounds(source.object, bounds))
            return false;
        sources.append({source.topLevelIndex, bounds});
        sourceIndexes.insert(source.topLevelIndex);
        lastSourceIndex = qMax(lastSourceIndex, source.topLevelIndex);
    }

    struct InterleavedPaint {
        int index;
        QRectF bounds;
        bool background;
    };
    QVector<InterleavedPaint> paints;
    // Keep geometry work bounded for adversarial pages with many interleaved
    // objects and runs. A failure leaves the source/destination untouched.
    constexpr qint64 maximumComparisons = 8 * 1024 * 1024;
    qint64 comparisons = 0;
    for (int index = prepared.insertionIndex + 1; index < lastSourceIndex; ++index) {
        if (sourceIndexes.contains(index))
            continue;
        QRectF bounds;
        const auto changed = changedPaintBounds.constFind(index);
        if (changed != changedPaintBounds.cend()) {
            // Another draft may move or replace this paint in the same Save.
            // Its new bounds are authoritative; never infer it as a source
            // background from geometry that is about to disappear.
            if (!changed->isEmpty())
                paints.append({index, *changed, false});
            continue;
        }
        if (!pageObjectBounds(FPDFPage_GetObject(page, index), bounds))
            return false;
        bool behindSource = false;
        bool overSource = false;
        for (const SourceBounds &source : std::as_const(sources)) {
            if (++comparisons > maximumComparisons)
                return false;
            if (!bounds.intersects(source.bounds))
                continue;
            behindSource |= index < source.index;
            overSource |= index > source.index;
        }
        paints.append({index, bounds, behindSource && !overSource});
    }

    // Word can paint a separate background before each line of a paragraph.
    // Replacing every line at the first source index puts later lines under
    // those backgrounds. Place each new run after the backgrounds it touches,
    // while stopping before paint that intentionally covered earlier text.
    // Per-run placement also preserves a foreground between two source lines.
    for (qsizetype index = 0; index < prepared.objects.size(); ++index) {
        const FPDF_PAGEOBJECT object = prepared.objects.at(index);
        if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_PATH && index > 0) {
            // Generated underlines belong to the immediately preceding run.
            prepared.objectInsertionBoundaries[index]
                = prepared.objectInsertionBoundaries.at(index - 1);
            continue;
        }
        QRectF bounds;
        if (!pageObjectBounds(object, bounds))
            return false;
        if (index + 1 < prepared.objects.size()
            && FPDFPageObj_GetType(prepared.objects.at(index + 1)) == FPDF_PAGEOBJ_PATH) {
            QRectF underlineBounds;
            if (!pageObjectBounds(prepared.objects.at(index + 1), underlineBounds))
                return false;
            bounds = bounds.united(underlineBounds);
        }
        int lowerBoundary = prepared.insertionIndex;
        int upperBoundary = lastSourceIndex;
        for (const InterleavedPaint &paint : std::as_const(paints)) {
            if (++comparisons > maximumComparisons)
                return false;
            if (!bounds.intersects(paint.bounds))
                continue;
            if (paint.background)
                lowerBoundary = qMax(lowerBoundary, paint.index + 1);
            else
                upperBoundary = qMin(upperBoundary, paint.index);
        }
        if (lowerBoundary > upperBoundary)
            return false;
        prepared.objectInsertionBoundaries[index] = lowerBoundary;
    }
    return true;
}

using PdfDocumentHandle = PDFiumDocument;

class PdfPageHandle
{
public:
    explicit PdfPageHandle(FPDF_PAGE page = nullptr)
        : m_page(page)
    {
    }

    ~PdfPageHandle()
    {
        if (m_page) {
            assertPDFiumProcessLockHeld();
            FPDF_ClosePage(m_page);
        }
    }

    FPDF_PAGE get() const
    {
        return m_page;
    }

    Q_DISABLE_COPY_MOVE(PdfPageHandle)

private:
    FPDF_PAGE m_page = nullptr;
};

class PdfAnnotationHandle
{
public:
    explicit PdfAnnotationHandle(FPDF_ANNOTATION annotation = nullptr)
        : m_annotation(annotation)
    {
    }

    ~PdfAnnotationHandle()
    {
        if (m_annotation) {
            assertPDFiumProcessLockHeld();
            FPDFPage_CloseAnnot(m_annotation);
        }
    }

    FPDF_ANNOTATION get() const
    {
        return m_annotation;
    }

    Q_DISABLE_COPY_MOVE(PdfAnnotationHandle)

private:
    FPDF_ANNOTATION m_annotation = nullptr;
};

class SaveFileWriter : public FPDF_FILEWRITE
{
public:
    explicit SaveFileWriter(const QString &filePath)
        : m_file(filePath)
    {
        version = 1;
        WriteBlock = &SaveFileWriter::writeBlock;
    }

    bool open()
    {
        return m_file.open(QIODevice::WriteOnly);
    }

    bool commit()
    {
        return m_file.commit();
    }

    QString errorString() const
    {
        return m_file.errorString();
    }

private:
    static int writeBlock(FPDF_FILEWRITE *fileWriter,
                          const void *data,
                          unsigned long size)
    {
        auto *writer = static_cast<SaveFileWriter *>(fileWriter);
        const qint64 byteCount = static_cast<qint64>(size);
        return writer->m_file.write(static_cast<const char *>(data), byteCount)
                == byteCount
            ? 1
            : 0;
    }

    PDFAtomicSaveFile m_file;
};

class PageCoordinateMapper
{
public:
    PageCoordinateMapper(FPDF_PAGE page, qreal pageWidth, qreal pageHeight)
        : m_page(page)
        , m_deviceWidth(qMax(1, qRound(pageWidth * kCoordinateScale)))
        , m_deviceHeight(qMax(1, qRound(pageHeight * kCoordinateScale)))
    {
    }

    bool pagePoint(qreal x, qreal y, QPointF &point) const
    {
        double pageX = 0.0;
        double pageY = 0.0;
        if (!FPDF_DeviceToPage(
                m_page,
                0,
                0,
                m_deviceWidth,
                m_deviceHeight,
                0,
                qRound(x * kCoordinateScale),
                qRound(y * kCoordinateScale),
                &pageX,
                &pageY)) {
            return false;
        }
        point = QPointF(pageX, pageY);
        return true;
    }

    bool textMatrix(qreal x,
                    qreal baselineFromTop,
                    qreal horizontalScale,
                    FS_MATRIX &matrix) const
    {
        QPointF origin;
        QPointF deviceRight;
        QPointF deviceUp;
        if (!pagePoint(x, baselineFromTop, origin)
            || !pagePoint(x + 1.0, baselineFromTop, deviceRight)
            || !pagePoint(x, baselineFromTop - 1.0, deviceUp)) {
            return false;
        }

        const QPointF horizontal = deviceRight - origin;
        const QPointF vertical = deviceUp - origin;
        matrix = FS_MATRIX{
            static_cast<float>(horizontal.x() * horizontalScale),
            static_cast<float>(horizontal.y() * horizontalScale),
            static_cast<float>(vertical.x()),
            static_cast<float>(vertical.y()),
            static_cast<float>(origin.x()),
            static_cast<float>(origin.y())};
        return true;
    }

    bool imageMatrix(const QRectF &rect,
                     FS_MATRIX &matrix,
                     int imageQuarterTurns = 0) const
    {
        const QTransform rotation
            = PDFSourceImageGeometry::unitQuarterTurnTransform(imageQuarterTurns);
        const auto imagePoint = [&](const QPointF &unitPoint, QPointF &point) {
            const QPointF rotated = rotation.map(unitPoint);
            return pagePoint(rect.left() + rotated.x() * rect.width(),
                             rect.top() + rotated.y() * rect.height(), point);
        };
        QPointF bottomLeft;
        QPointF bottomRight;
        QPointF topLeft;
        if (!imagePoint(QPointF(0.0, 1.0), bottomLeft)
            || !imagePoint(QPointF(1.0, 1.0), bottomRight)
            || !imagePoint(QPointF(0.0, 0.0), topLeft)) {
            return false;
        }

        const QPointF horizontal = bottomRight - bottomLeft;
        const QPointF vertical = topLeft - bottomLeft;
        matrix = FS_MATRIX{
            static_cast<float>(horizontal.x()),
            static_cast<float>(horizontal.y()),
            static_cast<float>(vertical.x()),
            static_cast<float>(vertical.y()),
            static_cast<float>(bottomLeft.x()),
            static_cast<float>(bottomLeft.y())};
        return true;
    }

    qreal strokeWidth(qreal deviceWidth) const
    {
        QPointF origin;
        QPointF deviceRight;
        QPointF deviceUp;
        if (!pagePoint(0.0, 0.0, origin)
            || !pagePoint(1.0, 0.0, deviceRight)
            || !pagePoint(0.0, 1.0, deviceUp)) {
            return deviceWidth;
        }
        const qreal horizontalScale = std::hypot(
            deviceRight.x() - origin.x(),
            deviceRight.y() - origin.y());
        const qreal verticalScale = std::hypot(
            deviceUp.x() - origin.x(),
            deviceUp.y() - origin.y());
        return deviceWidth * qMax<qreal>(
            0.01,
            (horizontalScale + verticalScale) / 2.0);
    }

private:
    FPDF_PAGE m_page = nullptr;
    int m_deviceWidth = 1;
    int m_deviceHeight = 1;
};

bool sameFamily(const QString &left, const QString &right)
{
    return left.trimmed().compare(right.trimmed(), Qt::CaseInsensitive) == 0;
}

bool sameFormat(const SaveFormat &left, const SaveFormat &right)
{
    return sameFamily(left.fontFamily, right.fontFamily)
        && sameFamily(left.embeddableFontFamily, right.embeddableFontFamily)
        && left.sourceFontFingerprint == right.sourceFontFingerprint
        && left.embeddedFontCharacters == right.embeddedFontCharacters
        && left.originalType1Font == right.originalType1Font
        && left.fontSizeRatio == right.fontSizeRatio
        && left.displayFontSizeRatio == right.displayFontSizeRatio
        && left.letterSpacingRatio == right.letterSpacingRatio
        && left.wordSpacingRatio == right.wordSpacingRatio
        && left.sourceAdvanceRatio == right.sourceAdvanceRatio
        && left.fontStretch == right.fontStretch
        && left.fontBold == right.fontBold
        && left.fontItalic == right.fontItalic
        && left.fontUnderline == right.fontUnderline
        && left.forceEmbeddedFont == right.forceEmbeddedFont
        && left.color == right.color;
}

bool samePositionedFormat(SaveFormat left, SaveFormat right)
{
    // Explicit character positions carry spacing independently from the font
    // and paint shared by the actual PDF text object.
    left.letterSpacingRatio = right.letterSpacingRatio = 0.0;
    left.wordSpacingRatio = right.wordSpacingRatio = 0.0;
    left.sourceAdvanceRatio = right.sourceAdvanceRatio = 0.0;
    return sameFormat(left, right)
        && left.displaySourceFingerprint == right.displaySourceFingerprint
        && left.embeddableLayoutFamily == right.embeddableLayoutFamily
        && left.allowOriginalFontReuse == right.allowOriginalFontReuse;
}

bool canReuseOriginalFont(const SaveFormat &format,
                          const SaveFormat &originalFormat)
{
    return sameFamily(format.fontFamily, originalFormat.fontFamily)
        && format.fontBold == originalFormat.fontBold
        && format.fontItalic == originalFormat.fontItalic;
}

QString standardFontName(const SaveFormat &format)
{
    QString normalized = format.semanticFontFamily.toLower();
    normalized.remove(QLatin1Char(' '));
    normalized.remove(QLatin1Char('-'));

    QString family;
    if (normalized.contains(QStringLiteral("courier"))
        || normalized.contains(QStringLiteral("mono"))) {
        family = QStringLiteral("Courier");
    } else if (normalized.contains(QStringLiteral("times"))
               || normalized.contains(QStringLiteral("serif"))) {
        family = QStringLiteral("Times");
    } else if (normalized.contains(QStringLiteral("helvetica"))
               || normalized.contains(QStringLiteral("arial"))
               || normalized.contains(QStringLiteral("sans"))) {
        family = QStringLiteral("Helvetica");
    } else {
        return {};
    }

    if (family == QLatin1String("Times")) {
        if (format.fontBold && format.fontItalic) {
            return QStringLiteral("Times-BoldItalic");
        }
        if (format.fontBold) {
            return QStringLiteral("Times-Bold");
        }
        if (format.fontItalic) {
            return QStringLiteral("Times-Italic");
        }
        return QStringLiteral("Times-Roman");
    }

    if (format.fontBold && format.fontItalic) {
        return family + QStringLiteral("-BoldOblique");
    }
    if (format.fontBold) {
        return family + QStringLiteral("-Bold");
    }
    if (format.fontItalic) {
        return family + QStringLiteral("-Oblique");
    }
    return family;
}

QString fontFamilyMatchKey(QString name)
{
    bool hasSubsetPrefix = name.size() > 7
        && name.at(6) == QLatin1Char('+');
    for (qsizetype index = 0; hasSubsetPrefix && index < 6; ++index) {
        const QChar character = name.at(index);
        hasSubsetPrefix = character >= QLatin1Char('A')
            && character <= QLatin1Char('Z');
    }
    if (hasSubsetPrefix) {
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

bool equivalentFamily(const QString &left, const QString &right)
{
    const QString leftKey = fontFamilyMatchKey(left);
    return sameFamily(left, right)
        || (!leftKey.isEmpty() && leftKey == fontFamilyMatchKey(right));
}

QString matchingKnownFamily(const QString &family,
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

QString matchingBundledFamily(const QString &family)
{
    return matchingKnownFamily(
        family,
        ApplicationFonts::bundledFontFamilies());
}

QString matchingSystemFamily(const QString &family)
{
    return matchingKnownFamily(
        family,
        ApplicationFonts::systemFontFamilies());
}

bool isArialCompatibleFamily(const QString &family)
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

QString fontFaceKey(const SaveFormat &format)
{
    return format.fontFamily.toCaseFolded()
        + QLatin1Char('|')
        + QString::number(format.fontBold)
        + QString::number(format.fontItalic);
}

QFont displayFont(const SaveFormat &format, qreal pageHeight)
{
    const qreal sizeRatio = format.displayFontSizeRatio > 0.0
        ? format.displayFontSizeRatio
        : format.fontSizeRatio;
    const int pixelSize = qMax(1, qRound(sizeRatio * pageHeight));
    // Also cover original-font reuse, which does not pass through the
    // replacement-font branch. Keep fontFamily unchanged for PDF identity and
    // reuse checks; only Qt layout uses the downloaded face's private name.
    const QString layoutFamily = !format.embeddableLayoutFamily.isEmpty()
            && sameFamily(format.fontFamily, format.embeddableFontFamily)
        ? format.embeddableLayoutFamily : format.fontFamily;
    QFont font;
    if (!layoutFamily.isEmpty()) {
        font.setFamily(layoutFamily);
    }
    font.setBold(format.fontBold);
    font.setItalic(format.fontItalic);
    font.setUnderline(format.fontUnderline);
    font.setPixelSize(pixelSize);
#ifdef Q_OS_WIN
    // Keep the writer's advances identical to fontForDisplay and calibration.
    font.setHintingPreference(QFont::PreferNoHinting);
#endif
    font.setStretch(ApplicationFonts::calibratedFontStretch(
        layoutFamily,
        format.fontStretch,
        format.fontBold,
        format.fontItalic,
        pixelSize));
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          (format.letterSpacingRatio + format.sourceAdvanceRatio) * pageHeight);
    font.setWordSpacing(format.wordSpacingRatio * pageHeight);
    font.setKerning(qFuzzyIsNull(format.sourceAdvanceRatio));
    return font;
}

const SaveFormat &formatAt(const SaveDraft &draft, qsizetype position)
{
    for (const SaveSpan &span : draft.characterSpans) {
        if (position >= span.start && position < span.start + span.length) {
            return span.format;
        }
    }
    return draft.characterSpans.isEmpty()
        ? draft.originalFormat
        : draft.characterSpans.constFirst().format;
}

SaveParagraph paragraphFormatAt(const SaveDraft &draft, qsizetype position)
{
    for (const SaveParagraph &paragraph : draft.paragraphFormats) {
        if (position >= paragraph.start
            && position <= paragraph.start + paragraph.length) {
            return paragraph;
        }
    }
    return {};
}

void appendSpan(QVector<SaveSpan> &spans,
                qsizetype start,
                qsizetype length,
                const SaveFormat &format)
{
    if (!spans.isEmpty()
        && spans.constLast().start + spans.constLast().length == start
        && sameFormat(spans.constLast().format, format)) {
        spans.last().length += length;
        return;
    }
    spans.append(SaveSpan{start, length, format});
}

void normalizeDraftText(SaveDraft &draft)
{
    const QString originalText = draft.text;
    const QString normalizedText = originalText.normalized(
        QString::NormalizationForm_C);
    if (originalText == normalizedText) {
        return;
    }

    QVector<SaveParagraph> originalParagraphs;
    QVector<bool> originalSourceBreaks;
    qsizetype paragraphStart = 0;
    while (paragraphStart <= originalText.size()) {
        originalParagraphs.append(paragraphFormatAt(draft, paragraphStart));
        const qsizetype newline = originalText.indexOf(
            QLatin1Char('\n'),
            paragraphStart);
        if (newline < 0) {
            break;
        }
        originalSourceBreaks.append(draft.sourceLineBreaks.contains(newline));
        paragraphStart = newline + 1;
    }

    QString rebuiltText;
    rebuiltText.reserve(normalizedText.size());
    QVector<SaveSpan> rebuiltSpans;
    // Keep each grapheme in one format span when NFC composes multiple UTF-16
    // code units into one character.
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, originalText);
    finder.toStart();
    qsizetype clusterStart = 0;
    while (clusterStart < originalText.size()) {
        const qsizetype clusterEnd = finder.toNextBoundary();
        if (clusterEnd < 0 || clusterEnd <= clusterStart) {
            break;
        }
        const QString cluster = originalText
            .sliced(clusterStart, clusterEnd - clusterStart)
            .normalized(QString::NormalizationForm_C);
        const qsizetype rebuiltStart = rebuiltText.size();
        rebuiltText.append(cluster);
        appendSpan(
            rebuiltSpans,
            rebuiltStart,
            cluster.size(),
            formatAt(draft, clusterStart));
        clusterStart = clusterEnd;
    }
    if (clusterStart < originalText.size()) {
        const QString tail = originalText
            .sliced(clusterStart)
            .normalized(QString::NormalizationForm_C);
        const qsizetype rebuiltStart = rebuiltText.size();
        rebuiltText.append(tail);
        appendSpan(
            rebuiltSpans,
            rebuiltStart,
            tail.size(),
            formatAt(draft, clusterStart));
    }

    QVector<SaveParagraph> rebuiltParagraphs;
    QVector<qsizetype> rebuiltSourceBreaks;
    paragraphStart = 0;
    qsizetype paragraphIndex = 0;
    while (paragraphStart <= rebuiltText.size()) {
        const qsizetype newline = rebuiltText.indexOf(
            QLatin1Char('\n'),
            paragraphStart);
        const qsizetype paragraphEnd = newline < 0
            ? rebuiltText.size()
            : newline;
        SaveParagraph paragraph = paragraphIndex < originalParagraphs.size()
            ? originalParagraphs.at(paragraphIndex) : SaveParagraph{};
        paragraph.start = paragraphStart;
        paragraph.length = paragraphEnd - paragraphStart;
        rebuiltParagraphs.append(paragraph);
        if (newline < 0) {
            break;
        }
        if (paragraphIndex < originalSourceBreaks.size()
            && originalSourceBreaks.at(paragraphIndex)) {
            rebuiltSourceBreaks.append(newline);
        }
        paragraphStart = newline + 1;
        ++paragraphIndex;
    }

    draft.text = std::move(rebuiltText);
    draft.characterSpans = std::move(rebuiltSpans);
    draft.paragraphFormats = std::move(rebuiltParagraphs);
    draft.sourceLineBreaks = std::move(rebuiltSourceBreaks);
}

bool requiresEmbeddedUnicodeFont(const QString &text)
{
    // An existing PDF font may be a subset whose Unicode map covers only the
    // original text. PDFium accepts new UTF-16 text without validating that map.
    for (uint character : text.toUcs4()) {
        if (character > 0x7fU) {
            return true;
        }
    }
    return false;
}

bool originalTextContainsCharacters(const QString &originalText,
                                    const QString &text)
{
    QSet<uint> originalCharacters;
    for (uint character : originalText.toUcs4()) {
        originalCharacters.insert(character);
    }
    for (uint character : text.toUcs4()) {
        // Only LF is consumed by layoutRuns rather than encoded as a glyph.
        // Even a space can be absent from the original PDF font's subset/map.
        if (character != '\n'
            && !originalCharacters.contains(character)) {
            return false;
        }
    }
    return true;
}

QList<QTextLayout::FormatRange> layoutFormats(const SaveDraft &draft,
                                               qsizetype paragraphStart,
                                               qsizetype paragraphLength,
                                               qreal pageHeight)
{
    QList<QTextLayout::FormatRange> formats;
    const qsizetype paragraphEnd = paragraphStart + paragraphLength;
    for (const SaveSpan &span : draft.characterSpans) {
        const qsizetype start = qMax(paragraphStart, span.start);
        const qsizetype end = qMin(paragraphEnd, span.start + span.length);
        if (end <= start) {
            continue;
        }

        QTextCharFormat textFormat;
        textFormat.setFont(displayFont(span.format, pageHeight));
        textFormat.setForeground(span.format.color);
        QTextLayout::FormatRange range;
        range.start = static_cast<int>(start - paragraphStart);
        range.length = static_cast<int>(end - start);
        range.format = textFormat;
        formats.append(range);
    }
    return formats;
}

bool hasTextSpacing(const SaveFormat &format)
{
    return !qFuzzyIsNull(format.letterSpacingRatio)
        || !qFuzzyIsNull(format.wordSpacingRatio)
        || !qFuzzyIsNull(format.sourceAdvanceRatio);
}

bool lineHasTextSpacing(const SaveDraft &draft,
                        qsizetype start,
                        qsizetype end)
{
    if (draft.characterSpans.isEmpty()) {
        return hasTextSpacing(draft.originalFormat);
    }
    return std::any_of(draft.characterSpans.cbegin(),
                       draft.characterSpans.cend(),
                       [start, end](const SaveSpan &span) {
        return span.start < end && span.start + span.length > start
            && hasTextSpacing(span.format);
    });
}

bool hasParagraphLayout(const SaveDraft &draft)
{
    // Resetting the controls must not discard the source group's explicit
    // paragraph boundaries or its original layout frame on the next save.
    // Header groups also need their frame retained for identical wrapping on
    // other pages, even when their text uses only one default character format.
    return draft.sourceParagraphLayout || !draft.headerGroupId.isEmpty()
        || std::any_of(draft.characterSpans.cbegin(), draft.characterSpans.cend(),
                       [](const SaveSpan &span) {
            return !qFuzzyIsNull(span.format.sourceAdvanceRatio);
        })
        // A rich-text edit can emit several faces with different ink bounds.
        // Persist its logical group and layout so reopen does not have to
        // infer joins or recalibrate each face from those bounds again.
        || (!draft.characterSpans.isEmpty()
            && std::any_of(draft.characterSpans.cbegin(), draft.characterSpans.cend(),
                           [&](const SaveSpan &span) {
                return !sameFormat(span.format, draft.characterSpans.constFirst().format);
            }))
        || std::any_of(draft.paragraphFormats.cbegin(),
                       draft.paragraphFormats.cend(),
                       [](const SaveParagraph &paragraph) {
        return !qFuzzyCompare(paragraph.lineSpacing, 1.0)
            || !qFuzzyIsNull(paragraph.spaceBeforeRatio)
            || !qFuzzyIsNull(paragraph.spaceAfterRatio)
            || !qFuzzyIsNull(paragraph.firstLineIndentRatio);
    });
}

qreal textLayoutDistance(qreal value)
{
    // QTextDocument stores proportional line advances and block margins in
    // 26.6 fixed-point units. Quantize each advance, not the accumulated y,
    // so fractional factors stay aligned over long paragraphs.
    return std::floor(value * 64.0) / 64.0;
}

QJsonObject paragraphLayoutModel(const SaveDraft &draft)
{
    QJsonArray paragraphs;
    for (const SaveParagraph &paragraph : draft.paragraphFormats) {
        const QString alignment = paragraph.alignment.testFlag(Qt::AlignJustify)
            ? QStringLiteral("justify")
            : paragraph.alignment.testFlag(Qt::AlignHCenter)
                ? QStringLiteral("center")
                : paragraph.alignment.testFlag(Qt::AlignRight)
                    ? QStringLiteral("right") : QStringLiteral("left");
        paragraphs.append(QJsonObject{
            {QStringLiteral("start"), paragraph.start},
            {QStringLiteral("length"), paragraph.length},
            {QStringLiteral("alignment"), alignment},
            {QStringLiteral("lineSpacing"), paragraph.lineSpacing},
            {QStringLiteral("spaceBeforeRatio"), paragraph.spaceBeforeRatio},
            {QStringLiteral("spaceAfterRatio"), paragraph.spaceAfterRatio},
            {QStringLiteral("firstLineIndentRatio"), paragraph.firstLineIndentRatio}});
    }
    QJsonArray sourceLineBreaks;
    for (qsizetype position : draft.sourceLineBreaks) {
        sourceLineBreaks.append(position);
    }
    // A space may be encoded with the preceding glyph even when its semantic
    // format differs. Keep all whitespace formats, including blank paragraphs;
    // visible text style remains authoritative in the actual PDF objects.
    QJsonArray whitespaceFormats;
    qsizetype spanIndex = 0;
    const auto appendWhitespace = [&](qsizetype start, qsizetype end) {
        while (start < end) {
            while (spanIndex + 1 < draft.characterSpans.size()
                   && draft.characterSpans[spanIndex].start
                       + draft.characterSpans[spanIndex].length <= start) {
                ++spanIndex;
            }
            const SaveFormat &format = draft.characterSpans.isEmpty()
                ? draft.originalFormat : draft.characterSpans[spanIndex].format;
            const qsizetype next = draft.characterSpans.isEmpty() ? end
                : qMin(end, draft.characterSpans[spanIndex].start
                               + draft.characterSpans[spanIndex].length);
            if (next <= start) {
                break;
            }
            whitespaceFormats.append(QJsonObject{
                {QStringLiteral("start"), start}, {QStringLiteral("length"), next - start},
                {QStringLiteral("fontFamily"), format.semanticFontFamily},
                {QStringLiteral("fontSizeRatio"), format.fontSizeRatio},
                {QStringLiteral("displayFontSizeRatio"), format.displayFontSizeRatio},
                {QStringLiteral("fontStretch"), format.fontStretch},
                {QStringLiteral("fontBold"), format.fontBold},
                {QStringLiteral("fontItalic"), format.fontItalic},
                {QStringLiteral("fontUnderline"), format.fontUnderline},
                {QStringLiteral("letterSpacingRatio"), format.letterSpacingRatio},
                {QStringLiteral("wordSpacingRatio"), format.wordSpacingRatio},
                {QStringLiteral("sourceAdvanceRatio"), format.sourceAdvanceRatio},
                {QStringLiteral("color"), format.color.name(QColor::HexArgb)}});
            start = next;
        }
    };
    for (qsizetype position = 0; position < draft.text.size();) {
        if (!draft.text.at(position).isSpace()) {
            ++position;
            continue;
        }
        const qsizetype start = position++;
        while (position < draft.text.size() && draft.text.at(position).isSpace()) {
            ++position;
        }
        appendWhitespace(start, position);
    }
    return QJsonObject{
        {QStringLiteral("text"), draft.text},
        {QStringLiteral("paragraphs"), paragraphs},
        {QStringLiteral("sourceLineBreaks"), sourceLineBreaks},
        {QStringLiteral("whitespaceFormats"), whitespaceFormats}};
}

bool appendSpacedLineRuns(const SaveDraft &draft,
                          const QString &paragraphText,
                          const QTextLine &line,
                          qsizetype paragraphStart,
                          qreal contentLeft,
                          qreal baseline,
                          qreal pageHeight,
                          QVector<LayoutRun> &runs)
{
    const qsizetype lineStart = paragraphStart + line.textStart();
    const qsizetype lineEnd = qMin(draft.text.size(), lineStart + line.textLength());
    if (lineEnd - lineStart > kMaximumSpacingDraftParts) {
        return false;
    }
    qsizetype firstVisible = lineStart;
    while (firstVisible < lineEnd && draft.text.at(firstVisible).isSpace()) {
        ++firstVisible;
    }
    if (firstVisible == lineEnd) {
        return true;
    }

    // A grapheme boundary alone can fall inside a shaped ligature. Intersect
    // it with Qt's glyph-cluster starts so a combining sequence or ligature
    // remains in one PDF text object.
    QSet<qsizetype> glyphStarts;
    QSet<qsizetype> rightToLeftGlyphStarts;
    QSet<qsizetype> adjustedGlyphStarts;
    const QList<QGlyphRun> glyphRuns = line.glyphRuns(
        line.textStart(), line.textLength(), QTextLayout::RetrieveStringIndexes);
    for (const QGlyphRun &glyphRun : glyphRuns) {
        const auto indexes = glyphRun.stringIndexes();
        for (qsizetype position : indexes) {
            glyphStarts.insert(paragraphStart + position);
            if (glyphRun.isRightToLeft()) {
                rightToLeftGlyphStarts.insert(paragraphStart + position);
            } else if (position >= 0 && position < paragraphText.size()) {
                const auto script = paragraphText.at(position).script();
                // Retained Latin layouts also need explicit origins when Qt's
                // replacement face has different advances from the PDF font.
                // The grapheme/cluster intersection below keeps shaping units
                // intact and the existing part budgets bound the output.
                if (script == QChar::Script_Latin || script == QChar::Script_Common)
                    adjustedGlyphStarts.insert(paragraphStart + position);
            }
        }
    }
    QSet<qsizetype> graphemeStarts;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, paragraphText);
    finder.setPosition(line.textStart());
    for (qsizetype position = finder.toNextBoundary();
         position >= 0 && paragraphStart + position < lineEnd;
         position = finder.toNextBoundary()) {
        graphemeStarts.insert(paragraphStart + position);
    }

    QVector<qsizetype> boundaries{lineStart, lineEnd};
    for (qsizetype position : std::as_const(graphemeStarts)) {
        if (position <= firstVisible || !glyphStarts.contains(position)
            || draft.text.at(position).isSpace()) {
            continue;
        }
        const SaveFormat &format = formatAt(draft, position);
        if (!qFuzzyIsNull(format.letterSpacingRatio)
            || adjustedGlyphStarts.contains(position)
            || !qFuzzyIsNull(format.sourceAdvanceRatio)
            || !qFuzzyIsNull(formatAt(draft, position - 1).sourceAdvanceRatio)
            || draft.text.at(position - 1).isSpace()) {
            boundaries.append(position);
        }
    }
    for (const SaveSpan &span : draft.characterSpans) {
        for (qsizetype position : {span.start, span.start + span.length}) {
            if (position > firstVisible && position < lineEnd
                && !draft.text.at(position).isSpace()
                && graphemeStarts.contains(position)
                && glyphStarts.contains(position)) {
                boundaries.append(position);
            }
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());
    // Symbol subsets often have a bullet but no space. Keep an inferred
    // separator with the following word when the preceding face cannot encode
    // it; the paragraph model retains the separator's own semantic format.
    QHash<QString, bool> spaceCoverage;
    const auto supportsSpace = [&](qsizetype position) {
        const SaveFormat &format = formatAt(draft, position);
        const QString key = fontFaceKey(format) + QLatin1Char('|')
            + QString::fromLatin1(format.sourceFontFingerprint.toHex());
        auto cached = spaceCoverage.constFind(key);
        if (cached == spaceCoverage.cend()) {
            const QRawFont raw(format.embeddableFontData, 16.0, QFont::PreferNoHinting);
            cached = spaceCoverage.insert(key, raw.isValid() && raw.supportsCharacter(U' '));
        }
        return cached.value();
    };
    for (qsizetype index = 1; index + 1 < boundaries.size(); ++index) {
        const qsizetype end = boundaries[index];
        qsizetype spaceStart = end;
        while (spaceStart > boundaries[index - 1]
               && draft.text.at(spaceStart - 1) == QLatin1Char(' '))
            --spaceStart;
        if (spaceStart == end || spaceStart == boundaries[index - 1])
            continue;
        if (samePositionedFormat(formatAt(draft, spaceStart - 1), formatAt(draft, end)))
            continue;
        if (!supportsSpace(spaceStart - 1) && supportsSpace(end))
            boundaries[index] = spaceStart;
    }
    const auto visibleFormatPosition = [&](qsizetype position, qsizetype end) {
        while (position + 1 < end && draft.text.at(position).isSpace())
            ++position;
        return position;
    };
    const qsizetype clusterCount = boundaries.size() - 1;
    if (clusterCount > kMaximumSpacingLineParts
        || runs.size() + clusterCount > kMaximumSpacingDraftParts) {
        return false;
    }

    const auto canPositionCharacters = [&](qsizetype start, qsizetype end) {
        for (qsizetype position = start; position < end; ++position) {
            const QChar character = draft.text.at(position);
            if (character == QLatin1Char('\t')) {
                continue;
            }
            if (!character.isPrint() || character.isSurrogate() || character.isMark()
                || (character.script() != QChar::Script_Latin
                    && character.script() != QChar::Script_Common)
                || rightToLeftGlyphStarts.contains(position)) {
                return false;
            }
            // Keep ligatures, combining sequences and shaped scripts on the
            // existing whole-cluster path. Simple spaces need no glyph mapping.
            if (!character.isSpace()
                && (!glyphStarts.contains(position)
                    || (position != lineStart && !graphemeStarts.contains(position))
                    || (position + 1 != lineEnd && !graphemeStarts.contains(position + 1)))) {
                return false;
            }
        }
        return true;
    };
    QVector<qsizetype> positionedBoundaries{lineStart};
    QVector<bool> positionedRuns;
    for (qsizetype index = 0; index < clusterCount; ++index) {
        const qsizetype start = boundaries.at(index);
        const qsizetype end = boundaries.at(index + 1);
        const bool positioned = canPositionCharacters(start, end);
        // Leave separators at the end of a word object. PDFium may collapse
        // repeated internal spaces or synthesize a space for an internal tab;
        // keeping this boundary preserves exact marked-text validation while
        // each multi-character word still establishes horizontal text flow.
        const bool merge = !positionedRuns.isEmpty() && positioned && positionedRuns.last()
            && !draft.text.at(start - 1).isSpace() && !draft.text.at(start).isSpace()
            && samePositionedFormat(
                formatAt(draft, qMax(positionedBoundaries[positionedBoundaries.size() - 2], firstVisible)),
                formatAt(draft, qMax(start, firstVisible)));
        if (merge) {
            positionedBoundaries.last() = end;
        } else {
            positionedBoundaries.append(end);
            positionedRuns.append(positioned);
        }
    }
    boundaries = std::move(positionedBoundaries);
    const qsizetype count = boundaries.size() - 1;

    const QByteArray group = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLatin1();
    for (qsizetype index = 0; index < count; ++index) {
        const qsizetype start = boundaries.at(index);
        const qsizetype end = boundaries.at(index + 1);
        const qreal startX = line.cursorToX(static_cast<int>(start - paragraphStart));
        const qreal endX = line.cursorToX(static_cast<int>(end - paragraphStart));
        LayoutRun run;
        run.textStart = start;
        run.textLength = end - start;
        run.spacingGroup = group;
        run.spacingText = draft.text.sliced(start, end - start);
        // Whitespace belongs to the preceding visible cluster. It remains
        // encoded for extraction; tabs contribute only their Qt layout advance.
        run.text = run.spacingText;
        run.text.remove(QLatin1Char('\t'));
        run.spacingStart = static_cast<int>(start - lineStart);
        run.spacingIndex = static_cast<int>(index);
        run.spacingCount = static_cast<int>(count);
        run.format = formatAt(draft, visibleFormatPosition(start, end));
        run.hasCharacterPositions = positionedRuns.at(index);
        if (run.hasCharacterPositions) {
            // Keep same-face characters in one TJ object. Singleton objects
            // otherwise make PDFium infer vertical text flow on sparse pages,
            // inserting spurious line breaks between horizontally laid out glyphs.
            bool firstEncoded = true;
            qreal encodedOrigin = 0.0;
            for (qsizetype position = start; position < end; ++position) {
                if (draft.text.at(position) == QLatin1Char('\t')) {
                    continue;
                }
                const qreal x = line.cursorToX(static_cast<int>(position - paragraphStart));
                if (!qIsFinite(x)) {
                    return false;
                }
                if (firstEncoded) {
                    encodedOrigin = x;
                    run.leadingLayoutAdvance = x - startX;
                    firstEncoded = false;
                } else {
                    run.characterPositions.append(x - encodedOrigin);
                }
            }
            if (firstEncoded || run.characterPositions.size() != run.text.size() - 1) {
                return false;
            }
        } else if (start < firstVisible) {
            run.leadingWhitespace = run.spacingText.first(firstVisible - start);
            run.leadingWhitespace.remove(QLatin1Char('\t'));
            run.leadingLayoutAdvance = line.cursorToX(
                static_cast<int>(firstVisible - paragraphStart)) - startX;
        }
        QJsonArray spacingRanges;
        qsizetype spacingStart = start;
        while (spacingStart < end) {
            const SaveFormat &spacingFormat = formatAt(draft, spacingStart);
            qsizetype spacingEnd = spacingStart + 1;
            while (spacingEnd < end) {
                const SaveFormat &nextFormat = formatAt(draft, spacingEnd);
                if (nextFormat.letterSpacingRatio != spacingFormat.letterSpacingRatio
                    || nextFormat.wordSpacingRatio != spacingFormat.wordSpacingRatio
                    || nextFormat.sourceAdvanceRatio != spacingFormat.sourceAdvanceRatio) {
                    break;
                }
                ++spacingEnd;
            }
            spacingRanges.append(QJsonArray{
                static_cast<int>(spacingStart - start),
                static_cast<int>(spacingEnd - spacingStart),
                spacingFormat.letterSpacingRatio * pageHeight,
                spacingFormat.wordSpacingRatio * pageHeight,
                spacingFormat.sourceAdvanceRatio * pageHeight});
            spacingStart = spacingEnd;
        }
        if (spacingRanges.size() > kMaximumSpacingLineParts) {
            return false;
        }
        if (spacingRanges.size() > 1) {
            run.spacingRuns = QJsonDocument(spacingRanges).toJson(QJsonDocument::Compact);
        }
        // Negative tracking can move the next LTR cursor left of this
        // cluster. Its glyph still starts at startX; using the lesser cursor
        // would shift the glyph itself and change its saved appearance.
        run.x = contentLeft + (rightToLeftGlyphStarts.contains(qMax(start, firstVisible))
            ? qMin(startX, endX) : startX);
        run.baselineFromTop = baseline;
        run.width = qMax(kMinimumObjectWidth, std::abs(endX - startX));
        run.positionedWord = true;
        runs.append(std::move(run));
    }
    return true;
}

QVector<LayoutRun> layoutRuns(const SaveDraft &draft,
                              qreal pageWidth,
                              qreal pageHeight,
                              bool &valid)
{
    valid = true;
    QVector<LayoutRun> runs;
    if (draft.text.isEmpty()) {
        return runs;
    }
    const auto validSourceAdvance = [pageHeight](const SaveFormat &format) {
        const qreal points = format.sourceAdvanceRatio * pageHeight;
        return qIsFinite(points) && qAbs(points) <= 512.0;
    };
    if (!validSourceAdvance(draft.originalFormat)
        || std::any_of(draft.characterSpans.cbegin(), draft.characterSpans.cend(),
                       [&](const SaveSpan &span) { return !validSourceAdvance(span.format); })) {
        valid = false;
        return {};
    }

    const qreal contentLeft = draft.hasBaseline && !draft.added
        ? draft.layoutOrigin.x() * pageWidth
        : draft.frameRect.x() * pageWidth;
    const qreal frameRight = draft.frameRect.right() * pageWidth;
    const qreal layoutWidth = qMax(kMinimumLayoutWidth, frameRight - contentLeft);
    const bool retainsParagraphLayout = hasParagraphLayout(draft);
    if (retainsParagraphLayout
        && (draft.text.size() > kMaximumSpacingDraftParts
            || draft.paragraphFormats.size() > kMaximumLayoutParagraphs)) {
        valid = false;
        return {};
    }

    qreal verticalOffset = 0.0;
    qreal firstBaseline = -1.0;
    qreal previousSpaceAfter = 0.0;
    qsizetype paragraphStart = 0;
    while (paragraphStart <= draft.text.size()) {
        const qsizetype newline = draft.text.indexOf(QLatin1Char('\n'), paragraphStart);
        const qsizetype paragraphEnd = newline < 0 ? draft.text.size() : newline;
        const qsizetype paragraphLength = paragraphEnd - paragraphStart;
        const bool emptyParagraph = paragraphLength == 0;
        const QString paragraphText = emptyParagraph
            ? QStringLiteral(" ")
            : draft.text.mid(paragraphStart, paragraphLength);
        const SaveFormat &baseFormat = formatAt(draft, paragraphStart);
        const SaveParagraph paragraph = paragraphFormatAt(draft, paragraphStart);
        if (!qIsFinite(paragraph.lineSpacing)
            || paragraph.lineSpacing < 0.5 || paragraph.lineSpacing > 3.0
            || !qIsFinite(paragraph.spaceBeforeRatio)
            || paragraph.spaceBeforeRatio < 0.0
            || paragraph.spaceBeforeRatio * pageHeight > 144.0 + 0.001
            || !qIsFinite(paragraph.spaceAfterRatio)
            || paragraph.spaceAfterRatio < 0.0
            || paragraph.spaceAfterRatio * pageHeight > 144.0 + 0.001
            || !qIsFinite(paragraph.firstLineIndentRatio)
            || paragraph.firstLineIndentRatio < 0.0
            || paragraph.firstLineIndentRatio * pageHeight > 144.0 + 0.001) {
            valid = false;
            return {};
        }
        // QTextDocument collapses adjacent paragraph margins to their maximum.
        // It ignores the first top margin, which is applied to topOffset below.
        if (paragraphStart > 0) {
            verticalOffset += textLayoutDistance(qMax(
                previousSpaceAfter, paragraph.spaceBeforeRatio * pageHeight));
        }
        QTextLayout layout(paragraphText, displayFont(baseFormat, pageHeight));
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(paragraph.alignment);
        layout.setTextOption(option);
        if (!emptyParagraph) {
            layout.setFormats(layoutFormats(
                draft,
                paragraphStart,
                paragraphLength,
                pageHeight));
        }

        qreal paragraphHeight = 0.0;
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            // Match QTextBlockFormat::textIndent: only the first visual line
            // moves right, while wrapped continuation lines use the full frame.
            const qreal indent = line.textStart() == 0
                ? paragraph.firstLineIndentRatio * pageHeight : 0.0;
            line.setLineWidth(qMax(kMinimumLayoutWidth, layoutWidth - indent));
            line.setPosition(QPointF(indent, paragraphHeight));
            const qreal baseline = verticalOffset + line.y() + line.ascent();
            if (firstBaseline < 0.0) {
                firstBaseline = baseline;
            }

            if (!emptyParagraph) {
                const qsizetype lineStart = paragraphStart + line.textStart();
                const qsizetype lineEnd = qMin(
                    paragraphEnd,
                    lineStart + line.textLength());
                // Retained paragraph layouts position words with Qt advances,
                // even at zero character spacing. Fitting a whole line's glyph
                // bounds would become a new font stretch after reopening and
                // make repeated saves gradually change wrapping and spacing.
                if (lineHasTextSpacing(draft, lineStart, lineEnd) || retainsParagraphLayout) {
                    if (!appendSpacedLineRuns(draft, paragraphText, line,
                                             paragraphStart, contentLeft,
                                             baseline, pageHeight, runs)) {
                        layout.endLayout();
                        valid = false;
                        return {};
                    }
                    paragraphHeight = line.y()
                        + textLayoutDistance(line.height() * paragraph.lineSpacing);
                    continue;
                }
                QVector<qsizetype> boundaries{lineStart, lineEnd};
                for (const SaveSpan &span : draft.characterSpans) {
                    const qsizetype spanStart = qBound(lineStart, span.start, lineEnd);
                    const qsizetype spanEnd = qBound(
                        lineStart,
                        span.start + span.length,
                        lineEnd);
                    boundaries.append(spanStart);
                    boundaries.append(spanEnd);
                }
                // A justified line distributes extra width between words.
                // Preserve each word's Qt position instead of scaling one
                // PDF text object across the entire justified line.
                const bool containsTab = paragraphText.mid(line.textStart(), line.textLength())
                    .contains(QLatin1Char('\t'));
                const bool justifiedLine = option.alignment().testFlag(Qt::AlignJustify)
                    && lineEnd < paragraphEnd;
                const bool positionedWords = justifiedLine || containsTab;
                if (positionedWords) {
                    for (qsizetype position = lineStart; position < lineEnd; ++position) {
                        const QChar character = draft.text.at(position);
                        // Tabs are layout advances, never font glyphs, even
                        // on the paragraph's final, unexpanded line.
                        if ((justifiedLine && character.isSpace())
                            || character == QLatin1Char('\t')) {
                            boundaries.append(position);
                            boundaries.append(position + 1);
                        }
                    }
                }
                std::sort(boundaries.begin(), boundaries.end());
                boundaries.erase(
                    std::unique(boundaries.begin(), boundaries.end()),
                    boundaries.end());

                for (qsizetype index = 0; index + 1 < boundaries.size(); ++index) {
                    const qsizetype start = boundaries.at(index);
                    const qsizetype end = boundaries.at(index + 1);
                    if (end <= start) {
                        continue;
                    }
                    const QString text = draft.text.mid(start, end - start);
                    if (text.trimmed().isEmpty()) {
                        continue;
                    }

                    const int localStart = static_cast<int>(start - paragraphStart);
                    const int localEnd = static_cast<int>(end - paragraphStart);
                    const qreal startX = line.cursorToX(localStart);
                    const qreal endX = line.cursorToX(localEnd);
                    LayoutRun run;
                    run.textStart = start;
                    run.textLength = end - start;
                    run.text = text;
                    run.format = formatAt(draft, start);
                    run.x = contentLeft + qMin(startX, endX);
                    run.baselineFromTop = baseline;
                    run.width = qMax(kMinimumObjectWidth, std::abs(endX - startX));
                    run.positionedWord = positionedWords;
                    runs.append(std::move(run));
                }
            }
            paragraphHeight = line.y()
                + textLayoutDistance(line.height() * paragraph.lineSpacing);
        }
        layout.endLayout();

        if (paragraphHeight <= 0.0) {
            const QFontMetricsF metrics(displayFont(baseFormat, pageHeight));
            if (firstBaseline < 0.0) {
                firstBaseline = verticalOffset + metrics.ascent();
            }
            paragraphHeight = textLayoutDistance(metrics.height() * paragraph.lineSpacing);
        }
        verticalOffset += paragraphHeight;
        previousSpaceAfter = paragraph.spaceAfterRatio * pageHeight;

        if (newline < 0) {
            break;
        }
        paragraphStart = newline + 1;
    }

    const qreal initialBefore = paragraphFormatAt(draft, 0).spaceBeforeRatio * pageHeight;
    const qreal topOffset = (draft.hasBaseline && !draft.added && firstBaseline >= 0.0
        ? draft.layoutOrigin.y() * pageHeight - firstBaseline
        : draft.frameRect.y() * pageHeight + 1.0) + initialBefore;
    for (LayoutRun &run : runs) {
        run.baselineFromTop += topOffset;
    }
    if (retainsParagraphLayout && !runs.isEmpty()) {
        if (runs.size() > kMaximumSpacingDraftParts) {
            valid = false;
            return {};
        }
        const QByteArray group = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLatin1();
        for (qsizetype index = 0; index < runs.size(); ++index) {
            LayoutRun &run = runs[index];
            run.paragraphGroup = group;
            run.paragraphIndex = static_cast<int>(index);
            run.paragraphCount = static_cast<int>(runs.size());
            run.paragraphText = draft.text.sliced(run.textStart, run.textLength);
        }
        LayoutRun &first = runs.first();
        first.paragraphModel = paragraphLayoutModel(draft);
        first.paragraphOrigin = draft.hasBaseline && !draft.added
            ? draft.layoutOrigin
            : QPointF(draft.frameRect.left(),
                      draft.frameRect.top() + (1.0 + firstBaseline) / pageHeight);
        first.paragraphFrame = draft.frameRect;
    }
    return runs;
}

SourceObject sourceObjectAtPath(FPDF_PAGE page, const QVector<int> &objectPath)
{
    if (!page || objectPath.isEmpty()) {
        return {};
    }

    const int topLevelIndex = objectPath.constFirst();
    if (topLevelIndex < 0 || topLevelIndex >= FPDFPage_CountObjects(page)) {
        return {};
    }

    SourceObject result;
    result.topLevelIndex = topLevelIndex;
    result.object = FPDFPage_GetObject(page, topLevelIndex);
    for (qsizetype depth = 1; result.object && depth < objectPath.size(); ++depth) {
        if (FPDFPageObj_GetType(result.object) != FPDF_PAGEOBJ_FORM) {
            return {};
        }
        const int childIndex = objectPath.at(depth);
        if (childIndex < 0 || childIndex >= FPDFFormObj_CountObjects(result.object)) {
            return {};
        }
        result.parentForm = result.object;
        result.object = FPDFFormObj_GetObject(
            result.parentForm,
            static_cast<unsigned long>(childIndex));
    }
    if (!result.object) {
        return {};
    }
    return result;
}

class FontResolver
{
public:
    struct ResolvedFont {
        enum class Source {
            None,
            Original,
            Embedded,
            Standard
        };

        FPDF_FONT handle = nullptr;
        Source source = Source::None;
    };

    explicit FontResolver(FPDF_DOCUMENT document, const QVector<SaveDraft> &drafts)
        : m_document(document)
    {
        assertPDFiumProcessLockHeld();
        // Share one encoding per complete face across pages/paragraphs. Without
        // this union, page numbers alone could embed the same font thousands
        // of times, each with a different set of digits.
        QHash<QByteArray, QSet<char32_t>> completeCharacters;
        const auto collect = [&](const SaveFormat &format, const QString &text) {
            const QByteArray &data = format.embeddableFontData;
            if (data.isEmpty() || data.startsWith("OTTO"))
                return;
            auto &characters = completeCharacters[fontProgramFingerprint(format)];
            const QRawFont rawFont = rawFontForFormat(format);
            for (char32_t character : text.toUcs4()) {
                if (characters.size() > 65535)
                    break; // Retain the overflow sentinel; validation rejects it.
                if (character != 0 && character != '\n' && character != '\t'
                    && !characters.contains(character) && rawFont.supportsCharacter(character))
                    characters.insert(character);
            }
        };
        for (const SaveDraft &draft : drafts) {
            if (draft.kind != SaveDraft::Kind::Text || draft.deleted || draft.movesSourceObjects)
                continue;
            QSet<QString> collectedFaces;
            const auto collectOnce = [&](const SaveFormat &format) {
                const QString key = embeddedFaceKey(format);
                if (!collectedFaces.contains(key)) {
                    collectedFaces.insert(key);
                    collect(format, draft.text.normalized(QString::NormalizationForm_C));
                }
            };
            collectOnce(draft.originalFormat);
            for (const SaveSpan &span : draft.characterSpans)
                collectOnce(span.format);
        }
        for (auto entry = completeCharacters.cbegin(); entry != completeCharacters.cend(); ++entry) {
            QVector<char32_t> characters(entry->cbegin(), entry->cend());
            std::sort(characters.begin(), characters.end());
            m_completeFontCharacters.insert(entry.key(), std::move(characters));
        }
    }

    ~FontResolver()
    {
        if (!m_loadedFonts.isEmpty()) {
            assertPDFiumProcessLockHeld();
        }
        for (FPDF_FONT font : std::as_const(m_loadedFonts)) {
            FPDFFont_Close(font);
        }
    }

    void beginPage()
    {
        // Source handles belong to page objects and must not key caches after
        // that page closes. Loaded replacement fonts remain document-owned.
        m_sourceFaces.clear();
        m_originalType1Faces.clear();
        m_originalObjectCharacters.clear();
        m_originalCharactersScanned = false;
        m_originalType1Limited = false;
        m_originalType1Bytes = 0;
    }

    const QList<QByteArray> &openTypeFonts() const { return m_openTypeFonts; }

    void collectOriginalType1Fonts(FPDF_PAGE page, const QVector<SourceObject> &sources,
                                   FPDF_TEXTPAGE &textPage)
    {
        for (const SourceObject &source : sources) {
            const FPDF_PAGEOBJECT textObject = PDFSourceTextGeometry::textObject(source.object);
            const FPDF_FONT font = FPDFTextObj_GetFont(textObject);
            if (!font)
                continue;
            auto found = m_originalType1Faces.find(font);
            if (found == m_originalType1Faces.end()) {
                if (m_originalType1Faces.size() >= 128) {
                    m_originalType1Limited = true;
                    continue;
                }
                OriginalType1Face face;
                size_t size = 0;
                if (FPDFFont_GetIsEmbedded(font) == 1
                    && FPDFFont_GetFontData(font, nullptr, 0, &size)
                    && size > 0 && size <= 4 * 1024 * 1024
                    && size <= 32 * 1024 * 1024 - m_originalType1Bytes) {
                    QByteArray bytes(qsizetype(size), '\0');
                    size_t actualSize = 0;
                    if (FPDFFont_GetFontData(font, reinterpret_cast<uint8_t *>(bytes.data()),
                            size, &actualSize) && actualSize == size
                        && (bytes.startsWith("%!PS-AdobeFont-1.")
                            || bytes.startsWith("%!FontType1-"))) {
                        const QRawFont raw(bytes, 16.0, QFont::PreferNoHinting);
                        if (raw.isValid()) {
                            face.fingerprint = QCryptographicHash::hash(
                                bytes, QCryptographicHash::Sha256);
                            face.bold = raw.weight() >= QFont::Bold;
                            face.italic = raw.style() != QFont::StyleNormal;
                        }
                    }
                    m_originalType1Bytes += size;
                } else if (size > 0) {
                    m_originalType1Limited = true;
                }
                found = m_originalType1Faces.insert(font, std::move(face));
            }
            if (found->fingerprint.isEmpty())
                continue;
            if (!m_originalCharactersScanned) {
                m_originalCharactersScanned = true;
                textPage = FPDFText_LoadPage(page);
                const int count = textPage ? FPDFText_CountChars(textPage) : -1;
                if (count >= 0 && count <= 50000) {
                    for (int index = 0; index < count; ++index) {
                        // Inferred spaces are not evidence that this PDF font's
                        // custom encoding has a usable space character.
                        if (FPDFText_IsGenerated(textPage, index) != 0)
                            continue;
                        const FPDF_PAGEOBJECT object = FPDFText_GetTextObject(textPage, index);
                        const uint character = FPDFText_GetUnicode(textPage, index);
                        if (object && character > 0 && character <= 0x10ffffU
                            && !(character >= 0xd800U && character <= 0xdfffU))
                            m_originalObjectCharacters[object].insert(character);
                    }
                }
            }
            found->characters.unite(m_originalObjectCharacters.value(textObject));
        }
    }

    bool prepareOriginalType1Font(SaveFormat &format, const QString &text,
                                  const QVector<FPDF_FONT> &sourceFonts,
                                  bool &knownSourceFace,
                                  uint &unsupportedCodePoint) const
    {
        format.originalType1Font = nullptr;
        if (format.displaySourceFingerprint.isEmpty())
            return false;
        // On a bounded-scan failure an exact local face can still satisfy the
        // request, but a guessed standard font cannot establish source identity.
        knownSourceFace = m_originalType1Limited;
        for (FPDF_FONT font : sourceFonts) {
            const auto face = m_originalType1Faces.constFind(font);
            if (face == m_originalType1Faces.cend()
                || face->fingerprint != format.displaySourceFingerprint)
                continue;
            knownSourceFace = true;
            if (face->bold != format.fontBold || face->italic != format.fontItalic)
                continue;
            uint missing = 0;
            for (uint character : text.toUcs4()) {
                if (character != '\n' && character != '\t'
                    && !face->characters.contains(character)) {
                    missing = character;
                    break;
                }
            }
            if (missing) {
                if (!unsupportedCodePoint)
                    unsupportedCodePoint = missing;
                continue;
            }
            format.originalType1Font = font;
            format.allowOriginalFontReuse = true;
            format.forceEmbeddedFont = false;
            unsupportedCodePoint = 0;
            return true;
        }
        return false;
    }

    bool originalType1FontContainsText(FPDF_FONT font, const QString &text) const
    {
        const auto face = m_originalType1Faces.constFind(font);
        if (face == m_originalType1Faces.cend())
            return false;
        const auto characters = text.toUcs4();
        return std::all_of(characters.cbegin(), characters.cend(), [&](uint character) {
            return face->characters.contains(character);
        });
    }

    bool prepareSourceFace(SaveFormat &format, const QString &text,
                           const QVector<FPDF_FONT> &sourceFonts,
                           uint &unsupportedCodePoint)
    {
        for (FPDF_FONT font : sourceFonts) {
            auto face = m_sourceFaces.constFind(font);
            if (face == m_sourceFaces.cend()) {
                if (m_sourceFaces.size() >= 128)
                    continue;
                face = m_sourceFaces.insert(font, readSourceFace(font));
            }
            if (face->data.isEmpty()
                || (!format.displaySourceFingerprint.isEmpty()
                    ? format.displaySourceFingerprint != face->fingerprint
                    : !equivalentFamily(format.semanticFontFamily, face->rawFont.familyName()))
                || format.fontBold != (face->rawFont.weight() >= QFont::Bold)
                || format.fontItalic != (face->rawFont.style() != QFont::StyleNormal))
                continue;
            uint missing = 0;
            for (uint character : text.toUcs4()) {
                // LF/tabs are layout controls; spaces need real cmap entries.
                if (character != '\n' && character != '\t'
                    && !face->rawFont.supportsCharacter(character)) {
                    missing = character;
                    break;
                }
            }
            if (missing) {
                if (!unsupportedCodePoint)
                    unsupportedCodePoint = missing;
                continue;
            }
            format.embeddableFontFamily = face->rawFont.familyName();
            format.embeddableFontData = face->data;
            // Full OpenType/CFF programs use their Unicode cmap through the
            // Type1 loader. The explicit CID-to-GID subset loader is TrueType
            // only; never send CFF through FPDFText_LoadCidType2Font.
            format.sourceFontFingerprint = face->data.startsWith("OTTO")
                ? QByteArray() : face->fingerprint;
            format.forceEmbeddedFont = true;
            unsupportedCodePoint = 0;
            return true;
        }
        return false;
    }

    QString embeddedFamilyForText(const SaveFormat &format,
                                  const QString &text,
                                  uint &unsupportedCodePoint)
    {
        unsupportedCodePoint = 0;
        if (format.embeddableFontFamily.isEmpty()
            || format.embeddableFontData.isEmpty()) {
            return {};
        }

        SaveFormat embeddedFormat = format;
        embeddedFormat.fontFamily = format.embeddableFontFamily;
        return embeddedFontSupportsText(
                   embeddedFormat,
                   text,
                   unsupportedCodePoint)
            ? format.embeddableFontFamily
            : QString();
    }

    ResolvedFont resolve(const SaveFormat &format,
                         const SaveFormat &originalFormat,
                         FPDF_FONT originalFont)
    {
        if (!format.forceEmbeddedFont && format.allowOriginalFontReuse
            && format.originalType1Font)
            return {format.originalType1Font, ResolvedFont::Source::Original};
        if (!format.forceEmbeddedFont
            && format.allowOriginalFontReuse
            && originalFont
            && canReuseOriginalFont(format, originalFormat)) {
            return {originalFont, ResolvedFont::Source::Original};
        }

        if (FPDF_FONT font = loadEmbeddedFont(format)) {
            return {font, ResolvedFont::Source::Embedded};
        }
        if (format.forceEmbeddedFont
            || !format.embeddableFontFamily.isEmpty()) {
            return {};
        }

        const QByteArray standardName = standardFontName(format).toLatin1();
        if (standardName.isEmpty()) {
            return {};
        }
        const QString key = QStringLiteral("standard|")
            + QString::fromLatin1(standardName);
        const auto cached = m_fontCache.constFind(key);
        if (cached != m_fontCache.cend()) {
            return {cached.value(), ResolvedFont::Source::Standard};
        }
        FPDF_FONT font = FPDFText_LoadStandardFont(
            m_document,
            standardName.constData());
        if (font) {
            m_fontCache.insert(key, font);
            m_loadedFonts.append(font);
        }
        return {
            font,
            font
                ? ResolvedFont::Source::Standard
                : ResolvedFont::Source::None
        };
    }

    Q_DISABLE_COPY_MOVE(FontResolver)

    QByteArray embeddedEncodingKey(const SaveFormat &format)
    {
        if (!format.sourceFontFingerprint.isEmpty())
            return format.sourceFontFingerprint;
        if (!format.embeddedFontCharacters.isEmpty())
            return fontProgramFingerprint(format);
        return {};
    }

    QVector<char32_t> completeFontCharacters(const QByteArray &key) const
    {
        return m_completeFontCharacters.value(key);
    }

    bool prepareEmbeddedText(SaveFormat &format, const QString &text,
                             uint &unsupportedCodePoint)
    {
        format.embeddedFontCharacters.clear();
        format.embeddedCharactersDigest.clear();
        if (format.sourceFontFingerprint.isEmpty()
            && ((!format.forceEmbeddedFont && format.allowOriginalFontReuse)
                || format.embeddableFontData.isEmpty()
                || format.embeddableFontData.startsWith("OTTO")))
            return true;
        const QRawFont rawFont = rawFontForFormat(format);
        if (!rawFont.isValid())
            return false;
        // PDFium 8086 resolves duplicate ToUnicode entries with last-wins.
        // Give every Unicode scalar its own CID, including complete local faces:
        // space/NBSP and punctuation aliases must not change after saving.
        // Validate the final layout run, which can add inferred separators.
        auto &characters = format.embeddedFontCharacters;
        for (uint character : text.toUcs4()) {
            if (!rawFont.supportsCharacter(character)) {
                unsupportedCodePoint = character;
                return false;
            }
            characters.append(static_cast<char32_t>(character));
        }
        std::sort(characters.begin(), characters.end());
        characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
        return !characters.isEmpty() && characters.size() <= 65535;
    }

    static bool setText(FPDF_PAGEOBJECT object, const SaveFormat &format,
                        const QString &text)
    {
        if (format.embeddedFontCharacters.isEmpty())
            return FPDFText_SetText(object,
                reinterpret_cast<const FPDF_WCHAR *>(text.utf16()));
        return PDFUnicodeFont::setText(object, format.embeddedFontCharacters, text);
    }

private:
    struct OriginalType1Face {
        QByteArray fingerprint;
        QSet<uint> characters;
        bool bold = false;
        bool italic = false;
    };

    struct SourceFace {
        QByteArray data;
        QByteArray fingerprint;
        QRawFont rawFont;
    };

    SourceFace readSourceFace(FPDF_FONT font)
    {
        constexpr size_t maximumFaceBytes = 4 * 1024 * 1024;
        constexpr qsizetype maximumSourceBytes = 32 * 1024 * 1024;
        size_t size = 0;
        // Never re-embed PDFium's substitute for an unembedded source font.
        if (!font || FPDFFont_GetIsEmbedded(font) != 1
            || !FPDFFont_GetFontData(font, nullptr, 0, &size)
            || size == 0 || size > maximumFaceBytes)
            return {};
        SourceFace face;
        face.data.resize(static_cast<qsizetype>(size));
        size_t actualSize = 0;
        if (!FPDFFont_GetFontData(font, reinterpret_cast<uint8_t *>(face.data.data()),
                                  size, &actualSize) || actualSize != size)
            return {};
        face.data = PDFSymbolFont::withUnicodeBullets(font, face.data);
        face.fingerprint = QCryptographicHash::hash(face.data, QCryptographicHash::Sha256);
        const auto existing = m_sourceData.constFind(face.fingerprint);
        if (existing != m_sourceData.cend())
            return existing.value();
        if (face.data.size() > maximumSourceBytes - m_sourceBytes)
            return {};
        face.rawFont = QRawFont(face.data, 16.0, QFont::PreferNoHinting);
        // A complete static OpenType/CFF source can be reused even after the
        // app cache is removed. It goes through the same bounded validation as
        // downloaded CFF and the post-serialization FontFile3 finalizer.
        if (face.data.startsWith("OTTO")) {
            QString error;
            if (!ApplicationFonts::validateDownloadedFontData(face.data,
                    face.rawFont.familyName(), face.rawFont.weight() >= QFont::Bold,
                    face.rawFont.style() != QFont::StyleNormal, 0, error))
                return {};
            m_sourceBytes += face.data.size();
            m_sourceData.insert(face.fingerprint, face);
            return face;
        }
        // The subset loader below accepts TrueType outlines. Limit it to
        // editable, subsettable static faces with a reliable Unicode cmap.
        if (!face.rawFont.isValid() || face.rawFont.fontTable("glyf").isEmpty()
            || face.rawFont.fontTable("loca").isEmpty()
            || face.rawFont.fontTable("cmap").isEmpty()
            || !face.rawFont.fontTable("fvar").isEmpty())
            return {};
        const QByteArray os2 = face.rawFont.fontTable("OS/2");
        if (os2.size() < 10)
            return {};
        const auto u16 = [&os2](int offset) {
            return (quint16(quint8(os2[offset])) << 8) | quint8(os2[offset + 1]);
        };
        const quint16 version = u16(0);
        const quint16 flags = u16(8);
        const quint16 permission = flags & 0x000fU;
        if ((flags & 0x0301U) != 0 // no subsetting, bitmap-only, or reserved bit
            || (version >= 2 && (flags & 0xfcf0U) != 0)
            || (permission != 0 && (version <= 2 ? !(permission & 0x0008U)
                                                    : permission != 0x0008U)))
            return {};
        m_sourceBytes += face.data.size();
        m_sourceData.insert(face.fingerprint, face);
        return face;
    }

    static QString embeddedFaceKey(const SaveFormat &format)
    {
        return fontFaceKey(format) + QLatin1Char('|')
            + QString::fromLatin1(format.sourceFontFingerprint.toHex());
    }

    QByteArray fontProgramFingerprint(const SaveFormat &format)
    {
        const QString key = embeddedFaceKey(format);
        auto fingerprint = m_fontProgramFingerprintCache.constFind(key);
        if (fingerprint == m_fontProgramFingerprintCache.cend()) {
            fingerprint = m_fontProgramFingerprintCache.insert(key, QCryptographicHash::hash(
                format.embeddableFontData, QCryptographicHash::Sha256));
        }
        return *fingerprint;
    }

    QRawFont rawFontForFormat(const SaveFormat &format)
    {
        const QString key = embeddedFaceKey(format);
        auto rawFont = m_rawFontCache.constFind(key);
        if (rawFont == m_rawFontCache.cend()) {
            const QByteArray &fontData = format.embeddableFontData;
            rawFont = m_rawFontCache.insert(key, fontData.isEmpty()
                ? QRawFont() : QRawFont(fontData, 16.0, QFont::PreferNoHinting));
        }
        return *rawFont;
    }

    bool embeddedFontSupportsText(const SaveFormat &format,
                                  const QString &text,
                                  uint &unsupportedCodePoint)
    {
        const QRawFont rawFont = rawFontForFormat(format);
        if (!rawFont.isValid())
            return false;
        for (uint character : text.toUcs4()) {
            if (!QChar::isSpace(static_cast<char32_t>(character))
                && !rawFont.supportsCharacter(character)) {
                unsupportedCodePoint = character;
                return false;
            }
        }
        return true;
    }

    FPDF_FONT loadEmbeddedFont(const SaveFormat &format)
    {
        QString key = QStringLiteral("embedded|") + embeddedFaceKey(format);
        if (!format.embeddedFontCharacters.isEmpty()) {
            const QByteArray digest = format.embeddedCharactersDigest.isEmpty()
                ? QCryptographicHash::hash(
                    QString::fromUcs4(format.embeddedFontCharacters.constData(),
                                     format.embeddedFontCharacters.size()).toUtf8(),
                    QCryptographicHash::Sha256)
                : format.embeddedCharactersDigest;
            key += QLatin1Char('|') + QString::fromLatin1(digest.toHex());
        }
        const auto cached = m_fontCache.constFind(key);
        if (cached != m_fontCache.cend()) {
            return cached.value();
        }

        const QByteArray &originalData = format.embeddableFontData;
        // PDFium's page generator identifies font resources by BaseFont and
        // subtype, not by the font dictionary or CID mapping. Keep separately
        // loaded TrueType programs distinct even when their family is equal.
        const QByteArray fontData = originalData.startsWith("OTTO")
                || !format.embeddedFontCharacters.isEmpty()
            ? originalData
            : PDFFontAliases::withSaveResourceName(originalData,
                QByteArrayLiteral("complete|")
                    + fontProgramFingerprint(format));
        if (fontData.isEmpty()) {
            return nullptr;
        }
        FPDF_FONT font = format.embeddedFontCharacters.isEmpty()
            ? FPDFText_LoadFont(m_document,
                reinterpret_cast<const uint8_t *>(fontData.constData()),
                static_cast<uint32_t>(fontData.size()),
                fontData.startsWith("OTTO") ? FPDF_FONT_TYPE1 : FPDF_FONT_TRUETYPE, true)
            : loadUnicodeFont(format);
        if (font) {
            m_fontCache.insert(key, font);
            m_loadedFonts.append(font);
            if (fontData.startsWith("OTTO") && !m_openTypeFonts.contains(fontData))
                m_openTypeFonts.append(fontData);
        }
        return font;
    }

    FPDF_FONT loadUnicodeFont(const SaveFormat &format)
    {
        constexpr qsizetype maximumLoadedSourceBytes = 32 * 1024 * 1024;
        const bool sourceFace = !format.sourceFontFingerprint.isEmpty();
        const QByteArray &originalData = format.embeddableFontData;
        if (sourceFace && originalData.size() > maximumLoadedSourceBytes - m_loadedSourceBytes)
            return nullptr;
        const auto &characters = format.embeddedFontCharacters;
        const QByteArray identity = QByteArrayLiteral("unicode|") + embeddedEncodingKey(format)
            + QString::fromUcs4(characters.constData(), characters.size()).toUtf8();
        const QByteArray fontData = PDFFontAliases::withSaveResourceName(originalData, identity);
        if (fontData.isEmpty())
            return nullptr;
        FPDF_FONT font = PDFUnicodeFont::loadTrueType(
            m_document, fontData, rawFontForFormat(format), characters);
        if (font && sourceFace)
            m_loadedSourceBytes += originalData.size();
        return font;
    }

    FPDF_DOCUMENT m_document = nullptr;
    QHash<QString, QRawFont> m_rawFontCache;
    QHash<QString, QByteArray> m_fontProgramFingerprintCache;
    QHash<QByteArray, QVector<char32_t>> m_completeFontCharacters;
    QHash<QString, FPDF_FONT> m_fontCache;
    QHash<FPDF_FONT, SourceFace> m_sourceFaces;
    QHash<FPDF_FONT, OriginalType1Face> m_originalType1Faces;
    QHash<FPDF_PAGEOBJECT, QSet<uint>> m_originalObjectCharacters;
    bool m_originalCharactersScanned = false;
    bool m_originalType1Limited = false;
    size_t m_originalType1Bytes = 0;
    QHash<QByteArray, SourceFace> m_sourceData;
    qsizetype m_sourceBytes = 0;
    qsizetype m_loadedSourceBytes = 0;
    QVector<FPDF_FONT> m_loadedFonts;
    QList<QByteArray> m_openTypeFonts;
};

PDFSaveWriter::Result failure(PDFSaveWriter::Error error,
                              int page = -1,
                              const QString &detail = {},
                              uint unsupportedCodePoint = 0)
{
    PDFSaveWriter::Result result;
    result.error = error;
    result.page = page;
    result.detail = detail;
    result.unsupportedCodePoint = unsupportedCodePoint;
    return result;
}

PDFSaveWriter::Result fontFailure(const SaveFormat &format, int page,
                                  uint unsupportedCodePoint = 0)
{
    auto result = failure(PDFSaveWriter::Error::FontUnavailable, page,
                          format.semanticFontFamily, unsupportedCodePoint);
    result.fontBold = format.fontBold;
    result.fontItalic = format.fontItalic;
    // A complete face already checked and lacking the glyph will not be fixed
    // by downloading that same face again.
    result.fontDownloadSuggested = !format.semanticFontFamily.trimmed().isEmpty()
        && (format.embeddableFontData.isEmpty() || !format.sourceFontFingerprint.isEmpty());
    return result;
}

struct RasterizedPaint {
    int topLevelIndex = -1;
    FPDF_PAGEOBJECT sourceObject = nullptr;
    FPDF_PAGEOBJECT replacementObject = nullptr;
    FPDF_BITMAP bitmap = nullptr;
};

class RasterizedPaints
{
public:
    RasterizedPaints() = default;

    ~RasterizedPaints()
    {
        if (!m_paints.isEmpty()) {
            assertPDFiumProcessLockHeld();
        }
        for (RasterizedPaint &paint : m_paints) {
            if (paint.replacementObject) {
                FPDFPageObj_Destroy(paint.replacementObject);
            }
            if (paint.bitmap) {
                FPDFBitmap_Destroy(paint.bitmap);
            }
        }
    }

    QVector<RasterizedPaint> &values()
    {
        return m_paints;
    }

    Q_DISABLE_COPY_MOVE(RasterizedPaints)

private:
    QVector<RasterizedPaint> m_paints;
};

bool restorePageObjectActivity(
    const QVector<FPDF_PAGEOBJECT> &objects,
    const QVector<FPDF_BOOL> &activeStates)
{
    assertPDFiumProcessLockHeld();
    bool restored = objects.size() == activeStates.size();
    const qsizetype count = qMin(objects.size(), activeStates.size());
    for (qsizetype index = 0; index < count; ++index) {
        FPDF_BOOL currentActive = false;
        if (!FPDFPageObj_GetIsActive(objects.at(index), &currentActive)
            || (currentActive != activeStates.at(index)
                && !FPDFPageObj_SetIsActive(
                    objects.at(index),
                    activeStates.at(index)))) {
            restored = false;
        }
    }
    return restored;
}

bool renderIsolatedPageObject(FPDF_PAGE page,
                              int objectIndex,
                              QImage &image,
                              FS_MATRIX &imageMatrix)
{
    assertPDFiumProcessLockHeld();
    const int objectCount = FPDFPage_CountObjects(page);
    if (objectIndex < 0 || objectIndex >= objectCount) {
        return false;
    }

    QVector<FPDF_PAGEOBJECT> objects;
    QVector<FPDF_BOOL> activeStates;
    objects.reserve(objectCount);
    activeStates.reserve(objectCount);
    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
        FPDF_BOOL active = false;
        if (!object || !FPDFPageObj_GetIsActive(object, &active)) {
            return false;
        }
        objects.append(object);
        activeStates.append(active);
    }
    if (!activeStates.at(objectIndex)) {
        return false;
    }

    bool isolated = true;
    for (int index = 0; index < objectCount; ++index) {
        const FPDF_BOOL desiredActive = index == objectIndex;
        if (activeStates.at(index) != desiredActive
            && !FPDFPageObj_SetIsActive(objects.at(index), desiredActive)) {
            isolated = false;
            break;
        }
    }
    if (!isolated) {
        restorePageObjectActivity(objects, activeStates);
        return false;
    }

    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    FPDF_PAGEOBJECT object = objects.at(objectIndex);
    const bool boundsAvailable = FPDFPageObj_GetBounds(
        object,
        &left,
        &bottom,
        &right,
        &top);
    const qreal width = static_cast<qreal>(right - left);
    const qreal height = static_cast<qreal>(top - bottom);
    if (!boundsAvailable
        || !std::isfinite(width)
        || !std::isfinite(height)
        || width <= 0.0
        || height <= 0.0) {
        restorePageObjectActivity(objects, activeStates);
        return false;
    }

    const qreal pageWidth = FPDF_GetPageWidthF(page);
    const qreal pageHeight = FPDF_GetPageHeightF(page);
    if (!std::isfinite(pageWidth)
        || !std::isfinite(pageHeight)
        || pageWidth <= 0.0
        || pageHeight <= 0.0) {
        restorePageObjectActivity(objects, activeStates);
        return false;
    }

    qreal scale = kUnsupportedPaintRasterScale;
    const qreal requestedPixels = pageWidth * pageHeight * scale * scale;
    if (requestedPixels > kMaximumUnsupportedPaintPixels) {
        scale = std::sqrt(
            static_cast<qreal>(kMaximumUnsupportedPaintPixels)
            / (pageWidth * pageHeight));
    }
    if (!std::isfinite(scale) || scale < 0.25) {
        restorePageObjectActivity(objects, activeStates);
        return false;
    }

    const int pagePixelWidth = qCeil(pageWidth * scale);
    const int pagePixelHeight = qCeil(pageHeight * scale);
    if (pagePixelWidth <= 0
        || pagePixelHeight <= 0
        || static_cast<qint64>(pagePixelWidth) * pagePixelHeight
            > kMaximumUnsupportedPaintPixels + 16384) {
        restorePageObjectActivity(objects, activeStates);
        return false;
    }

    QImage renderedPage(
        pagePixelWidth,
        pagePixelHeight,
        QImage::Format_ARGB32);
    renderedPage.fill(Qt::transparent);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        pagePixelWidth,
        pagePixelHeight,
        FPDFBitmap_BGRA,
        renderedPage.bits(),
        renderedPage.bytesPerLine());
    if (!bitmap) {
        restorePageObjectActivity(objects, activeStates);
        return false;
    }
    FPDF_RenderPageBitmap(
        bitmap,
        page,
        0,
        0,
        pagePixelWidth,
        pagePixelHeight,
        0,
        0);
    FPDFBitmap_Destroy(bitmap);

    if (!restorePageObjectActivity(objects, activeStates)) {
        image = {};
        return false;
    }

    const qreal padding = 2.0 / scale;
    const qreal paddedLeft = static_cast<qreal>(left) - padding;
    const qreal paddedBottom = static_cast<qreal>(bottom) - padding;
    const qreal paddedRight = static_cast<qreal>(right) + padding;
    const qreal paddedTop = static_cast<qreal>(top) + padding;
    const QPointF pageCorners[] = {
        QPointF(paddedLeft, paddedBottom),
        QPointF(paddedRight, paddedBottom),
        QPointF(paddedRight, paddedTop),
        QPointF(paddedLeft, paddedTop),
    };
    int minimumX = pagePixelWidth;
    int minimumY = pagePixelHeight;
    int maximumX = 0;
    int maximumY = 0;
    for (const QPointF &corner : pageCorners) {
        int deviceX = 0;
        int deviceY = 0;
        if (!FPDF_PageToDevice(
                page,
                0,
                0,
                pagePixelWidth,
                pagePixelHeight,
                0,
                corner.x(),
                corner.y(),
                &deviceX,
                &deviceY)) {
            return false;
        }
        minimumX = qMin(minimumX, deviceX);
        minimumY = qMin(minimumY, deviceY);
        maximumX = qMax(maximumX, deviceX);
        maximumY = qMax(maximumY, deviceY);
    }
    const QRect crop = QRect(
        QPoint(minimumX, minimumY),
        QPoint(maximumX, maximumY))
                           .normalized()
                           .intersected(renderedPage.rect());
    if (crop.isEmpty()) {
        return false;
    }
    image = renderedPage.copy(crop);

    double bottomLeftX = 0.0;
    double bottomLeftY = 0.0;
    double bottomRightX = 0.0;
    double bottomRightY = 0.0;
    double topLeftX = 0.0;
    double topLeftY = 0.0;
    const int cropRight = crop.x() + crop.width();
    const int cropBottom = crop.y() + crop.height();
    if (!FPDF_DeviceToPage(
            page,
            0,
            0,
            pagePixelWidth,
            pagePixelHeight,
            0,
            crop.left(),
            cropBottom,
            &bottomLeftX,
            &bottomLeftY)
        || !FPDF_DeviceToPage(
            page,
            0,
            0,
            pagePixelWidth,
            pagePixelHeight,
            0,
            cropRight,
            cropBottom,
            &bottomRightX,
            &bottomRightY)
        || !FPDF_DeviceToPage(
            page,
            0,
            0,
            pagePixelWidth,
            pagePixelHeight,
            0,
            crop.left(),
            crop.top(),
            &topLeftX,
            &topLeftY)) {
        image = {};
        return false;
    }
    imageMatrix = FS_MATRIX{
        static_cast<float>(bottomRightX - bottomLeftX),
        static_cast<float>(bottomRightY - bottomLeftY),
        static_cast<float>(topLeftX - bottomLeftX),
        static_cast<float>(topLeftY - bottomLeftY),
        static_cast<float>(bottomLeftX),
        static_cast<float>(bottomLeftY)};
    return true;
}

bool colorsApproximatelyEqual(const QColor &left, const QColor &right)
{
    return qAbs(left.red() - right.red()) <= kPaintColorTolerance
        && qAbs(left.green() - right.green()) <= kPaintColorTolerance
        && qAbs(left.blue() - right.blue()) <= kPaintColorTolerance;
}

bool renderedPaintDiffersFromSerializableColors(FPDF_PAGEOBJECT object,
                                                const QImage &image)
{
    unsigned int fillRed = 0;
    unsigned int fillGreen = 0;
    unsigned int fillBlue = 0;
    unsigned int fillAlpha = 0;
    const bool fillAvailable = FPDFPageObj_GetFillColor(
        object,
        &fillRed,
        &fillGreen,
        &fillBlue,
        &fillAlpha);
    const QColor fillColor(
        static_cast<int>(fillRed),
        static_cast<int>(fillGreen),
        static_cast<int>(fillBlue),
        static_cast<int>(fillAlpha));

    FPDF_BOOL strokes = false;
    int fillMode = FPDF_FILLMODE_NONE;
    const bool drawModeAvailable = FPDFPath_GetDrawMode(
        object,
        &fillMode,
        &strokes);
    unsigned int strokeRed = 0;
    unsigned int strokeGreen = 0;
    unsigned int strokeBlue = 0;
    unsigned int strokeAlpha = 0;
    const bool strokeAvailable = drawModeAvailable
        && strokes
        && FPDFPageObj_GetStrokeColor(
            object,
            &strokeRed,
            &strokeGreen,
            &strokeBlue,
            &strokeAlpha);
    const QColor strokeColor(
        static_cast<int>(strokeRed),
        static_cast<int>(strokeGreen),
        static_cast<int>(strokeBlue),
        static_cast<int>(strokeAlpha));

    int opaquePixels = 0;
    int unexpectedPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = QColor::fromRgba(line[x]);
            if (color.alpha() < 240) {
                continue;
            }
            ++opaquePixels;
            const bool matchesFill = fillAvailable
                && colorsApproximatelyEqual(color, fillColor);
            const bool matchesStroke = strokeAvailable
                && colorsApproximatelyEqual(color, strokeColor);
            if (!matchesFill && !matchesStroke) {
                ++unexpectedPixels;
            }
        }
    }
    if (opaquePixels == 0) {
        return false;
    }
    return !fillAvailable
        || unexpectedPixels >= qMax(8, opaquePixels / 200);
}

bool materializeWhitePathFill(FPDF_PAGEOBJECT inspectionObject,
                              FPDF_PAGEOBJECT sourceObject)
{
    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL strokes = false;
    if (!FPDFPath_GetDrawMode(inspectionObject, &fillMode, &strokes)) {
        return false;
    }
    if (fillMode == FPDF_FILLMODE_NONE) {
        return true;
    }

    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int alpha = 0;
    if (!FPDFPageObj_GetFillColor(
            inspectionObject,
            &red,
            &green,
            &blue,
            &alpha)) {
        return false;
    }
    if (red != 255 || green != 255 || blue != 255 || alpha != 255) {
        return true;
    }

    // Word-authored PDFs can expose a white DeviceGray path correctly while
    // loaded, then let PDFium serialize that implicit paint as RGB black after
    // nearby source objects are replaced. Marking only exact opaque-white
    // fills explicitly avoids changing CMYK/ICC/spot paints with real color.
    return FPDFPageObj_SetFillColor(
        sourceObject,
        red,
        green,
        blue,
        alpha);
}

bool materializeTextObjectPaint(FPDF_PAGEOBJECT inspectionObject,
                                FPDF_PAGEOBJECT sourceObject)
{
    const FPDF_TEXT_RENDERMODE renderMode
        = FPDFTextObj_GetTextRenderMode(inspectionObject);
    bool usesFill = false;
    bool usesStroke = false;
    switch (renderMode) {
    case FPDF_TEXTRENDERMODE_FILL:
    case FPDF_TEXTRENDERMODE_FILL_CLIP:
        usesFill = true;
        break;
    case FPDF_TEXTRENDERMODE_STROKE:
    case FPDF_TEXTRENDERMODE_STROKE_CLIP:
        usesStroke = true;
        break;
    case FPDF_TEXTRENDERMODE_FILL_STROKE:
    case FPDF_TEXTRENDERMODE_FILL_STROKE_CLIP:
        usesFill = true;
        usesStroke = true;
        break;
    case FPDF_TEXTRENDERMODE_INVISIBLE:
    case FPDF_TEXTRENDERMODE_CLIP:
        return true;
    case FPDF_TEXTRENDERMODE_UNKNOWN:
    default:
        return false;
    }

    if (usesFill) {
        unsigned int red = 0;
        unsigned int green = 0;
        unsigned int blue = 0;
        unsigned int alpha = 0;
        if (!FPDFPageObj_GetFillColor(
                inspectionObject,
                &red,
                &green,
                &blue,
                &alpha)
            || !FPDFPageObj_SetFillColor(
                sourceObject,
                red,
                green,
                blue,
                alpha)) {
            return false;
        }
    }
    if (usesStroke) {
        unsigned int red = 0;
        unsigned int green = 0;
        unsigned int blue = 0;
        unsigned int alpha = 0;
        if (!FPDFPageObj_GetStrokeColor(
                inspectionObject,
                &red,
                &green,
                &blue,
                &alpha)
            || !FPDFPageObj_SetStrokeColor(
                sourceObject,
                red,
                green,
                blue,
                alpha)) {
            return false;
        }
    }
    return true;
}

PDFSaveWriter::Result materializeTextPaints(
    FPDF_PAGE page,
    FPDF_PAGE inspectionPage,
    int pageIndex,
    const QSet<int> &removedTextObjectIndexes)
{
    assertPDFiumProcessLockHeld();
    const int objectCount = FPDFPage_CountObjects(page);
    if (objectCount != FPDFPage_CountObjects(inspectionPage)) {
        return failure(PDFSaveWriter::Error::SourceObjectMissing, pageIndex);
    }

    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT inspectionObject = FPDFPage_GetObject(
            inspectionPage,
            index);
        FPDF_PAGEOBJECT sourceObject = FPDFPage_GetObject(page, index);
        if (!inspectionObject
            || !sourceObject
            || FPDFPageObj_GetType(inspectionObject)
                != FPDFPageObj_GetType(sourceObject)) {
            return failure(PDFSaveWriter::Error::SourceObjectMissing, pageIndex);
        }
        if (removedTextObjectIndexes.contains(index)
            || FPDFPageObj_GetType(inspectionObject) != FPDF_PAGEOBJ_TEXT) {
            continue;
        }

        // PDFium can resolve an inherited ICC/CMYK text paint while parsing but
        // omit it when regenerating the page content stream. Materialize the
        // effective RGBA on the writer object before that regeneration.
        if (!materializeTextObjectPaint(inspectionObject, sourceObject)) {
            return failure(
                PDFSaveWriter::Error::PageContentGenerationFailed,
                pageIndex);
        }
    }
    return {};
}

bool createRasterizedPaint(FPDF_DOCUMENT document,
                           FPDF_PAGE page,
                           FPDF_PAGEOBJECT sourceObject,
                           int topLevelIndex,
                           const QImage &sourceImage,
                           const FS_MATRIX &imageMatrix,
                           RasterizedPaints &paints)
{
    assertPDFiumProcessLockHeld();
    const QImage image = sourceImage.format() == QImage::Format_ARGB32
        ? sourceImage
        : sourceImage.convertToFormat(QImage::Format_ARGB32);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        image.width(),
        image.height(),
        FPDFBitmap_BGRA,
        nullptr,
        0);
    if (!bitmap) {
        return false;
    }

    void *const bitmapBuffer = FPDFBitmap_GetBuffer(bitmap);
    const int bitmapStride = FPDFBitmap_GetStride(bitmap);
    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 4;
    if (!bitmapBuffer || bitmapStride < rowBytes) {
        FPDFBitmap_Destroy(bitmap);
        return false;
    }
    for (int row = 0; row < image.height(); ++row) {
        std::memcpy(
            static_cast<uchar *>(bitmapBuffer)
                + static_cast<qsizetype>(row) * bitmapStride,
            image.constScanLine(row),
            static_cast<size_t>(rowBytes));
    }

    FPDF_PAGEOBJECT imageObject = FPDFPageObj_NewImageObj(document);
    FPDF_PAGE loadedPages[] = {page};
    const bool configured = imageObject
        && FPDFPageObj_AddMark(imageObject, kPreservedPaintMark)
        && FPDFImageObj_SetBitmap(loadedPages, 1, imageObject, bitmap)
        && FPDFPageObj_SetMatrix(imageObject, &imageMatrix);
    if (!configured) {
        if (imageObject) {
            FPDFPageObj_Destroy(imageObject);
        }
        FPDFBitmap_Destroy(bitmap);
        return false;
    }

    paints.values().append(RasterizedPaint{
        topLevelIndex,
        sourceObject,
        imageObject,
        bitmap});
    return true;
}

PDFSaveWriter::Result prepareUnsupportedPaints(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    FPDF_PAGE inspectionPage,
    int pageIndex,
    RasterizedPaints &paints)
{
    assertPDFiumProcessLockHeld();
    const int objectCount = FPDFPage_CountObjects(page);
    if (objectCount != FPDFPage_CountObjects(inspectionPage)) {
        return failure(PDFSaveWriter::Error::SourceObjectMissing, pageIndex);
    }

    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT inspectionObject = FPDFPage_GetObject(
            inspectionPage,
            index);
        FPDF_PAGEOBJECT sourceObject = FPDFPage_GetObject(page, index);
        if (!inspectionObject
            || !sourceObject
            || FPDFPageObj_GetType(inspectionObject)
                != FPDFPageObj_GetType(sourceObject)) {
            return failure(PDFSaveWriter::Error::SourceObjectMissing, pageIndex);
        }

        const int objectType = FPDFPageObj_GetType(inspectionObject);
        if (objectType != FPDF_PAGEOBJ_PATH
            && objectType != FPDF_PAGEOBJ_SHADING) {
            continue;
        }
        if (objectType == FPDF_PAGEOBJ_PATH) {
            PDFShapeUtils::Metadata shapeMetadata;
            QByteArray shapeBytes;
            if (PDFShapeUtils::readMetadata(inspectionPage,
                    QSizeF(FPDF_GetPageWidthF(inspectionPage),
                           FPDF_GetPageHeightF(inspectionPage)),
                    inspectionObject, shapeMetadata, shapeBytes)) {
                // Canonical managed paths have explicit solid paint. Keep
                // their path and identity intact, including a white fill.
                continue;
            }
            int fillMode = FPDF_FILLMODE_NONE;
            FPDF_BOOL strokes = false;
            if (!FPDFPath_GetDrawMode(
                    inspectionObject,
                    &fillMode,
                    &strokes)
                || (fillMode == FPDF_FILLMODE_NONE && !strokes)) {
                continue;
            }
        }

        QImage renderedObject;
        FS_MATRIX imageMatrix{};
        if (!renderIsolatedPageObject(
                inspectionPage,
                index,
                renderedObject,
                imageMatrix)) {
            return failure(
                PDFSaveWriter::Error::ImageObjectCreationFailed,
                pageIndex);
        }
        const bool unsupportedPaint = objectType == FPDF_PAGEOBJ_SHADING
            || renderedPaintDiffersFromSerializableColors(
                inspectionObject,
                renderedObject);
        if (unsupportedPaint) {
            if (!createRasterizedPaint(
                document,
                page,
                sourceObject,
                index,
                renderedObject,
                imageMatrix,
                paints)) {
                return failure(
                    PDFSaveWriter::Error::ImageObjectCreationFailed,
                    pageIndex);
            }
            continue;
        }
        if (objectType == FPDF_PAGEOBJ_PATH
            && !materializeWhitePathFill(
                inspectionObject,
                sourceObject)) {
            return failure(
                PDFSaveWriter::Error::PageContentGenerationFailed,
                pageIndex);
        }
    }
    return {};
}

bool replaceUnsupportedPaints(FPDF_PAGE page, RasterizedPaints &paints)
{
    assertPDFiumProcessLockHeld();
    QVector<RasterizedPaint> &values = paints.values();
    std::sort(
        values.begin(),
        values.end(),
        [](const RasterizedPaint &left, const RasterizedPaint &right) {
            return left.topLevelIndex > right.topLevelIndex;
        });
    for (RasterizedPaint &paint : values) {
        if (!FPDFPage_RemoveObject(page, paint.sourceObject)) {
            return false;
        }
        FPDFPageObj_Destroy(paint.sourceObject);
        paint.sourceObject = nullptr;

        FPDF_PAGEOBJECT replacement = paint.replacementObject;
        const bool inserted = FPDFPage_InsertObjectAtIndex(
            page,
            replacement,
            static_cast<size_t>(paint.topLevelIndex));
        // InsertObjectAtIndex() takes ownership on both success and failure.
        paint.replacementObject = nullptr;
        if (!inserted) {
            return false;
        }
    }
    return true;
}

QVector<SourceObject> sourceObjectsForDraft(FPDF_PAGE page,
                                             const SaveDraft &draft,
                                             PDFSaveWriter::Result &result)
{
    QVector<SourceObject> objects;
    QSet<FPDF_PAGEOBJECT> seen;
    for (const QVector<int> &path : draft.sourceObjectPaths) {
        if (path.size() != 1) {
            result = failure(
                PDFSaveWriter::Error::UnsupportedSourceObject,
                draft.page,
                draft.objectId);
            return {};
        }
        const SourceObject source = sourceObjectAtPath(page, path);
        if (!source.object) {
            result = failure(
                PDFSaveWriter::Error::SourceObjectMissing,
                draft.page,
                draft.objectId);
            return {};
        }
        const int expectedType = draft.kind == SaveDraft::Kind::Image
            ? FPDF_PAGEOBJ_IMAGE
            : draft.kind == SaveDraft::Kind::Shape
                ? FPDF_PAGEOBJ_PATH : FPDF_PAGEOBJ_TEXT;
        const bool matchesType = draft.kind == SaveDraft::Kind::Image
            ? PDFSourceImageGeometry::isImageObject(source.object)
            : draft.kind == SaveDraft::Kind::Text
                ? PDFSourceTextGeometry::textObject(source.object) != nullptr
                : FPDFPageObj_GetType(source.object) == expectedType;
        if (!matchesType) {
            result = failure(
                PDFSaveWriter::Error::SourceObjectMissing,
                draft.page,
                draft.objectId);
            return {};
        }
        if (draft.kind == SaveDraft::Kind::Shape) {
            PDFShapeUtils::Metadata metadata;
            QByteArray bytes;
            if (draft.sourceShapeMetadata.isEmpty()
                || !PDFShapeUtils::readEditableShape(page,
                    QSizeF(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)),
                    source.object, metadata, bytes)
                || bytes != draft.sourceShapeMetadata) {
                result = failure(PDFSaveWriter::Error::SourceObjectMissing,
                                 draft.page, draft.objectId);
                return {};
            }
        }
        if (!seen.contains(source.object)) {
            seen.insert(source.object);
            objects.append(source);
        }
    }
    return objects;
}

QVector<SourceObject> sourceUnderlineObjectsForDraft(
    FPDF_PAGE page,
    const SaveDraft &draft,
    PDFSaveWriter::Result &result)
{
    QVector<SourceObject> objects;
    QSet<FPDF_PAGEOBJECT> seen;
    for (const QVector<int> &path : draft.sourceUnderlineObjectPaths) {
        if (draft.kind != SaveDraft::Kind::Text || path.size() != 1) {
            result = failure(
                PDFSaveWriter::Error::UnsupportedSourceObject,
                draft.page,
                draft.objectId);
            return {};
        }
        const SourceObject source = sourceObjectAtPath(page, path);
        if (!source.object
            || FPDFPageObj_GetType(source.object) != FPDF_PAGEOBJ_PATH) {
            result = failure(
                PDFSaveWriter::Error::SourceObjectMissing,
                draft.page,
                draft.objectId);
            return {};
        }
        if (!seen.contains(source.object)) {
            seen.insert(source.object);
            objects.append(source);
        }
    }
    return objects;
}

bool markTextSpacing(FPDF_DOCUMENT document,
                     FPDF_PAGEOBJECT textObject,
                     const LayoutRun &run,
                     qreal pageHeight,
                     qreal originOffset)
{
    if (run.spacingGroup.isEmpty()) {
        return true;
    }
    const QByteArray text = run.spacingText.toUtf8();
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(textObject, kTextSpacingMark);
    bool marked = mark
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Version", 2)
        && FPDFPageObjMark_SetStringParam(document, textObject, mark, "Group",
                                         run.spacingGroup.constData())
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Index",
                                      run.spacingIndex)
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Count",
                                      run.spacingCount)
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Start",
                                      run.spacingStart)
        && FPDFPageObjMark_SetBlobParam(
            document, textObject, mark, "Text",
            reinterpret_cast<const unsigned char *>(text.constData()),
            static_cast<unsigned long>(text.size()))
        && FPDFPageObjMark_SetFloatParam(
            document, textObject, mark, "LetterSpacing",
            static_cast<float>(run.format.letterSpacingRatio * pageHeight))
        && FPDFPageObjMark_SetFloatParam(
            document, textObject, mark, "WordSpacing",
            static_cast<float>(run.format.wordSpacingRatio * pageHeight))
        && FPDFPageObjMark_SetFloatParam(
            document, textObject, mark, "SourceAdvance",
            static_cast<float>(run.format.sourceAdvanceRatio * pageHeight));
    if (marked && !run.spacingRuns.isEmpty()) {
        marked = FPDFPageObjMark_SetBlobParam(
            document, textObject, mark, "SpacingRuns",
            reinterpret_cast<const unsigned char *>(run.spacingRuns.constData()),
            static_cast<unsigned long>(run.spacingRuns.size()));
    }
    if (marked && !qFuzzyIsNull(originOffset)) {
        marked = FPDFPageObjMark_SetFloatParam(
            document, textObject, mark, "OriginOffset",
            static_cast<float>(originOffset));
    }
    return marked;
}

bool markParagraphLayout(FPDF_DOCUMENT document,
                         FPDF_PAGEOBJECT textObject,
                         const LayoutRun &run,
                         qreal pageWidth,
                         qreal pageHeight,
                         qreal originOffset)
{
    if (run.paragraphGroup.isEmpty()) {
        return true;
    }
    const QByteArray text = run.paragraphText.toUtf8();
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(textObject, kParagraphLayoutMark);
    bool marked = mark
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Version", 1)
        && FPDFPageObjMark_SetStringParam(document, textObject, mark, "Group",
                                         run.paragraphGroup.constData())
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Index",
                                      run.paragraphIndex)
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Count",
                                      run.paragraphCount)
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Start",
                                      static_cast<int>(run.textStart))
        && FPDFPageObjMark_SetIntParam(document, textObject, mark, "Length",
                                      static_cast<int>(run.textLength))
        && FPDFPageObjMark_SetBlobParam(
            document, textObject, mark, "Text",
            reinterpret_cast<const unsigned char *>(text.constData()),
            static_cast<unsigned long>(text.size()));
    if (marked && run.paragraphIndex == 0) {
        // Relative geometry survives translating the original matrices during
        // a move-only save, without rewriting the stored model.
        const QPointF anchor((run.x + originOffset) / pageWidth,
                             run.baselineFromTop / pageHeight);
        const QPointF logicalOrigin = run.paragraphOrigin - anchor;
        QJsonObject model = run.paragraphModel;
        model.insert(QStringLiteral("originOffset"),
                     QJsonArray{logicalOrigin.x(), logicalOrigin.y()});
        model.insert(QStringLiteral("frameOffset"), QJsonArray{
            run.paragraphFrame.x() - anchor.x(),
            run.paragraphFrame.y() - anchor.y(),
            run.paragraphFrame.width(), run.paragraphFrame.height()});
        const QByteArray bytes = QJsonDocument(model).toJson(QJsonDocument::Compact);
        marked = bytes.size() <= kMaximumParagraphModelBytes
            && FPDFPageObjMark_SetBlobParam(
                document, textObject, mark, "Model",
                reinterpret_cast<const unsigned char *>(bytes.constData()),
                static_cast<unsigned long>(bytes.size()));
    }
    return marked;
}

bool markUnderlinePair(FPDF_DOCUMENT document,
                       FPDF_PAGEOBJECT textObject,
                       FPDF_PAGEOBJECT underlineObject)
{
    const QByteArray id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLatin1();
    for (FPDF_PAGEOBJECT object : {textObject, underlineObject}) {
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(object, kUnderlineMark);
        if (!mark
            || !FPDFPageObjMark_SetStringParam(
                document,
                object,
                mark,
                kUnderlineIdKey,
                id.constData())) {
            return false;
        }
    }
    return true;
}

FPDF_PAGEOBJECT createUnderline(const LayoutRun &run,
                                qreal fontSize,
                                const PageCoordinateMapper &coordinateMapper)
{
    const qreal underlineY = run.baselineFromTop
        + qMax<qreal>(0.8, fontSize * 0.10);
    QPointF start;
    QPointF end;
    if (!coordinateMapper.pagePoint(run.x, underlineY, start)
        || !coordinateMapper.pagePoint(run.x + run.width, underlineY, end)) {
        return nullptr;
    }
    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(
        static_cast<float>(start.x()),
        static_cast<float>(start.y()));
    if (!path) {
        return nullptr;
    }
    const QColor color = run.format.color.toRgb();
    if (!FPDFPath_LineTo(
            path,
            static_cast<float>(end.x()),
            static_cast<float>(end.y()))
        || !FPDFPageObj_SetStrokeColor(
            path,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()))
        || !FPDFPageObj_SetStrokeWidth(
            path,
            static_cast<float>(coordinateMapper.strokeWidth(
                qMax<qreal>(0.5, fontSize * 0.05))))
        || !FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, true)) {
        FPDFPageObj_Destroy(path);
        return nullptr;
    }
    return path;
}

PDFSaveWriter::Result createImageDraftObject(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper,
    QVector<FPDF_PAGEOBJECT> &objects,
    QVector<FPDF_BITMAP> &bitmaps)
{
    if (draft.image.isNull() || draft.frameRect.isEmpty()) {
        return failure(
            PDFSaveWriter::Error::ImageObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    const QImage image = draft.image.format() == QImage::Format_ARGB32
        ? draft.image
        : draft.image.convertToFormat(QImage::Format_ARGB32);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        image.width(),
        image.height(),
        FPDFBitmap_BGRA,
        nullptr,
        0);
    if (!bitmap) {
        return failure(
            PDFSaveWriter::Error::ImageObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    void *const bitmapBuffer = FPDFBitmap_GetBuffer(bitmap);
    const int bitmapStride = FPDFBitmap_GetStride(bitmap);
    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 4;
    if (!bitmapBuffer || bitmapStride < rowBytes) {
        FPDFBitmap_Destroy(bitmap);
        return failure(
            PDFSaveWriter::Error::ImageObjectCreationFailed,
            draft.page,
            draft.objectId);
    }
    for (int row = 0; row < image.height(); ++row) {
        std::memcpy(
            static_cast<uchar *>(bitmapBuffer)
                + static_cast<qsizetype>(row) * bitmapStride,
            image.constScanLine(row),
            static_cast<size_t>(rowBytes));
    }

    FPDF_PAGEOBJECT imageObject = FPDFPageObj_NewImageObj(document);
    FS_MATRIX matrix{};
    const QRectF frame(
        draft.frameRect.x() * pageWidth,
        draft.frameRect.y() * pageHeight,
        draft.frameRect.width() * pageWidth,
        draft.frameRect.height() * pageHeight);
    FPDF_PAGE loadedPages[] = {page};
    const bool configured = imageObject
        && FPDFImageObj_SetBitmap(loadedPages, 1, imageObject, bitmap)
        && coordinateMapper.imageMatrix(frame, matrix, draft.imageQuarterTurns)
        && FPDFPageObj_SetMatrix(imageObject, &matrix)
        && PDFHeaderMetadata::write(document, imageObject, draft.headerGroupId, draft.headerRole);
    if (!configured) {
        if (imageObject) {
            FPDFPageObj_Destroy(imageObject);
        }
        FPDFBitmap_Destroy(bitmap);
        return failure(
            PDFSaveWriter::Error::ImageObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    objects.append(imageObject);
    bitmaps.append(bitmap);
    return {};
}

PDFSaveWriter::Result createShapeDraftObject(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    QVector<FPDF_PAGEOBJECT> &objects)
{
    PDFShapeUtils::Metadata metadata;
    metadata.type = draft.shapeType;
    metadata.frame = draft.frameRect;
    metadata.start = draft.shapeStartRatio;
    metadata.end = draft.shapeEndRatio;
    metadata.strokeWidthRatio = draft.shapeStrokeWidthRatio;
    metadata.stroke = draft.commentProperties.strokeColor;
    metadata.fill = draft.commentProperties.interiorColor;
    metadata.opacity = draft.commentProperties.opacity;
    FPDF_PAGEOBJECT path = PDFShapeUtils::createPath(
        page, QSizeF(pageWidth, pageHeight), metadata);
    if (!path || !PDFShapeUtils::markPath(document, path, metadata)) {
        if (path) {
            FPDFPageObj_Destroy(path);
        }
        return failure(PDFSaveWriter::Error::ShapeObjectCreationFailed,
                       draft.page, draft.objectId);
    }
    objects.append(path);
    return {};
}

PDFSaveWriter::Result moveSourceObject(
    FPDF_PAGE page,
    const SaveDraft &draft,
    const SourceObject &source,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper)
{
    const PDFSaveWriter::Error moveError
        = draft.kind == SaveDraft::Kind::Image
        ? PDFSaveWriter::Error::ImageObjectCreationFailed
        : PDFSaveWriter::Error::TextObjectCreationFailed;
    if (!source.object
        || source.parentForm
        || draft.sourceRect.isEmpty()
        || draft.frameRect.isEmpty()) {
        return failure(
            moveError,
            draft.page,
            draft.objectId);
    }

    if (draft.kind == SaveDraft::Kind::Image) {
        FS_MATRIX originalMatrix{};
        FS_MATRIX transformedMatrix{};
        if (!FPDFPageObj_GetMatrix(source.object, &originalMatrix)
            || !PDFSourceImageGeometry::transformedMatrix(page, source.object,
                draft.sourceRect, draft.frameRect, originalMatrix, transformedMatrix,
                draft.imageQuarterTurns)
            || !FPDFPageObj_SetMatrix(source.object, &transformedMatrix)) {
            return failure(moveError, draft.page, draft.objectId);
        }
        return {};
    }

    const QPointF sourceTopLeft(
        draft.sourceRect.x() * pageWidth,
        draft.sourceRect.y() * pageHeight);
    const QPointF targetTopLeft(
        draft.frameRect.x() * pageWidth,
        draft.frameRect.y() * pageHeight);
    QPointF sourcePagePoint;
    QPointF targetPagePoint;
    FS_MATRIX matrix{};
    if (!coordinateMapper.pagePoint(
            sourceTopLeft.x(),
            sourceTopLeft.y(),
            sourcePagePoint)
        || !coordinateMapper.pagePoint(
            targetTopLeft.x(),
            targetTopLeft.y(),
            targetPagePoint)
        || !FPDFPageObj_GetMatrix(source.object, &matrix)) {
        return failure(
            moveError,
            draft.page,
            draft.objectId);
    }

    matrix.e += static_cast<float>(targetPagePoint.x() - sourcePagePoint.x());
    matrix.f += static_cast<float>(targetPagePoint.y() - sourcePagePoint.y());
    if (!FPDFPageObj_SetMatrix(source.object, &matrix)) {
        return failure(
            moveError,
            draft.page,
            draft.objectId);
    }
    return {};
}

PDFSaveWriter::Result createDraftObjects(FPDF_DOCUMENT document,
                                         FPDF_PAGE page,
                                         const SaveDraft &draft,
                                         qreal pageWidth,
                                         qreal pageHeight,
                                         FPDF_FONT originalFont,
                                         const QVector<FPDF_FONT> &sourceFonts,
                                         FontResolver &fontResolver,
                                         const PageCoordinateMapper &coordinateMapper,
                                         QVector<FPDF_PAGEOBJECT> &objects,
                                         QVector<FPDF_BITMAP> &bitmaps)
{
    if (draft.deleted) {
        return {};
    }
    if (draft.movesSourceObjects) {
        return {};
    }
    if (draft.kind == SaveDraft::Kind::Image) {
        if (!draft.added) {
            return {};
        }
        return createImageDraftObject(
            document,
            page,
            draft,
            pageWidth,
            pageHeight,
            coordinateMapper,
            objects,
            bitmaps);
    }
    if (draft.kind == SaveDraft::Kind::Shape) {
        return createShapeDraftObject(
            document,
            page,
            draft,
            pageWidth,
            pageHeight,
            objects);
    }

    SaveDraft preparedDraft = draft;
    normalizeDraftText(preparedDraft);
    const auto prepareFormat = [&](SaveFormat &format,
                                   const QString &text,
                                   uint &unsupportedCodePoint) {
        unsupportedCodePoint = 0;
        format.originalType1Font = nullptr;
        if (std::all_of(text.cbegin(), text.cend(), [](QChar character) {
                return character == QLatin1Char('\n') || character == QLatin1Char('\t');
            })) {
            return true;
        }

        const bool originalFontCanRepresentText = originalFont
            && !draft.added
            && canReuseOriginalFont(format, draft.originalFormat)
            && originalTextContainsCharacters(draft.originalText, text);
        format.allowOriginalFontReuse = originalFontCanRepresentText;
        if (originalFontCanRepresentText) {
            return true;
        }

        uint missingOriginalCodePoint = 0;
        bool knownOriginalType1 = false;
        if (!draft.added && fontResolver.prepareOriginalType1Font(
                format, text, sourceFonts, knownOriginalType1, missingOriginalCodePoint))
            return true;

        // Reuse the actual embedded face before replacing it with a complete
        // local face. The bounded source loader validates editable embedding
        // and every requested glyph, and supplies its own Unicode/CID mapping
        // when PDFium's original subset encoding cannot represent the edit.
        uint missingSourceCodePoint = 0;
        if (!draft.added && fontResolver.prepareSourceFace(
                format, text, sourceFonts, missingSourceCodePoint))
            return true;

        const QString family = fontResolver.embeddedFamilyForText(
            format,
            text,
            unsupportedCodePoint);
        if (!family.isEmpty()) {
            format.fontFamily = family;
            format.forceEmbeddedFont = true;
            return true;
        }
        if (!unsupportedCodePoint)
            unsupportedCodePoint = missingSourceCodePoint
                ? missingSourceCodePoint : missingOriginalCodePoint;
        if (unsupportedCodePoint || knownOriginalType1)
            return false;
        return !requiresEmbeddedUnicodeFont(text)
            && format.embeddableFontFamily.isEmpty();
    };

    uint unsupportedCodePoint = 0;
    if (preparedDraft.characterSpans.isEmpty()) {
        if (!prepareFormat(
                preparedDraft.originalFormat,
                preparedDraft.text,
                unsupportedCodePoint)) {
            return fontFailure(preparedDraft.originalFormat, draft.page, unsupportedCodePoint);
        }
    }
    for (SaveSpan &span : preparedDraft.characterSpans) {
        const qsizetype start = qBound<qsizetype>(
            0,
            span.start,
            preparedDraft.text.size());
        const qsizetype length = qBound<qsizetype>(
            0,
            span.length,
            preparedDraft.text.size() - start);
        if (!prepareFormat(
                span.format,
                preparedDraft.text.sliced(start, length),
                unsupportedCodePoint)) {
            return fontFailure(span.format, draft.page, unsupportedCodePoint);
        }
    }

    bool validLayout = false;
    QVector<LayoutRun> runs = layoutRuns(
        preparedDraft,
        pageWidth,
        pageHeight,
        validLayout);
    if (!validLayout) {
        return failure(PDFSaveWriter::Error::TextObjectCreationFailed,
                       draft.page, draft.objectId);
    }
    struct EmbeddedEncoding {
        QVector<char32_t> characters;
        QByteArray digest;
    };
    QHash<QByteArray, EmbeddedEncoding> embeddedEncodings;
    for (LayoutRun &run : runs) {
        // Layout can attach an inferred separator with its own complete face
        // to a preceding source-font run. Recheck the final encoded text before
        // reusing that source font: preparing only the original visible span
        // would silently map an absent subset space to .notdef.
        const bool originalFontContainsRun = run.format.originalType1Font
            ? fontResolver.originalType1FontContainsText(run.format.originalType1Font, run.text)
            : originalTextContainsCharacters(draft.originalText, run.text);
        if (run.format.allowOriginalFontReuse && !originalFontContainsRun
            && !prepareFormat(run.format, run.text, unsupportedCodePoint)) {
            return fontFailure(run.format, draft.page, unsupportedCodePoint);
        }
        if (!fontResolver.prepareEmbeddedText(run.format, run.text, unsupportedCodePoint)) {
            return fontFailure(run.format, draft.page, unsupportedCodePoint);
        }
        if (!run.format.embeddedFontCharacters.isEmpty())
            embeddedEncodings[fontResolver.embeddedEncodingKey(run.format)].characters
                += run.format.embeddedFontCharacters;
    }
    for (auto entry = embeddedEncodings.begin(); entry != embeddedEncodings.end(); ++entry) {
        auto &characters = entry->characters;
        characters += fontResolver.completeFontCharacters(entry.key());
        std::sort(characters.begin(), characters.end());
        characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
        if (characters.size() > 65535)
            return failure(PDFSaveWriter::Error::FontUnavailable,
                           draft.page, draft.originalFormat.semanticFontFamily);
        entry->digest = QCryptographicHash::hash(
            QString::fromUcs4(characters.constData(), characters.size()).toUtf8(),
            QCryptographicHash::Sha256);
    }
    for (LayoutRun &run : runs) {
        if (!run.format.embeddedFontCharacters.isEmpty()) {
            const auto encoding = embeddedEncodings.constFind(
                fontResolver.embeddedEncodingKey(run.format));
            run.format.embeddedFontCharacters = encoding->characters;
            run.format.embeddedCharactersDigest = encoding->digest;
        }
        const FontResolver::ResolvedFont resolvedFont = fontResolver.resolve(
            run.format,
            draft.originalFormat,
            originalFont);
        FPDF_FONT font = resolvedFont.handle;
        if (!font) {
            return fontFailure(run.format, draft.page);
        }

        const qreal sizeRatio = run.format.displayFontSizeRatio > 0.0
            ? run.format.displayFontSizeRatio
            : run.format.fontSizeRatio;
        const qreal fontSize = qMax<qreal>(1.0, qRound(sizeRatio * pageHeight));
        FPDF_PAGEOBJECT textObject = FPDFPageObj_CreateTextObj(
            document,
            font,
            static_cast<float>(fontSize));
        if (!textObject) {
            return failure(
                PDFSaveWriter::Error::TextObjectCreationFailed,
                draft.page,
                draft.objectId);
        }

        const QColor color = run.format.color.toRgb();
        const FPDF_TEXT_RENDERMODE renderMode = draft.added
            ? FPDF_TEXTRENDERMODE_FILL
            : draft.originalRenderMode;
        if (renderMode != FPDF_TEXTRENDERMODE_FILL
            && renderMode != FPDF_TEXTRENDERMODE_STROKE
            && renderMode != FPDF_TEXTRENDERMODE_FILL_STROKE) {
            FPDFPageObj_Destroy(textObject);
            return failure(
                PDFSaveWriter::Error::UnsupportedSourceObject,
                draft.page,
                draft.objectId);
        }

        const bool colorChanged = run.format.color != draft.originalFormat.color;
        const QColor fillColor = colorChanged
            ? color
            : draft.originalFillColor.toRgb();
        const QColor strokeColor = colorChanged
            ? color
            : draft.originalStrokeColor.toRgb();
        const bool usesFill = renderMode == FPDF_TEXTRENDERMODE_FILL
            || renderMode == FPDF_TEXTRENDERMODE_FILL_STROKE;
        const bool usesStroke = renderMode == FPDF_TEXTRENDERMODE_STROKE
            || renderMode == FPDF_TEXTRENDERMODE_FILL_STROKE;
        bool configured = FontResolver::setText(textObject, run.format, run.text)
            && FPDFTextObj_SetTextRenderMode(textObject, renderMode);
        if (configured && usesFill) {
            configured = FPDFPageObj_SetFillColor(
                textObject,
                static_cast<unsigned int>(fillColor.red()),
                static_cast<unsigned int>(fillColor.green()),
                static_cast<unsigned int>(fillColor.blue()),
                static_cast<unsigned int>(fillColor.alpha()));
        }
        if (configured && usesStroke) {
            configured = FPDFPageObj_SetStrokeColor(
                textObject,
                static_cast<unsigned int>(strokeColor.red()),
                static_cast<unsigned int>(strokeColor.green()),
                static_cast<unsigned int>(strokeColor.blue()),
                static_cast<unsigned int>(strokeColor.alpha()))
                && FPDFPageObj_SetStrokeWidth(
                    textObject,
                    static_cast<float>(draft.originalStrokeWidth));
        }

        float left = 0.0F;
        float bottom = 0.0F;
        float right = 0.0F;
        float top = 0.0F;
        qreal horizontalScale = 1.0;
        const bool preservesOriginalHorizontalScale =
            resolvedFont.source == FontResolver::ResolvedFont::Source::Original
            && !run.format.originalType1Font
            && run.format.fontStretch == draft.originalFormat.fontStretch;
        if (configured && preservesOriginalHorizontalScale) {
            horizontalScale = qBound(
                kMinimumScale,
                qIsFinite(draft.originalHorizontalScale)
                    ? draft.originalHorizontalScale
                    : 1.0,
                kMaximumScale);
        } else if (configured && run.positionedWord) {
            // Spacing is already represented by each run's Qt origin. A
            // glyph bounding box omits side bearings, so fitting it to the
            // word advance would stretch the glyphs themselves.
            horizontalScale = qBound(
                kMinimumScale,
                static_cast<qreal>(run.format.fontStretch) / QFont::Unstretched,
                kMaximumScale);
        } else if (configured
            && FPDFPageObj_GetBounds(textObject, &left, &bottom, &right, &top)
            && right - left > kMinimumObjectWidth) {
            horizontalScale = qBound(
                kMinimumScale,
                run.width / static_cast<qreal>(right - left),
                kMaximumScale);
        }
        qreal originOffset = run.leadingLayoutAdvance;
        if (configured && run.hasCharacterPositions && !run.characterPositions.isEmpty()) {
            QVector<float> positions;
            positions.reserve(run.characterPositions.size());
            for (qreal position : run.characterPositions) {
                const qreal localPosition = position / horizontalScale;
                if (!qIsFinite(localPosition)
                    || qAbs(localPosition) > std::numeric_limits<float>::max()) {
                    configured = false;
                    break;
                }
                positions.append(static_cast<float>(localPosition));
            }
            configured = configured && FPDFText_SetPositions(
                textObject, positions.constData(), size_t(positions.size()));
        }
        if (configured && !run.hasCharacterPositions && (!run.leadingWhitespace.isEmpty()
                           || !qFuzzyIsNull(originOffset))) {
            for (uint character : run.leadingWhitespace.toUcs4()) {
                float advance = 0.0F;
                configured = FPDFFont_GetGlyphWidth(font, character,
                    static_cast<float>(fontSize), &advance)
                    && qIsFinite(advance) && advance >= 0.0F;
                if (!configured) {
                    break;
                }
                originOffset -= advance * horizontalScale;
            }
        }
        FS_MATRIX matrix{};
        configured = configured
            && coordinateMapper.textMatrix(
                run.x + originOffset,
                run.baselineFromTop,
                horizontalScale,
                matrix)
            && FPDFPageObj_SetMatrix(textObject, &matrix)
            && markTextSpacing(document, textObject, run, pageHeight, originOffset)
            && markParagraphLayout(document, textObject, run,
                                   pageWidth, pageHeight, originOffset)
            && PDFHeaderMetadata::write(document, textObject, draft.headerGroupId, draft.headerRole);
        if (!configured) {
            FPDFPageObj_Destroy(textObject);
            return failure(
                PDFSaveWriter::Error::TextObjectCreationFailed,
                draft.page,
                draft.objectId);
        }
        objects.append(textObject);

        if (run.format.fontUnderline) {
            FPDF_PAGEOBJECT underline = createUnderline(
                run,
                fontSize,
                coordinateMapper);
            if (!underline || !markUnderlinePair(document, textObject, underline)) {
                if (underline) {
                    FPDFPageObj_Destroy(underline);
                }
                return failure(
                    PDFSaveWriter::Error::TextObjectCreationFailed,
                    draft.page,
                    draft.objectId);
            }
            objects.append(underline);
        }
    }
    Q_UNUSED(page);
    return {};
}

void destroyObjects(QVector<FPDF_PAGEOBJECT> &objects)
{
    for (FPDF_PAGEOBJECT object : std::as_const(objects)) {
        if (object) {
            FPDFPageObj_Destroy(object);
        }
    }
    objects.clear();
}

void destroyBitmaps(QVector<FPDF_BITMAP> &bitmaps)
{
    for (FPDF_BITMAP bitmap : std::as_const(bitmaps)) {
        if (bitmap) {
            FPDFBitmap_Destroy(bitmap);
        }
    }
    bitmaps.clear();
}

void destroyPreparedObjects(QVector<PreparedDraft> &preparedDrafts)
{
    for (PreparedDraft &prepared : preparedDrafts) {
        destroyObjects(prepared.objects);
        destroyBitmaps(prepared.bitmaps);
    }
}

QString pdfAnnotationDateString(const QDateTime &dateTime)
{
    if (!dateTime.isValid()) {
        return {};
    }

    const int offsetSeconds = dateTime.offsetFromUtc();
    QString timeZoneSuffix;
    if (offsetSeconds == 0) {
        timeZoneSuffix = QStringLiteral("Z");
    } else {
        const int absoluteMinutes = qAbs(offsetSeconds) / 60;
        const int offsetHours = absoluteMinutes / 60;
        const int offsetMinutes = absoluteMinutes % 60;
        timeZoneSuffix = QStringLiteral("%1%2'%3'")
            .arg(offsetSeconds < 0 ? QLatin1Char('-') : QLatin1Char('+'))
            .arg(offsetHours, 2, 10, QLatin1Char('0'))
            .arg(offsetMinutes, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("D:%1%2")
        .arg(
            dateTime.toString(QStringLiteral("yyyyMMddHHmmss")),
            timeZoneSuffix);
}

bool setOptionalAnnotationString(FPDF_ANNOTATION annotation,
                                 FPDF_BYTESTRING key,
                                 const QString &value)
{
    return value.isEmpty()
        || FPDFAnnot_SetStringValue(
            annotation,
            key,
            reinterpret_cast<const FPDF_WCHAR *>(value.utf16()));
}

QString pdfCommentContents(const QString &text)
{
    QString contents = normalizedCommentText(text);
    contents.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    return contents;
}

bool setCommentAnnotationMetadata(FPDF_ANNOTATION annotation,
                                  const SaveDraft &draft)
{
    return setOptionalAnnotationString(
               annotation,
               "NM",
               draft.objectId)
        && setOptionalAnnotationString(
               annotation,
               "T",
               draft.commentAuthor)
        && setOptionalAnnotationString(
            annotation,
            "CreationDate",
            pdfAnnotationDateString(draft.commentCreationDate))
        && setOptionalAnnotationString(
            annotation,
            "M",
            pdfAnnotationDateString(draft.commentModifiedDate))
        && setOptionalAnnotationString(
            annotation,
            kCommentThreadIdKey,
            draft.commentThreadId)
        && setOptionalAnnotationString(
            annotation,
            kCommentReplyToKey,
            draft.replyToThreadId);
}

QString commentStatusPdfName(PDFEditController::CommentStatus status)
{
    switch (status) {
    case PDFEditController::NoCommentStatus:
        return QStringLiteral("None");
    case PDFEditController::AcceptedCommentStatus:
        return QStringLiteral("Accepted");
    case PDFEditController::RejectedCommentStatus:
        return QStringLiteral("Rejected");
    case PDFEditController::CancelledCommentStatus:
        return QStringLiteral("Cancelled");
    case PDFEditController::CompletedCommentStatus:
        return QStringLiteral("Completed");
    case PDFEditController::DeferredCommentStatus:
        return QStringLiteral("Deferred");
    case PDFEditController::FutureCommentStatus:
        return QStringLiteral("Future");
    }
    return {};
}

QString uniqueSourceStateAnnotationName(FPDF_PAGE page,
                                        int annotationIndex,
                                        const QString &stateModel,
                                        const QString &author)
{
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    if (annotationIndex < 0 || annotationIndex >= annotationCount) {
        return {};
    }

    PdfAnnotationHandle stateAnnotation(FPDFPage_GetAnnot(
        page,
        annotationIndex));
    const QString annotationName = stateAnnotation.get()
        ? annotationString(stateAnnotation.get(), "NM")
        : QString();
    if (!stateAnnotation.get()
        || FPDFAnnot_GetSubtype(stateAnnotation.get()) != FPDF_ANNOT_TEXT
        || annotationName.isEmpty()
        || annotationString(stateAnnotation.get(), "State").trimmed().isEmpty()
        || annotationString(stateAnnotation.get(), "StateModel").trimmed()
               .compare(stateModel, Qt::CaseInsensitive)
            != 0
        || annotationString(stateAnnotation.get(), "T").trimmed().compare(
               author.trimmed(),
               Qt::CaseSensitive)
            != 0) {
        return {};
    }

    int matchingNames = 0;
    for (int index = 0; index < annotationCount; ++index) {
        PdfAnnotationHandle annotation(FPDFPage_GetAnnot(page, index));
        if (annotation.get()
            && annotationString(annotation.get(), "NM") == annotationName) {
            ++matchingNames;
        }
    }
    return matchingNames == 1 ? annotationName : QString();
}

PDFSaveWriter::Result createCommentStateAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    const QString &state,
    const QString &stateModel,
    const QString &parentAnnotationId,
    QVector<PDFAcroFormCanonicalizer::AnnotationRelationship>
        &annotationRelationships)
{
    if (!page
        || draft.page < 0
        || draft.objectId.isEmpty()
        || draft.commentThreadId.isEmpty()
        || draft.commentStateAuthor.trimmed().isEmpty()
        || !draft.commentStateModifiedDate.isValid()
        || state.isEmpty()
        || stateModel.isEmpty()) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    const QString annotationId = QStringLiteral("comment-state:")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString displayedState
        = stateModel == QLatin1String("Marked")
            && state == QLatin1String("Unmarked")
        ? QStringLiteral("None")
        : state;
    const QString contents = displayedState
        + QStringLiteral(" set by ")
        + draft.commentStateAuthor;
    const QString modificationDate = pdfAnnotationDateString(
        draft.commentStateModifiedDate);
    constexpr int stateFlags = FPDF_ANNOT_FLAG_HIDDEN
        | FPDF_ANNOT_FLAG_PRINT
        | FPDF_ANNOT_FLAG_NOZOOM
        | FPDF_ANNOT_FLAG_NOROTATE;
    FS_RECTF stateRect{};
    PdfAnnotationHandle annotation(
        FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT));
    if (!annotation.get()
        || !FPDFAnnot_SetRect(annotation.get(), &stateRect)
        || !FPDFAnnot_SetFlags(annotation.get(), stateFlags)
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "NM",
            reinterpret_cast<const FPDF_WCHAR *>(annotationId.utf16()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "T",
            reinterpret_cast<const FPDF_WCHAR *>(
                draft.commentStateAuthor.utf16()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            reinterpret_cast<const FPDF_WCHAR *>(contents.utf16()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "CreationDate",
            reinterpret_cast<const FPDF_WCHAR *>(
                modificationDate.utf16()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "M",
            reinterpret_cast<const FPDF_WCHAR *>(
                modificationDate.utf16()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "State",
            reinterpret_cast<const FPDF_WCHAR *>(state.utf16()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "StateModel",
            reinterpret_cast<const FPDF_WCHAR *>(stateModel.utf16()))) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    PDFAcroFormCanonicalizer::AnnotationRelationship relationship;
    relationship.page = draft.page;
    if (parentAnnotationId.isEmpty()) {
        relationship.parentThreadId = draft.commentThreadId;
    } else {
        relationship.parentAnnotationId = parentAnnotationId;
    }
    relationship.childAnnotationId = annotationId;
    relationship.kind = PDFAcroFormCanonicalizer::
        AnnotationRelationshipKind::State;
    relationship.expectedState = state;
    relationship.expectedStateModel = stateModel;
    annotationRelationships.append(std::move(relationship));
    return {};
}

QString freeTextAppearanceId(const SaveDraft &draft)
{
    if (draft.page < 0 || draft.objectId.isEmpty()) {
        return {};
    }
    return QStringLiteral("%1:%2")
        .arg(draft.page)
        .arg(draft.objectId);
}

QString stickyNoteAppearance(const FS_RECTF &rect, const QColor &color)
{
    const qreal width = rect.right - rect.left;
    const qreal height = rect.top - rect.bottom;
    const qreal minimumDimension = qMin(width, height);
    if (!(width > 0.0)
        || !(height > 0.0)
        || !std::isfinite(minimumDimension)) {
        return {};
    }

    const qreal lineWidth = qBound<qreal>(0.5, minimumDimension * 0.045, 1.25);
    const qreal inset = lineWidth * 0.75;
    const qreal fold = qBound<qreal>(3.0, minimumDimension * 0.28, 8.0);
    const qreal left = rect.left + inset;
    const qreal right = rect.right - inset;
    const qreal bottom = rect.bottom + inset;
    const qreal top = rect.top - inset;
    const qreal foldLeft = qMax(left, right - fold);
    const qreal foldBottom = qMax(bottom, top - fold);
    const qreal textLeft = left + minimumDimension * 0.16;
    const qreal textRight = qMax(textLeft, foldLeft - minimumDimension * 0.08);
    const qreal firstLine = top - minimumDimension * 0.42;
    const qreal secondLine = top - minimumDimension * 0.62;
    const QColor fill = color.toRgb();
    const QColor foldColor = fill.lighter(112);
    const auto number = [](qreal value) {
        return QString::number(value, 'f', 4);
    };
    const auto colorComponent = [&number](int value) {
        return number(static_cast<qreal>(value) / 255.0);
    };

    QString appearance = QStringLiteral("q\n");
    if (fill.alpha() < 255) {
        appearance += QStringLiteral("/GS gs\n");
    }
    appearance += colorComponent(fill.red()) + QLatin1Char(' ')
        + colorComponent(fill.green()) + QLatin1Char(' ')
        + colorComponent(fill.blue()) + QStringLiteral(" rg\n");
    appearance += QStringLiteral("0.18 0.16 0.08 RG\n")
        + number(lineWidth) + QStringLiteral(" w\n")
        + number(left) + QLatin1Char(' ') + number(bottom)
        + QStringLiteral(" m\n")
        + number(right) + QLatin1Char(' ') + number(bottom)
        + QStringLiteral(" l\n")
        + number(right) + QLatin1Char(' ') + number(foldBottom)
        + QStringLiteral(" l\n")
        + number(foldLeft) + QLatin1Char(' ') + number(top)
        + QStringLiteral(" l\n")
        + number(left) + QLatin1Char(' ') + number(top)
        + QStringLiteral(" l\nh\nB\n");
    appearance += colorComponent(foldColor.red()) + QLatin1Char(' ')
        + colorComponent(foldColor.green()) + QLatin1Char(' ')
        + colorComponent(foldColor.blue()) + QStringLiteral(" rg\n")
        + number(right) + QLatin1Char(' ') + number(foldBottom)
        + QStringLiteral(" m\n")
        + number(foldLeft) + QLatin1Char(' ') + number(top)
        + QStringLiteral(" l\n")
        + number(foldLeft) + QLatin1Char(' ') + number(foldBottom)
        + QStringLiteral(" l\nh\nB\n")
        + QStringLiteral("0.28 0.25 0.10 RG\n")
        + number(lineWidth * 0.8) + QStringLiteral(" w\n")
        + number(textLeft) + QLatin1Char(' ') + number(firstLine)
        + QStringLiteral(" m\n")
        + number(textRight) + QLatin1Char(' ') + number(firstLine)
        + QStringLiteral(" l\nS\n")
        + number(textLeft) + QLatin1Char(' ') + number(secondLine)
        + QStringLiteral(" m\n")
        + number(textRight) + QLatin1Char(' ') + number(secondLine)
        + QStringLiteral(" l\nS\nQ\n");
    return appearance;
}

PDFSaveWriter::Result createTextMarkupAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper)
{
    QVector<FS_QUADPOINTSF> quadPoints;
    quadPoints.reserve(draft.highlightRects.size());
    bool hasBounds = false;
    qreal minimumX = 0.0;
    qreal maximumX = 0.0;
    qreal minimumY = 0.0;
    qreal maximumY = 0.0;
    const auto includePoint = [
        &hasBounds,
        &minimumX,
        &maximumX,
        &minimumY,
        &maximumY](const QPointF &point) {
        if (!hasBounds) {
            minimumX = maximumX = point.x();
            minimumY = maximumY = point.y();
            hasBounds = true;
            return;
        }
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    };
    const auto finitePoint = [](const QPointF &point) {
        return std::isfinite(point.x()) && std::isfinite(point.y());
    };

    for (const QRectF &highlightRect : draft.highlightRects) {
        if (!std::isfinite(highlightRect.x())
            || !std::isfinite(highlightRect.y())
            || !std::isfinite(highlightRect.width())
            || !std::isfinite(highlightRect.height())) {
            continue;
        }
        const QRectF normalizedRect = highlightRect.normalized().intersected(
            QRectF(0.0, 0.0, 1.0, 1.0));
        if (normalizedRect.isEmpty()) {
            continue;
        }

        const QRectF deviceRect(
            normalizedRect.x() * pageWidth,
            normalizedRect.y() * pageHeight,
            normalizedRect.width() * pageWidth,
            normalizedRect.height() * pageHeight);
        QPointF topLeft;
        QPointF topRight;
        QPointF bottomLeft;
        QPointF bottomRight;
        if (!coordinateMapper.pagePoint(
                deviceRect.left(),
                deviceRect.top(),
                topLeft)
            || !coordinateMapper.pagePoint(
                deviceRect.right(),
                deviceRect.top(),
                topRight)
            || !coordinateMapper.pagePoint(
                deviceRect.left(),
                deviceRect.bottom(),
                bottomLeft)
            || !coordinateMapper.pagePoint(
                deviceRect.right(),
                deviceRect.bottom(),
                bottomRight)
            || !finitePoint(topLeft)
            || !finitePoint(topRight)
            || !finitePoint(bottomLeft)
            || !finitePoint(bottomRight)) {
            continue;
        }

        quadPoints.append(FS_QUADPOINTSF{
            static_cast<float>(topLeft.x()),
            static_cast<float>(topLeft.y()),
            static_cast<float>(topRight.x()),
            static_cast<float>(topRight.y()),
            static_cast<float>(bottomLeft.x()),
            static_cast<float>(bottomLeft.y()),
            static_cast<float>(bottomRight.x()),
            static_cast<float>(bottomRight.y())});
        includePoint(topLeft);
        includePoint(topRight);
        includePoint(bottomLeft);
        includePoint(bottomRight);
    }

    if (quadPoints.isEmpty() || !hasBounds) {
        return failure(
            PDFSaveWriter::Error::TextMarkupAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    PdfAnnotationHandle annotation(
        FPDFPage_CreateAnnot(page, draft.annotationSubtype));
    const QColor color = draft.highlightColor.isValid()
        ? draft.highlightColor.toRgb()
        : QColor(255, 235, 59, 96);
    if (!annotation.get()
        || !FPDFAnnot_SetColor(
            annotation.get(),
            FPDFANNOT_COLORTYPE_Color,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()))
        || !FPDFAnnot_SetFlags(
            annotation.get(),
            FPDF_ANNOT_FLAG_PRINT)
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            reinterpret_cast<const FPDF_WCHAR *>(
                pdfCommentContents(draft.text).utf16()))
        || !setCommentAnnotationMetadata(annotation.get(), draft)) {
        return failure(
            PDFSaveWriter::Error::TextMarkupAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }
    for (const FS_QUADPOINTSF &quadPoint : std::as_const(quadPoints)) {
        if (!FPDFAnnot_AppendAttachmentPoints(
                annotation.get(),
                &quadPoint)) {
            return failure(
                PDFSaveWriter::Error::TextMarkupAnnotationCreationFailed,
                draft.page,
                draft.objectId);
        }
    }

    FS_RECTF annotationRect{};
    annotationRect.left = static_cast<float>(minimumX);
    annotationRect.top = static_cast<float>(maximumY);
    annotationRect.right = static_cast<float>(maximumX);
    annotationRect.bottom = static_cast<float>(minimumY);
    if (!FPDFAnnot_SetRect(annotation.get(), &annotationRect)) {
        return failure(
            PDFSaveWriter::Error::TextMarkupAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }
    return {};
}

bool annotationRectForFrame(const QRectF &frameRect,
                            qreal pageWidth,
                            qreal pageHeight,
                            const PageCoordinateMapper &coordinateMapper,
                            FS_RECTF &annotationRect)
{
    if (!std::isfinite(frameRect.x())
        || !std::isfinite(frameRect.y())
        || !std::isfinite(frameRect.width())
        || !std::isfinite(frameRect.height())) {
        return false;
    }
    const QRectF normalizedRect = frameRect.normalized().intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    if (normalizedRect.isEmpty()) {
        return false;
    }

    const QRectF deviceRect(
        normalizedRect.x() * pageWidth,
        normalizedRect.y() * pageHeight,
        normalizedRect.width() * pageWidth,
        normalizedRect.height() * pageHeight);
    const std::array<QPointF, 4> devicePoints{
        deviceRect.topLeft(),
        deviceRect.topRight(),
        deviceRect.bottomLeft(),
        deviceRect.bottomRight()};
    std::array<QPointF, 4> pagePoints;
    for (size_t index = 0; index < devicePoints.size(); ++index) {
        if (!coordinateMapper.pagePoint(
                devicePoints[index].x(),
                devicePoints[index].y(),
                pagePoints[index])
            || !std::isfinite(pagePoints[index].x())
            || !std::isfinite(pagePoints[index].y())) {
            return false;
        }
    }

    qreal minimumX = pagePoints.front().x();
    qreal maximumX = minimumX;
    qreal minimumY = pagePoints.front().y();
    qreal maximumY = minimumY;
    for (const QPointF &point : pagePoints) {
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    }
    annotationRect = FS_RECTF{
        static_cast<float>(minimumX),
        static_cast<float>(maximumY),
        static_cast<float>(maximumX),
        static_cast<float>(minimumY)};
    return annotationRect.right > annotationRect.left
        && annotationRect.top > annotationRect.bottom;
}

PDFSaveWriter::Result prepareDrawingCommentAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper,
    bool createPlaceholder,
    PDFCommentAnnotation::DrawingAnnotation &appearanceRequest)
{
    constexpr qsizetype maximumVertices = 4096;
    constexpr qsizetype maximumInkStrokes = 64;
    constexpr qsizetype maximumInkPoints = 32768;
    const bool ink
        = draft.drawingType == PDFCommentAnnotation::DrawingType::Ink;
    if (draft.objectId.isEmpty()) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    const auto mapNormalizedPoint = [
        pageWidth,
        pageHeight,
        &coordinateMapper](const QPointF &point,
                           QPointF &pagePoint) {
        return std::isfinite(point.x())
            && std::isfinite(point.y())
            && point.x() >= 0.0
            && point.x() <= 1.0
            && point.y() >= 0.0
            && point.y() <= 1.0
            && coordinateMapper.pagePoint(
                point.x() * pageWidth,
                point.y() * pageHeight,
                pagePoint)
            && std::isfinite(pagePoint.x())
            && std::isfinite(pagePoint.y());
    };

    QVector<QPointF> pageVertices;
    QVector<QVector<QPointF>> pageStrokes;
    if (ink) {
        if (!draft.drawingVertices.isEmpty()
            || draft.inkStrokes.isEmpty()
            || draft.inkStrokes.size() > maximumInkStrokes) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                draft.page,
                draft.objectId);
        }
        qsizetype totalInkPoints = 0;
        pageStrokes.reserve(draft.inkStrokes.size());
        for (const QVector<QPointF> &stroke : draft.inkStrokes) {
            if (stroke.size() < 2 || stroke.size() > maximumVertices
                || stroke.size() > maximumInkPoints - totalInkPoints) {
                return failure(
                    PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                    draft.page,
                    draft.objectId);
            }
            QVector<QPointF> mappedStroke;
            mappedStroke.reserve(stroke.size());
            for (const QPointF &point : stroke) {
                QPointF pagePoint;
                if (!mapNormalizedPoint(point, pagePoint)
                    || (!mappedStroke.isEmpty()
                        && std::hypot(
                            pagePoint.x() - mappedStroke.constLast().x(),
                            pagePoint.y() - mappedStroke.constLast().y())
                            <= 0.0)) {
                    return failure(
                        PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                        draft.page,
                        draft.objectId);
                }
                mappedStroke.append(pagePoint);
            }
            totalInkPoints += mappedStroke.size();
            pageStrokes.append(std::move(mappedStroke));
        }
    } else {
        if (draft.drawingVertices.size() < 2
            || draft.drawingVertices.size() > maximumVertices
            || !draft.inkStrokes.isEmpty()) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                draft.page,
                draft.objectId);
        }
        pageVertices.reserve(draft.drawingVertices.size());
        for (const QPointF &vertex : draft.drawingVertices) {
            QPointF pageVertex;
            if (!mapNormalizedPoint(vertex, pageVertex)) {
                return failure(
                    PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                    draft.page,
                    draft.objectId);
            }
            pageVertices.append(pageVertex);
        }
    }

    const bool rectangular
        = draft.drawingType == PDFCommentAnnotation::DrawingType::Square
        || draft.drawingType == PDFCommentAnnotation::DrawingType::Circle;
    const qsizetype minimumVertexCount
        = draft.drawingType == PDFCommentAnnotation::DrawingType::Polygon
        ? 3
        : 2;
    if (!ink
        && (pageVertices.size() < minimumVertexCount
            || (rectangular && pageVertices.size() != 2)
            || (draft.drawingType == PDFCommentAnnotation::DrawingType::Line
                && pageVertices.size() != 2))) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    PDFCommentAnnotation::CommonProperties properties
        = draft.commentProperties;
    properties.opacity = std::clamp(properties.opacity, 0.0, 1.0);
    properties.borderWidth = coordinateMapper.strokeWidth(
        (std::max)(0.1, properties.borderWidth));
    properties.flags = properties.flags > 0
        ? properties.flags
        : FPDF_ANNOT_FLAG_PRINT;

    QRectF pageRect;
    if (rectangular || ink || !createPlaceholder) {
        FS_RECTF mappedRect{};
        if (!annotationRectForFrame(
                draft.frameRect,
                pageWidth,
                pageHeight,
                coordinateMapper,
                mappedRect)) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                draft.page,
                draft.objectId);
        }
        pageRect = QRectF(
            QPointF(mappedRect.left, mappedRect.bottom),
            QPointF(mappedRect.right, mappedRect.top)).normalized();
    } else {
        const QPointF firstPoint = ink
            ? pageStrokes.constFirst().constFirst()
            : pageVertices.constFirst();
        qreal minimumX = firstPoint.x();
        qreal maximumX = minimumX;
        qreal minimumY = firstPoint.y();
        qreal maximumY = minimumY;
        const auto includePoint = [
            &minimumX,
            &maximumX,
            &minimumY,
            &maximumY](const QPointF &point) {
            minimumX = (std::min)(minimumX, point.x());
            maximumX = (std::max)(maximumX, point.x());
            minimumY = (std::min)(minimumY, point.y());
            maximumY = (std::max)(maximumY, point.y());
        };
        if (ink) {
            for (const QVector<QPointF> &stroke : std::as_const(pageStrokes)) {
                for (const QPointF &point : stroke) {
                    includePoint(point);
                }
            }
        } else {
            for (const QPointF &vertex : std::as_const(pageVertices)) {
                includePoint(vertex);
            }
        }
        const qreal padding = (std::max)(0.5, properties.borderWidth / 2.0);
        pageRect = QRectF(
            QPointF(minimumX - padding, minimumY - padding),
            QPointF(maximumX + padding, maximumY + padding)).normalized();
    }
    if (!pageRect.isValid()
        || pageRect.width() <= 0.0
        || pageRect.height() <= 0.0) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    appearanceRequest.page = draft.page;
    appearanceRequest.annotationId = draft.objectId;
    appearanceRequest.type = draft.drawingType;
    appearanceRequest.pageRect = pageRect;
    appearanceRequest.pageVertices = std::move(pageVertices);
    appearanceRequest.pageStrokes = std::move(pageStrokes);
    appearanceRequest.properties = properties;
    if (!createPlaceholder) {
        return {};
    }

    const FS_RECTF annotationRect{
        static_cast<float>(pageRect.left()),
        static_cast<float>(pageRect.bottom()),
        static_cast<float>(pageRect.right()),
        static_cast<float>(pageRect.top())};
    PdfAnnotationHandle annotation(
        // Use one supported Square placeholder for every drawing subtype. The
        // canonicalizer atomically writes the final subtype, geometry (including
        // /InkList), properties, and /AP after serialization.
        FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE));
    if (!annotation.get()) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }
    const QColor stroke = properties.strokeColor.isValid()
        ? properties.strokeColor.toRgb()
        : QColor(Qt::black);
    const QString contents = pdfCommentContents(draft.text);
    const bool colorSet = FPDFAnnot_SetColor(
        annotation.get(),
        FPDFANNOT_COLORTYPE_Color,
        static_cast<unsigned int>(stroke.red()),
        static_cast<unsigned int>(stroke.green()),
        static_cast<unsigned int>(stroke.blue()),
        static_cast<unsigned int>(qRound(properties.opacity * 255.0)));
    bool interiorSet = true;
    if (properties.interiorColor.isValid()) {
        const QColor interior = properties.interiorColor.toRgb();
        interiorSet = FPDFAnnot_SetColor(
            annotation.get(),
            FPDFANNOT_COLORTYPE_InteriorColor,
            static_cast<unsigned int>(interior.red()),
            static_cast<unsigned int>(interior.green()),
            static_cast<unsigned int>(interior.blue()),
            static_cast<unsigned int>(qRound(properties.opacity * 255.0)));
    }
    if (!colorSet
        || !interiorSet
        || !FPDFAnnot_SetRect(annotation.get(), &annotationRect)
        || !FPDFAnnot_SetFlags(annotation.get(), properties.flags)
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            reinterpret_cast<const FPDF_WCHAR *>(contents.utf16()))
        || !setOptionalAnnotationString(
            annotation.get(),
            "Subj",
            properties.subject)
        || !setCommentAnnotationMetadata(annotation.get(), draft)) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    return {};
}

PDFSaveWriter::Result createStickyNoteAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper)
{
    if (!std::isfinite(draft.frameRect.x())
        || !std::isfinite(draft.frameRect.y())
        || !std::isfinite(draft.frameRect.width())
        || !std::isfinite(draft.frameRect.height())) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }
    const QRectF normalizedRect = draft.frameRect.normalized().intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    if (normalizedRect.isEmpty()) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    const QRectF deviceRect(
        normalizedRect.x() * pageWidth,
        normalizedRect.y() * pageHeight,
        normalizedRect.width() * pageWidth,
        normalizedRect.height() * pageHeight);
    const std::array<QPointF, 4> devicePoints{
        deviceRect.topLeft(),
        deviceRect.topRight(),
        deviceRect.bottomLeft(),
        deviceRect.bottomRight()};
    std::array<QPointF, 4> pagePoints;
    for (size_t index = 0; index < devicePoints.size(); ++index) {
        if (!coordinateMapper.pagePoint(
                devicePoints[index].x(),
                devicePoints[index].y(),
                pagePoints[index])
            || !std::isfinite(pagePoints[index].x())
            || !std::isfinite(pagePoints[index].y())) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                draft.page,
                draft.objectId);
        }
    }

    qreal minimumX = pagePoints.front().x();
    qreal maximumX = minimumX;
    qreal minimumY = pagePoints.front().y();
    qreal maximumY = minimumY;
    for (const QPointF &point : pagePoints) {
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    }
    const FS_RECTF annotationRect{
        static_cast<float>(minimumX),
        static_cast<float>(maximumY),
        static_cast<float>(maximumX),
        static_cast<float>(minimumY)};

    PdfAnnotationHandle annotation(
        FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT));
    const QColor color = draft.highlightColor.isValid()
        ? draft.highlightColor.toRgb()
        : QColor(255, 235, 59);
    const QString commentContents = pdfCommentContents(draft.text);
    const QString appearance = stickyNoteAppearance(annotationRect, color);
    const auto *contents = reinterpret_cast<const FPDF_WCHAR *>(
        commentContents.utf16());
    if (!annotation.get()
        || appearance.isEmpty()
        || !FPDFAnnot_SetRect(annotation.get(), &annotationRect)
        || !FPDFAnnot_SetColor(
            annotation.get(),
            FPDFANNOT_COLORTYPE_Color,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            contents)
        || !setCommentAnnotationMetadata(annotation.get(), draft)
        || !FPDFAnnot_SetFlags(
            annotation.get(),
            FPDF_ANNOT_FLAG_PRINT)
        || !FPDFAnnot_SetAP(
            annotation.get(),
            FPDF_ANNOT_APPEARANCEMODE_NORMAL,
            reinterpret_cast<const FPDF_WCHAR *>(appearance.utf16()))) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }
    return {};
}

PDFSaveWriter::Result createFreeTextAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper)
{
    FS_RECTF annotationRect{};
    if (!annotationRectForFrame(
            draft.frameRect,
            pageWidth,
            pageHeight,
            coordinateMapper,
            annotationRect)) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    PdfAnnotationHandle annotation(
        FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT));
    const QColor color = draft.highlightColor.isValid()
        ? draft.highlightColor.toRgb()
        : QColor(32, 33, 36);
    const QString commentContents = pdfCommentContents(draft.text);
    const auto *contents = reinterpret_cast<const FPDF_WCHAR *>(
        commentContents.utf16());
    static constexpr char16_t defaultAppearance[]
        = u"/Helv 12 Tf 0 g";
    if (!annotation.get()
        || !FPDFAnnot_SetRect(annotation.get(), &annotationRect)
        || !FPDFAnnot_SetColor(
            annotation.get(),
            FPDFANNOT_COLORTYPE_Color,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()))
        || !FPDFAnnot_SetBorder(annotation.get(), 0.0f, 0.0f, 1.0f)
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            contents)
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "DA",
            reinterpret_cast<const FPDF_WCHAR *>(defaultAppearance))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            kFreeTextAppearanceIdKey,
            reinterpret_cast<const FPDF_WCHAR *>(
                freeTextAppearanceId(draft).utf16()))
        || !setCommentAnnotationMetadata(annotation.get(), draft)
        || !FPDFAnnot_SetFlags(
            annotation.get(),
            FPDF_ANNOT_FLAG_PRINT)) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }
    return {};
}

PDFSaveWriter::Result createStampAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper,
    PDFAcroFormCanonicalizer::StampAnnotation &appearanceRequest)
{
    FS_RECTF annotationRect{};
    if (draft.objectId.isEmpty()
        || draft.stampName.isEmpty()
        || draft.stampText.isEmpty()
        || !annotationRectForFrame(
            draft.frameRect,
            pageWidth,
            pageHeight,
            coordinateMapper,
            annotationRect)) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    PdfAnnotationHandle annotation(
        FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP));
    const QColor color = draft.stampColor.isValid()
        ? draft.stampColor.toRgb()
        : QColor(97, 97, 97);
    const QString contents = pdfCommentContents(draft.text);
    if (!annotation.get()
        || !FPDFAnnot_SetRect(annotation.get(), &annotationRect)
        || !FPDFAnnot_SetColor(
            annotation.get(),
            FPDFANNOT_COLORTYPE_Color,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()))
        || !FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            reinterpret_cast<const FPDF_WCHAR *>(contents.utf16()))
        || !setCommentAnnotationMetadata(annotation.get(), draft)
        || !FPDFAnnot_SetFlags(annotation.get(), FPDF_ANNOT_FLAG_PRINT)) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationCreationFailed,
            draft.page,
            draft.objectId);
    }

    appearanceRequest.page = draft.page;
    appearanceRequest.annotationId = draft.objectId;
    appearanceRequest.stampName = draft.stampName;
    appearanceRequest.displayText = draft.stampText;
    appearanceRequest.detailText = draft.stampDetail;
    appearanceRequest.color = color;
    appearanceRequest.pageRect = QRectF(
        QPointF(annotationRect.left, annotationRect.bottom),
        QPointF(annotationRect.right, annotationRect.top)).normalized();
    return {};
}

FPDF_ANNOTATION_SUBTYPE sourceAnnotationSubtypeForDraft(
    const SaveDraft &draft)
{
    if (draft.kind == SaveDraft::Kind::StickyNote) {
        return FPDF_ANNOT_TEXT;
    }
    if (draft.kind == SaveDraft::Kind::FreeText) {
        return FPDF_ANNOT_FREETEXT;
    }
    if (draft.kind == SaveDraft::Kind::Stamp) {
        return FPDF_ANNOT_STAMP;
    }
    return draft.annotationSubtype;
}

QString plainTextCommentRichText(const QString &text)
{
    QString escapedText = normalizedCommentText(text).toHtmlEscaped();
    escapedText.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return QStringLiteral(
               "<?xml version=\"1.0\"?>"
               "<body xmlns=\"http://www.w3.org/1999/xhtml\"><p>")
        + escapedText
        + QStringLiteral("</p></body>");
}

PDFSaveWriter::Result updateSourceCommentAnnotation(
    FPDF_PAGE page,
    const SaveDraft &draft,
    int pageIndex,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper,
    QVector<PDFCommentAnnotation::SourceDrawingGeometryUpdate>
        &sourceDrawingGeometryUpdates,
    QVector<PDFAcroFormCanonicalizer::SourceCommentGeometryUpdate>
        &sourceCommentGeometryUpdates)
{
    if (draft.added || draft.deleted) {
        return {};
    }

    const int annotationCount = FPDFPage_GetAnnotCount(page);
    if (draft.sourceAnnotationIndex < 0
        || draft.sourceAnnotationIndex >= annotationCount) {
        return failure(
            PDFSaveWriter::Error::CommentAnnotationUpdateFailed,
            pageIndex,
            draft.objectId);
    }

    PdfAnnotationHandle annotation(FPDFPage_GetAnnot(
        page,
        draft.sourceAnnotationIndex));
    const int annotationFlags = annotation.get()
        ? FPDFAnnot_GetFlags(annotation.get())
        : 0;
    const bool commentTextReadOnly
        = (annotationFlags & FPDF_ANNOT_FLAG_READONLY) != 0
        || (annotationFlags & kAnnotationFlagLockedContents) != 0;
    const bool geometryComment
        = draft.kind == SaveDraft::Kind::TextMarkup
        || draft.kind == SaveDraft::Kind::StickyNote
        || draft.kind == SaveDraft::Kind::FreeText
        || draft.kind == SaveDraft::Kind::Stamp
        || draft.kind == SaveDraft::Kind::DrawingComment;
    const bool sourceGeometryChanged = geometryComment
        && draft.frameRect != draft.sourceRect;
    const bool sourceDrawingPropertiesChanged
        = draft.kind == SaveDraft::Kind::DrawingComment
        && draft.commentProperties != draft.originalCommentProperties;
    const bool sourceDrawingAppearanceMatches
        = !draft.sourceDrawingAppearanceFingerprint.isEmpty()
            && PDFAnnotationUtils::normalAppearanceFingerprint(
                   PDFAnnotationUtils::annotationNormalAppearance(
                       annotation.get()))
                == draft.sourceDrawingAppearanceFingerprint;
    const bool updatesSourceDrawingComment
        = draft.kind == SaveDraft::Kind::DrawingComment
        && (sourceGeometryChanged || sourceDrawingPropertiesChanged);
    const bool geometryLocked
        = commentTextReadOnly
        || (annotationFlags & FPDF_ANNOT_FLAG_LOCKED) != 0;
    const bool sourceHasName = annotation.get()
        && FPDFAnnot_HasKey(annotation.get(), "NM");
    const QString sourceName = sourceHasName
        ? annotationString(annotation.get(), "NM")
        : QString();
    const QString sourceThreadId = annotation.get()
        ? annotationString(
              annotation.get(),
              kCommentThreadIdKey).trimmed()
        : QString();
    QString drawingPrefix;
    switch (draft.drawingType) {
    case PDFCommentAnnotation::DrawingType::Square:
        drawingPrefix = QStringLiteral("square:");
        break;
    case PDFCommentAnnotation::DrawingType::Circle:
        drawingPrefix = QStringLiteral("circle:");
        break;
    case PDFCommentAnnotation::DrawingType::Line:
        drawingPrefix = QStringLiteral("line:");
        break;
    case PDFCommentAnnotation::DrawingType::Polygon:
        drawingPrefix = QStringLiteral("polygon:");
        break;
    case PDFCommentAnnotation::DrawingType::PolyLine:
        drawingPrefix = QStringLiteral("polyLine:");
        break;
    case PDFCommentAnnotation::DrawingType::Ink:
        drawingPrefix = QStringLiteral("ink:");
        break;
    }
    const QString sourceDrawingUuidText = sourceName.startsWith(
        drawingPrefix)
        ? sourceName.mid(drawingPrefix.size())
        : QString();
    const QUuid sourceDrawingUuid = QUuid::fromString(
        sourceDrawingUuidText);
    const bool hasTrePDFManagedDrawingAppearance
        = !sourceDrawingUuid.isNull()
        && sourceDrawingUuid.toString(QUuid::WithoutBraces).compare(
               sourceDrawingUuidText,
               Qt::CaseInsensitive) == 0
        && sourceThreadId == sourceName;
    const bool sourceIdentityMatches = annotation.get()
        && normalizedCommentText(
               annotationString(annotation.get(), "Contents"))
            == normalizedCommentText(draft.originalText)
        && sourceHasName == draft.sourceAnnotationHasName
        && (!sourceHasName
            || sourceName == draft.sourceAnnotationName);
    const bool textChanged
        = normalizedCommentText(draft.text)
        != normalizedCommentText(draft.originalText);
    const bool commentStateChanged
        = draft.commentStatus != draft.originalCommentStatus
        || draft.commentMarked != draft.originalCommentMarked;
    const QString modificationDate = pdfAnnotationDateString(
        draft.commentModifiedDate);
    bool updated = annotation.get()
        && !commentTextReadOnly
        && sourceIdentityMatches
        && FPDFAnnot_GetSubtype(annotation.get())
            == sourceAnnotationSubtypeForDraft(draft)
        && (!(sourceGeometryChanged || sourceDrawingPropertiesChanged)
            || !geometryLocked)
        && (!sourceDrawingPropertiesChanged
            || (hasTrePDFManagedDrawingAppearance
                && sourceDrawingAppearanceMatches));
    if (updated && textChanged) {
        const QString commentContents = pdfCommentContents(draft.text);
        updated = FPDFAnnot_SetStringValue(
            annotation.get(),
            "Contents",
            reinterpret_cast<const FPDF_WCHAR *>(
                commentContents.utf16()));
    }
    if (updated
        && textChanged
        && FPDFAnnot_HasKey(annotation.get(), "RC")) {
        const QString richText = plainTextCommentRichText(draft.text);
        updated = FPDFAnnot_SetStringValue(
            annotation.get(),
            "RC",
            reinterpret_cast<const FPDF_WCHAR *>(richText.utf16()));
    }
    if (updated && commentStateChanged) {
        updated = !draft.commentThreadId.isEmpty()
            && (sourceThreadId.isEmpty()
                || sourceThreadId == draft.commentThreadId)
            && setOptionalAnnotationString(
                annotation.get(),
                kCommentThreadIdKey,
                draft.commentThreadId);
    }
    if (updated
        && (textChanged
            || sourceGeometryChanged
            || sourceDrawingPropertiesChanged)) {
        updated = setOptionalAnnotationString(
            annotation.get(),
            "M",
            modificationDate);
    }
    if (updated && draft.kind == SaveDraft::Kind::StickyNote) {
        const unsigned long appearanceBytes = FPDFAnnot_GetAP(
            annotation.get(),
            FPDF_ANNOT_APPEARANCEMODE_NORMAL,
            nullptr,
            0);
        if (appearanceBytes <= sizeof(FPDF_WCHAR)) {
            FS_RECTF annotationRect{};
            updated = sourceGeometryChanged
                ? annotationRectForFrame(
                      draft.frameRect,
                      pageWidth,
                      pageHeight,
                      coordinateMapper,
                      annotationRect)
                    && FPDFAnnot_SetRect(
                        annotation.get(),
                        &annotationRect)
                : FPDFAnnot_GetRect(annotation.get(), &annotationRect);
            const bool hasUsableRect = updated
                && annotationRect.right > annotationRect.left
                && annotationRect.top > annotationRect.bottom;
            if (hasUsableRect) {
                unsigned int red = 255;
                unsigned int green = 235;
                unsigned int blue = 59;
                unsigned int alpha = 255;
                FPDFAnnot_GetColor(
                    annotation.get(),
                    FPDFANNOT_COLORTYPE_Color,
                    &red,
                    &green,
                    &blue,
                    &alpha);
                const QString appearance = stickyNoteAppearance(
                    annotationRect,
                    QColor(
                        static_cast<int>(red),
                        static_cast<int>(green),
                        static_cast<int>(blue),
                        static_cast<int>(alpha)));
                updated = !appearance.isEmpty()
                    && FPDFAnnot_SetAP(
                        annotation.get(),
                        FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                        reinterpret_cast<const FPDF_WCHAR *>(
                            appearance.utf16()));
            } else {
                updated = updated && draft.reply;
            }
        }
    }
    if (updated && draft.kind == SaveDraft::Kind::FreeText) {
        FS_RECTF annotationRect{};
        static constexpr char16_t defaultAppearance[]
            = u"/Helv 12 Tf 0 g";
        updated = annotationRectForFrame(
                      draft.frameRect,
                      pageWidth,
                      pageHeight,
                      coordinateMapper,
                      annotationRect)
            && FPDFAnnot_SetRect(annotation.get(), &annotationRect)
            && (draft.sourceAnnotationName.isEmpty()
                ? FPDFAnnot_SetStringValue(
                      annotation.get(),
                      "NM",
                      reinterpret_cast<const FPDF_WCHAR *>(
                          draft.objectId.utf16()))
                : true)
            && (FPDFAnnot_HasKey(annotation.get(), "DA")
                || FPDFAnnot_SetStringValue(
                    annotation.get(),
                    "DA",
                    reinterpret_cast<const FPDF_WCHAR *>(
                        defaultAppearance)))
            && FPDFAnnot_SetStringValue(
                annotation.get(),
                kFreeTextAppearanceIdKey,
                reinterpret_cast<const FPDF_WCHAR *>(
                    freeTextAppearanceId(draft).utf16()))
            && FPDFAnnot_SetAP(
                annotation.get(),
                FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                nullptr);
    }
    if (updated && updatesSourceDrawingComment) {
        PDFCommentAnnotation::DrawingAnnotation appearanceRequest;
        const PDFSaveWriter::Result geometryResult
            = prepareDrawingCommentAnnotation(
                page,
                draft,
                pageWidth,
                pageHeight,
                coordinateMapper,
                false,
                appearanceRequest);
        if (!geometryResult.succeeded()) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationUpdateFailed,
                pageIndex,
                draft.objectId);
        }
        // FPDFAnnot_SetRect() may enlarge the normal appearance's /BBox when
        // the new rectangle contains it. That leaves the old drawing commands
        // small inside the resized BBox. Mark this exact source annotation and
        // patch /Rect plus subtype geometry after PDFium serialization.
        const QString markerId = QUuid::createUuid().toString(
            QUuid::WithoutBraces);
        updated = !FPDFAnnot_HasKey(
                       annotation.get(),
                       PDFCommentAnnotation::kSourceDrawingGeometryMarkerKey)
            && FPDFAnnot_SetStringValue(
                annotation.get(),
                PDFCommentAnnotation::kSourceDrawingGeometryMarkerKey,
                reinterpret_cast<const FPDF_WCHAR *>(markerId.utf16()));
        if (updated) {
            constexpr qreal geometryEpsilon = 1e-9;
            const QRectF sourceRect = draft.sourceRect.normalized();
            const QRectF targetRect = draft.frameRect.normalized();
            const bool resized
                = qAbs(sourceRect.width() - targetRect.width())
                        > geometryEpsilon
                || qAbs(sourceRect.height() - targetRect.height())
                        > geometryEpsilon;
            sourceDrawingGeometryUpdates.append({
                markerId,
                std::move(appearanceRequest),
                sourceGeometryChanged,
                sourceDrawingPropertiesChanged,
                sourceDrawingPropertiesChanged
                    || (resized
                        && hasTrePDFManagedDrawingAppearance
                        && sourceDrawingAppearanceMatches)});
        }
    }
    if (updated
        && sourceGeometryChanged
        && draft.kind != SaveDraft::Kind::FreeText
        && draft.kind != SaveDraft::Kind::DrawingComment) {
        FS_RECTF annotationRect{};
        updated = annotationRectForFrame(
            draft.frameRect,
            pageWidth,
            pageHeight,
            coordinateMapper,
            annotationRect);
        PDFAcroFormCanonicalizer::SourceCommentGeometryUpdate update;
        update.markerId = QUuid::createUuid().toString(
            QUuid::WithoutBraces);
        update.page = draft.page;
        update.pageRect = QRectF(
            QPointF(annotationRect.left, annotationRect.bottom),
            QPointF(annotationRect.right, annotationRect.top)).normalized();
        if (draft.kind == SaveDraft::Kind::StickyNote) {
            update.type = PDFAcroFormCanonicalizer::
                SourceCommentGeometryType::StickyNote;
        } else if (draft.kind == SaveDraft::Kind::Stamp) {
            update.type = PDFAcroFormCanonicalizer::
                SourceCommentGeometryType::Stamp;
        } else {
            switch (draft.annotationSubtype) {
            case FPDF_ANNOT_UNDERLINE:
                update.type = PDFAcroFormCanonicalizer::
                    SourceCommentGeometryType::Underline;
                break;
            case FPDF_ANNOT_SQUIGGLY:
                update.type = PDFAcroFormCanonicalizer::
                    SourceCommentGeometryType::Squiggly;
                break;
            case FPDF_ANNOT_STRIKEOUT:
                update.type = PDFAcroFormCanonicalizer::
                    SourceCommentGeometryType::StrikeOut;
                break;
            case FPDF_ANNOT_HIGHLIGHT:
                update.type = PDFAcroFormCanonicalizer::
                    SourceCommentGeometryType::Highlight;
                break;
            default:
                updated = false;
                break;
            }
            if (updated && draft.textMarkupQuads.isEmpty()) {
                updated = false;
            }
            if (updated) {
                update.pageQuadPoints.reserve(
                    draft.textMarkupQuads.size());
                for (const std::array<QPointF, 4> &normalizedQuad
                     : draft.textMarkupQuads) {
                    std::array<QPointF, 4> pageQuad;
                    for (size_t pointIndex = 0;
                         pointIndex < normalizedQuad.size();
                         ++pointIndex) {
                        const QPointF &point = normalizedQuad.at(pointIndex);
                        if (!coordinateMapper.pagePoint(
                                point.x() * pageWidth,
                                point.y() * pageHeight,
                                pageQuad.at(pointIndex))) {
                            updated = false;
                            break;
                        }
                    }
                    if (!updated) {
                        break;
                    }
                    update.pageQuadPoints.append(pageQuad);
                }
            }
        }
        updated = updated
            && !FPDFAnnot_HasKey(
                annotation.get(),
                PDFAcroFormCanonicalizer::
                    kSourceCommentGeometryMarkerKey)
            && FPDFAnnot_SetStringValue(
                annotation.get(),
                PDFAcroFormCanonicalizer::
                    kSourceCommentGeometryMarkerKey,
                reinterpret_cast<const FPDF_WCHAR *>(
                    update.markerId.utf16()));
        if (updated) {
            sourceCommentGeometryUpdates.append(std::move(update));
        }
    }
    return updated
        ? PDFSaveWriter::Result{}
        : failure(
            PDFSaveWriter::Error::CommentAnnotationUpdateFailed,
            pageIndex,
            draft.objectId);
}

PDFSaveWriter::Result assignSourceReplyParentThreadIds(
    FPDF_PAGE page,
    const QVector<SaveDraft> &drafts,
    int pageIndex)
{
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    QHash<int, QString> threadIdsByAnnotation;
    for (const SaveDraft &draft : drafts) {
        if (!draft.added
            || !draft.reply
            || draft.deleted
            || draft.replyToSourceAnnotationIndex < 0) {
            continue;
        }
        const auto existing = threadIdsByAnnotation.constFind(
            draft.replyToSourceAnnotationIndex);
        if (draft.replyToThreadId.isEmpty()
            || (existing != threadIdsByAnnotation.cend()
                && *existing != draft.replyToThreadId)) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                pageIndex,
                draft.objectId);
        }
        threadIdsByAnnotation.insert(
            draft.replyToSourceAnnotationIndex,
            draft.replyToThreadId);
    }

    for (auto thread = threadIdsByAnnotation.cbegin();
         thread != threadIdsByAnnotation.cend();
         ++thread) {
        if (thread.key() < 0 || thread.key() >= annotationCount) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                pageIndex,
                thread.value());
        }
        PdfAnnotationHandle parent(FPDFPage_GetAnnot(page, thread.key()));
        PdfAnnotationHandle inReplyTo(parent.get()
                ? FPDFAnnot_GetLinkedAnnot(parent.get(), "IRT")
                : nullptr);
        const QString currentThreadId = parent.get()
            ? annotationString(parent.get(), kCommentThreadIdKey).trimmed()
            : QString();
        if (!parent.get()
            || !isSupportedCommentAnnotationSubtype(
                FPDFAnnot_GetSubtype(parent.get()))
            || inReplyTo.get()
            || (!currentThreadId.isEmpty()
                && currentThreadId != thread.value())
            || !setOptionalAnnotationString(
                parent.get(),
                kCommentThreadIdKey,
                thread.value())) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                pageIndex,
                thread.value());
        }
    }
    return {};
}

int popupAnnotationIndexForSourceComment(FPDF_PAGE page,
                                         FPDF_ANNOTATION comment,
                                         int commentIndex,
                                         int annotationCount,
                                         bool &valid)
{
    assertPDFiumProcessLockHeld();
    valid = false;

    int forwardPopupIndex = -1;
    PdfAnnotationHandle forwardPopup(FPDFAnnot_GetLinkedAnnot(
        comment,
        "Popup"));
    if (forwardPopup.get()) {
        forwardPopupIndex = FPDFPage_GetAnnotIndex(
            page,
            forwardPopup.get());
        if (FPDFAnnot_GetSubtype(forwardPopup.get()) != FPDF_ANNOT_POPUP
            || forwardPopupIndex < 0
            || forwardPopupIndex >= annotationCount) {
            return -1;
        }
    }

    int reversePopupIndex = -1;
    for (int annotationIndex = 0;
         annotationIndex < annotationCount;
         ++annotationIndex) {
        PdfAnnotationHandle popup(FPDFPage_GetAnnot(page, annotationIndex));
        if (!popup.get()
            || FPDFAnnot_GetSubtype(popup.get()) != FPDF_ANNOT_POPUP) {
            continue;
        }
        PdfAnnotationHandle parent(FPDFAnnot_GetLinkedAnnot(
            popup.get(),
            "Parent"));
        if (!parent.get()
            || FPDFPage_GetAnnotIndex(page, parent.get()) != commentIndex) {
            continue;
        }
        if (reversePopupIndex >= 0) {
            return -1;
        }
        reversePopupIndex = annotationIndex;
    }

    if (forwardPopupIndex >= 0
        && reversePopupIndex != forwardPopupIndex) {
        return -1;
    }
    valid = true;
    return forwardPopupIndex >= 0
        ? forwardPopupIndex
        : reversePopupIndex;
}

PDFSaveWriter::Result removeDeletedCommentAnnotations(
    FPDF_PAGE page,
    const QVector<SaveDraft> &drafts,
    int pageIndex)
{
    struct AnnotationRemoval {
        int annotationIndex = -1;
        QString objectId;
    };

    QHash<int, QString> removalObjectIds;
    QSet<int> sourceAnnotationIndices;
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    for (const SaveDraft &draft : drafts) {
        const bool commentDraft
            = draft.kind == SaveDraft::Kind::TextMarkup
            || draft.kind == SaveDraft::Kind::StickyNote
            || draft.kind == SaveDraft::Kind::FreeText
            || draft.kind == SaveDraft::Kind::Stamp
            || draft.kind == SaveDraft::Kind::DrawingComment;
        if (!commentDraft || !draft.deleted || draft.added) {
            continue;
        }
        if (draft.sourceAnnotationIndex < 0
            || draft.sourceAnnotationIndex >= annotationCount
            || sourceAnnotationIndices.contains(draft.sourceAnnotationIndex)) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                pageIndex,
                draft.objectId);
        }
        sourceAnnotationIndices.insert(draft.sourceAnnotationIndex);

        PdfAnnotationHandle annotation(FPDFPage_GetAnnot(
            page,
            draft.sourceAnnotationIndex));
        if (!annotation.get()
            || FPDFAnnot_GetSubtype(annotation.get())
                != sourceAnnotationSubtypeForDraft(draft)) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                pageIndex,
                draft.objectId);
        }
        const int annotationFlags = FPDFAnnot_GetFlags(annotation.get());
        if ((annotationFlags
             & (FPDF_ANNOT_FLAG_READONLY
                | FPDF_ANNOT_FLAG_LOCKED
                | kAnnotationFlagLockedContents)) != 0) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                pageIndex,
                draft.objectId);
        }
        removalObjectIds.insert(
            draft.sourceAnnotationIndex,
            draft.objectId);
    }

    if (sourceAnnotationIndices.isEmpty()) {
        return {};
    }

    for (int annotationIndex = 0;
         annotationIndex < annotationCount;
         ++annotationIndex) {
        if (sourceAnnotationIndices.contains(annotationIndex)) {
            continue;
        }
        PdfAnnotationHandle annotation(FPDFPage_GetAnnot(
            page,
            annotationIndex));
        if (!annotation.get()
            || !annotationUsesReplyRelation(annotation.get())) {
            continue;
        }

        bool markupReplyChain = isMarkupAnnotation(annotation.get());
        QSet<int> visited{annotationIndex};
        FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
            annotation.get(),
            "IRT");
        while (parent) {
            const int parentIndex = FPDFPage_GetAnnotIndex(page, parent);
            if (parentIndex < 0 || visited.contains(parentIndex)) {
                FPDFPage_CloseAnnot(parent);
                break;
            }
            visited.insert(parentIndex);
            if (sourceAnnotationIndices.contains(parentIndex)) {
                const QString rootObjectId = removalObjectIds.value(
                    parentIndex);
                if (!markupReplyChain) {
                    FPDFPage_CloseAnnot(parent);
                    return failure(
                        PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                        pageIndex,
                        rootObjectId);
                }
                const int annotationFlags
                    = FPDFAnnot_GetFlags(annotation.get());
                if ((annotationFlags
                     & (FPDF_ANNOT_FLAG_READONLY
                        | FPDF_ANNOT_FLAG_LOCKED
                        | kAnnotationFlagLockedContents)) != 0) {
                    FPDFPage_CloseAnnot(parent);
                    return failure(
                        PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                        pageIndex,
                        rootObjectId);
                }
                removalObjectIds.insert(
                    annotationIndex,
                    rootObjectId);
                FPDFPage_CloseAnnot(parent);
                break;
            }
            if (!annotationUsesReplyRelation(parent)) {
                FPDFPage_CloseAnnot(parent);
                break;
            }
            markupReplyChain = markupReplyChain
                && isMarkupAnnotation(parent);
            FPDF_ANNOTATION nextParent = FPDFAnnot_GetLinkedAnnot(
                parent,
                "IRT");
            FPDFPage_CloseAnnot(parent);
            parent = nextParent;
        }
    }

    const QList<int> commentIndices = removalObjectIds.keys();
    for (int commentIndex : commentIndices) {
        PdfAnnotationHandle annotation(FPDFPage_GetAnnot(page, commentIndex));
        if (!annotation.get()) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                pageIndex,
                removalObjectIds.value(commentIndex));
        }
        bool popupValid = false;
        const int popupIndex = popupAnnotationIndexForSourceComment(
            page,
            annotation.get(),
            commentIndex,
            annotationCount,
            popupValid);
        if (!popupValid) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                pageIndex,
                removalObjectIds.value(commentIndex));
        }
        if (popupIndex >= 0 && !removalObjectIds.contains(popupIndex)) {
            removalObjectIds.insert(
                popupIndex,
                removalObjectIds.value(commentIndex));
        }
    }

    QVector<AnnotationRemoval> removals;
    removals.reserve(removalObjectIds.size());
    for (auto removal = removalObjectIds.cbegin();
         removal != removalObjectIds.cend();
         ++removal) {
        removals.append({removal.key(), removal.value()});
    }

    std::sort(
        removals.begin(),
        removals.end(),
        [](const AnnotationRemoval &left,
           const AnnotationRemoval &right) {
            return left.annotationIndex > right.annotationIndex;
        });
    for (const AnnotationRemoval &removal : std::as_const(removals)) {
        if (!FPDFPage_RemoveAnnot(page, removal.annotationIndex)) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationRemovalFailed,
                pageIndex,
                removal.objectId);
        }
    }
    return {};
}

PDFSaveWriter::Result applyPageDrafts(FPDF_DOCUMENT document,
                                      FPDF_DOCUMENT inspectionDocument,
                                      int pageIndex,
                                      const QVector<SaveDraft> &drafts,
                                      FontResolver &fontResolver,
                                      QVector<PDFAcroFormCanonicalizer::AnnotationRelationship>
                                          &annotationRelationships,
                                      QVector<PDFAcroFormCanonicalizer::StampAnnotation>
                                          &stampAnnotations,
                                      QVector<PDFCommentAnnotation::DrawingAnnotation>
                                          &drawingAnnotations,
                                      QVector<PDFCommentAnnotation::SourceDrawingGeometryUpdate>
                                          &sourceDrawingGeometryUpdates,
                                      QVector<PDFAcroFormCanonicalizer::SourceCommentGeometryUpdate>
                                          &sourceCommentGeometryUpdates,
                                      QSet<int> &imageFormPages)
{
    assertPDFiumProcessLockHeld();
    FPDF_PAGE loadedPage = FPDF_LoadPage(document, pageIndex);
    PdfPageHandle page(loadedPage);
    if (!page.get()) {
        return failure(PDFSaveWriter::Error::PageLoadFailed, pageIndex);
    }
    PdfPageHandle inspectionPage(FPDF_LoadPage(inspectionDocument, pageIndex));
    if (!inspectionPage.get()) {
        return failure(PDFSaveWriter::Error::PageLoadFailed, pageIndex);
    }

    double pageWidth = 0.0;
    double pageHeight = 0.0;
    if (!FPDF_GetPageSizeByIndex(
            document,
            pageIndex,
            &pageWidth,
            &pageHeight)
        || pageWidth <= 0.0
        || pageHeight <= 0.0) {
        return failure(PDFSaveWriter::Error::PageLoadFailed, pageIndex);
    }

    const PageCoordinateMapper coordinateMapper(
        page.get(),
        pageWidth,
        pageHeight);
    RasterizedPaints rasterizedPaints;
    // Validate indexed shapes and text occurrences before the paint-preservation
    // pass can replace source objects. A text-only Form must still have its
    // bounded, safe single-child structure, including when it is deleted.
    for (const SaveDraft &draft : drafts) {
        if ((draft.kind != SaveDraft::Kind::Shape && draft.kind != SaveDraft::Kind::Text)
            || draft.added) {
            continue;
        }
        PDFSaveWriter::Result sourceResult;
        const QVector<SourceObject> sources = sourceObjectsForDraft(
            page.get(), draft, sourceResult);
        if (!sourceResult.succeeded()) {
            return sourceResult;
        }
        if (draft.kind == SaveDraft::Kind::Shape && sources.size() != 1) {
            return failure(PDFSaveWriter::Error::SourceObjectMissing,
                           pageIndex, draft.objectId);
        }
    }
    const bool hasPageObjectDrafts = std::any_of(
        drafts.cbegin(),
        drafts.cend(),
        [](const SaveDraft &draft) {
            return draft.kind != SaveDraft::Kind::TextMarkup
                && draft.kind != SaveDraft::Kind::StickyNote
                && draft.kind != SaveDraft::Kind::FreeText
                && draft.kind != SaveDraft::Kind::Stamp
                && draft.kind != SaveDraft::Kind::DrawingComment;
        });
    const bool modifiesExistingObject = std::any_of(
        drafts.cbegin(),
        drafts.cend(),
        [](const SaveDraft &draft) {
            return draft.kind != SaveDraft::Kind::TextMarkup
                && draft.kind != SaveDraft::Kind::StickyNote
                && draft.kind != SaveDraft::Kind::FreeText
                && draft.kind != SaveDraft::Kind::Stamp
                && draft.kind != SaveDraft::Kind::DrawingComment
                && !draft.added;
        });
    if (hasPageObjectDrafts) {
        // Retained Form occurrences can also rely on page resources that the
        // generator prunes when a neighboring object is edited or added.
        for (int index = 0; index < FPDFPage_CountObjects(page.get()); ++index) {
            const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page.get(), index);
            if (PDFSourceImageGeometry::isImageOnlyForm(object)
                || PDFSourceTextGeometry::isTextOnlyForm(object)) {
                imageFormPages.insert(pageIndex);
                break;
            }
        }
        QSet<int> removedTextObjectIndexes;
        QSet<int> retainedTextObjectIndexes;
        for (const SaveDraft &draft : drafts) {
            if (draft.kind != SaveDraft::Kind::Text || draft.added) {
                continue;
            }
            QSet<int> &objectIndexes = draft.movesSourceObjects
                ? retainedTextObjectIndexes
                : removedTextObjectIndexes;
            for (const QVector<int> &path : draft.sourceObjectPaths) {
                if (path.size() == 1) {
                    objectIndexes.insert(path.constFirst());
                }
            }
        }
        removedTextObjectIndexes.subtract(retainedTextObjectIndexes);
        const PDFSaveWriter::Result textPaintResult = materializeTextPaints(
            page.get(),
            inspectionPage.get(),
            pageIndex,
            removedTextObjectIndexes);
        if (!textPaintResult.succeeded()) {
            return textPaintResult;
        }
    }
    if (modifiesExistingObject) {
        // PDFium's content generator cannot serialize pattern and shading
        // paints reliably, and can lose implicit opaque-white path fills while
        // replacing nearby source objects. Preserve those paints before the
        // source content stream is regenerated.
        const PDFSaveWriter::Result paintResult = prepareUnsupportedPaints(
            document,
            page.get(),
            inspectionPage.get(),
            pageIndex,
            rasterizedPaints);
        if (!paintResult.succeeded()) {
            return paintResult;
        }
        if (!replaceUnsupportedPaints(page.get(), rasterizedPaints)) {
            return failure(
                PDFSaveWriter::Error::ImageObjectCreationFailed,
                pageIndex);
        }
    }
    fontResolver.beginPage();
    FPDF_TEXTPAGE sourceTextPage = nullptr;
    const auto closeSourceTextPage = qScopeGuard([&]() {
        if (sourceTextPage)
            FPDFText_ClosePage(sourceTextPage);
    });
    const int originalObjectCount = FPDFPage_CountObjects(page.get());
    QVector<PreparedDraft> preparedDrafts;
    preparedDrafts.reserve(drafts.size());
    for (const SaveDraft &draft : drafts) {
        if (draft.kind == SaveDraft::Kind::TextMarkup
            || draft.kind == SaveDraft::Kind::StickyNote
            || draft.kind == SaveDraft::Kind::FreeText
            || draft.kind == SaveDraft::Kind::Stamp
            || draft.kind == SaveDraft::Kind::DrawingComment) {
            continue;
        }
        PreparedDraft prepared;
        prepared.kind = draft.kind;
        prepared.objectId = draft.objectId;
        PDFSaveWriter::Result sourceResult;
        prepared.sources = sourceObjectsForDraft(
            page.get(),
            draft,
            sourceResult);
        if (!sourceResult.succeeded()) {
            destroyPreparedObjects(preparedDrafts);
            return sourceResult;
        }
        prepared.append = prepared.sources.isEmpty();
        prepared.movesSourceObjects = draft.movesSourceObjects;
        prepared.sourceRect = draft.sourceRect;
        prepared.frameRect = draft.frameRect;
        prepared.imageQuarterTurns = draft.imageQuarterTurns;
        prepared.insertionIndex = prepared.append
            ? originalObjectCount
            : prepared.sources.constFirst().topLevelIndex;
        for (const SourceObject &source : std::as_const(prepared.sources)) {
            prepared.insertionIndex = qMin(
                prepared.insertionIndex,
                source.topLevelIndex);
        }

        FPDF_FONT originalFont = prepared.kind == SaveDraft::Kind::Text
                && !prepared.sources.isEmpty()
            ? FPDFTextObj_GetFont(PDFSourceTextGeometry::textObject(
                  prepared.sources.constFirst().object))
            : nullptr;
        QVector<FPDF_FONT> sourceFonts;
        if (prepared.kind == SaveDraft::Kind::Text) {
            fontResolver.collectOriginalType1Fonts(page.get(), prepared.sources, sourceTextPage);
            for (const SourceObject &source : std::as_const(prepared.sources)) {
                const FPDF_FONT font = FPDFTextObj_GetFont(
                    PDFSourceTextGeometry::textObject(source.object));
                if (font && !sourceFonts.contains(font))
                    sourceFonts.append(font);
                if (font != originalFont) {
                    originalFont = nullptr;
                }
            }
        }
        // Decorations share text ownership, but must not participate in
        // original-font resolution or the text-only source-object contract.
        const QVector<SourceObject> underlineSources = sourceUnderlineObjectsForDraft(
            page.get(),
            draft,
            sourceResult);
        if (!sourceResult.succeeded()) {
            destroyPreparedObjects(preparedDrafts);
            return sourceResult;
        }
        for (const SourceObject &source : underlineSources) {
            prepared.insertionIndex = qMin(
                prepared.insertionIndex,
                source.topLevelIndex);
            prepared.sources.append(source);
        }
        const PDFSaveWriter::Result createResult = createDraftObjects(
            document,
            page.get(),
            draft,
            pageWidth,
            pageHeight,
            originalFont,
            sourceFonts,
            fontResolver,
            coordinateMapper,
            prepared.objects,
            prepared.bitmaps);
        if (!createResult.succeeded()) {
            destroyObjects(prepared.objects);
            destroyBitmaps(prepared.bitmaps);
            destroyPreparedObjects(preparedDrafts);
            return createResult;
        }
        preparedDrafts.append(std::move(prepared));
    }

    // Text extraction borrows original page objects. Release it before those
    // objects are moved/replaced; new text objects retain their font handles.
    if (sourceTextPage) {
        FPDFText_ClosePage(sourceTextPage);
        sourceTextPage = nullptr;
    }

    for (qsizetype index = 0; index < preparedDrafts.size(); ++index) {
        PreparedDraft &prepared = preparedDrafts[index];
        if (!prepared.movesSourceObjects) {
            continue;
        }
        if (prepared.sources.isEmpty()) {
            destroyPreparedObjects(preparedDrafts);
            return failure(
                PDFSaveWriter::Error::UnsupportedSourceObject,
                pageIndex);
        }

        SaveDraft moveDraft;
        moveDraft.kind = prepared.kind;
        moveDraft.page = pageIndex;
        moveDraft.objectId = prepared.objectId;
        moveDraft.sourceRect = prepared.sourceRect;
        moveDraft.frameRect = prepared.frameRect;
        moveDraft.imageQuarterTurns = prepared.imageQuarterTurns;
        for (const SourceObject &source : std::as_const(prepared.sources)) {
            const PDFSaveWriter::Result moveResult = moveSourceObject(
                page.get(),
                moveDraft,
                source,
                pageWidth,
                pageHeight,
                coordinateMapper);
            if (!moveResult.succeeded()) {
                destroyPreparedObjects(preparedDrafts);
                return moveResult;
            }
        }
    }

    QHash<int, QRectF> changedPaintBounds;
    for (const PreparedDraft &prepared : std::as_const(preparedDrafts)) {
        QRectF bounds;
        const auto includeBounds = [&](FPDF_PAGEOBJECT object) {
            QRectF objectBounds;
            if (!pageObjectBounds(object, objectBounds))
                return false;
            bounds = bounds.united(objectBounds);
            return true;
        };
        bool valid = true;
        if (prepared.movesSourceObjects) {
            for (const SourceObject &source : prepared.sources)
                valid = includeBounds(source.object) && valid;
        } else {
            for (FPDF_PAGEOBJECT object : prepared.objects)
                valid = includeBounds(object) && valid;
        }
        if (!valid) {
            destroyPreparedObjects(preparedDrafts);
            return failure(PDFSaveWriter::Error::UnsupportedSourceObject,
                           pageIndex, prepared.objectId);
        }
        for (const SourceObject &source : prepared.sources)
            changedPaintBounds.insert(source.topLevelIndex, bounds);
    }
    for (PreparedDraft &prepared : preparedDrafts) {
        if (!prepareTextPaintOrder(page.get(), prepared, changedPaintBounds)) {
            destroyPreparedObjects(preparedDrafts);
            return failure(PDFSaveWriter::Error::UnsupportedSourceObject,
                           pageIndex, prepared.objectId);
        }
    }

    QSet<FPDF_PAGEOBJECT> removed;
    QSet<int> removedIndexes;
    for (PreparedDraft &prepared : preparedDrafts) {
        if (!prepared.movesSourceObjects) {
            for (const SourceObject &source : std::as_const(prepared.sources)) {
                if (removed.contains(source.object)) {
                    continue;
                }
                if (source.parentForm) {
                    destroyPreparedObjects(preparedDrafts);
                    return failure(
                        PDFSaveWriter::Error::UnsupportedSourceObject,
                        pageIndex);
                }
                const bool succeeded = FPDFPage_RemoveObject(
                    page.get(),
                    source.object);
                if (!succeeded) {
                    destroyPreparedObjects(preparedDrafts);
                    return failure(
                        PDFSaveWriter::Error::SourceObjectRemovalFailed,
                        pageIndex);
                }
                removed.insert(source.object);
                removedIndexes.insert(source.topLevelIndex);
                FPDFPageObj_Destroy(source.object);
            }
        }
    }

    // Boundaries refer to the original page, not a shifting object index.
    // Remove all sources first, then translate each boundary to the number of
    // surviving original objects before it. Descending insertion preserves
    // ordering even when several drafts have interleaved source objects.
    QVector<int> survivingBefore(originalObjectCount + 1);
    for (int index = 0; index < originalObjectCount; ++index)
        survivingBefore[index + 1] = survivingBefore.at(index)
            + (removedIndexes.contains(index) ? 0 : 1);
    struct ObjectInsertion {
        qsizetype draft;
        qsizetype object;
        int boundary;
    };
    QVector<ObjectInsertion> insertions;
    for (qsizetype draft = 0; draft < preparedDrafts.size(); ++draft) {
        const auto &prepared = preparedDrafts.at(draft);
        for (qsizetype index = 0; index < prepared.objects.size(); ++index)
            insertions.append({draft, index, prepared.objectInsertionBoundaries.at(index)});
    }
    std::stable_sort(insertions.begin(), insertions.end(),
        [](const ObjectInsertion &left, const ObjectInsertion &right) {
            return left.boundary < right.boundary;
        });
    for (auto it = insertions.crbegin(); it != insertions.crend(); ++it) {
        auto &prepared = preparedDrafts[it->draft];
        FPDF_PAGEOBJECT object = prepared.objects.at(it->object);
        if (!FPDFPage_InsertObjectAtIndex(
                page.get(),
                object,
                static_cast<size_t>(survivingBefore.at(it->boundary)))) {
            prepared.objects[it->object] = nullptr;
            destroyPreparedObjects(preparedDrafts);
            const PDFSaveWriter::Error creationError
                = prepared.kind == SaveDraft::Kind::Image
                ? PDFSaveWriter::Error::ImageObjectCreationFailed
                : prepared.kind == SaveDraft::Kind::Shape
                    ? PDFSaveWriter::Error::ShapeObjectCreationFailed
                    : PDFSaveWriter::Error::TextObjectCreationFailed;
            return failure(
                creationError,
                pageIndex);
        }
        prepared.objects[it->object] = nullptr;
    }

    if (hasPageObjectDrafts
        && !FPDFPage_GenerateContent(page.get())) {
        destroyPreparedObjects(preparedDrafts);
        return failure(
            PDFSaveWriter::Error::PageContentGenerationFailed,
            pageIndex);
    }
    destroyPreparedObjects(preparedDrafts);

    const PDFSaveWriter::Result replyParentResult
        = assignSourceReplyParentThreadIds(
            page.get(),
            drafts,
            pageIndex);
    if (!replyParentResult.succeeded()) {
        return replyParentResult;
    }

    QHash<QString, QString> statusParentAnnotationIds;
    QHash<QString, QString> markedParentAnnotationIds;
    for (const SaveDraft &draft : drafts) {
        if (draft.deleted || draft.added) {
            continue;
        }
        if (draft.commentStatus != draft.originalCommentStatus) {
            const QString parentName = uniqueSourceStateAnnotationName(
                page.get(),
                draft.commentStatusParentAnnotationIndex,
                QStringLiteral("Review"),
                draft.commentStateAuthor);
            if (!parentName.isEmpty()) {
                statusParentAnnotationIds.insert(
                    draft.objectId,
                    parentName);
            }
        }
        if (draft.commentMarked != draft.originalCommentMarked) {
            const QString parentName = uniqueSourceStateAnnotationName(
                page.get(),
                draft.commentMarkedParentAnnotationIndex,
                QStringLiteral("Marked"),
                draft.commentStateAuthor);
            if (!parentName.isEmpty()) {
                markedParentAnnotationIds.insert(
                    draft.objectId,
                    parentName);
            }
        }
    }

    for (const SaveDraft &draft : drafts) {
        if ((draft.kind != SaveDraft::Kind::TextMarkup
             && draft.kind != SaveDraft::Kind::StickyNote
             && draft.kind != SaveDraft::Kind::FreeText
             && draft.kind != SaveDraft::Kind::Stamp
             && draft.kind != SaveDraft::Kind::DrawingComment)
            || draft.added
            || draft.deleted) {
            continue;
        }
        const PDFSaveWriter::Result annotationUpdateResult
            = updateSourceCommentAnnotation(
                page.get(),
                draft,
                pageIndex,
                pageWidth,
                pageHeight,
                coordinateMapper,
                sourceDrawingGeometryUpdates,
                sourceCommentGeometryUpdates);
        if (!annotationUpdateResult.succeeded()) {
            return annotationUpdateResult;
        }
    }

    const PDFSaveWriter::Result annotationRemovalResult
        = removeDeletedCommentAnnotations(
            page.get(),
            drafts,
            pageIndex);
    if (!annotationRemovalResult.succeeded()) {
        return annotationRemovalResult;
    }

    for (const SaveDraft &draft : drafts) {
        if (draft.deleted || !draft.added) {
            continue;
        }
        PDFSaveWriter::Result annotationResult;
        if (draft.kind == SaveDraft::Kind::TextMarkup) {
            annotationResult = createTextMarkupAnnotation(
                page.get(),
                draft,
                pageWidth,
                pageHeight,
                coordinateMapper);
        } else if (draft.kind == SaveDraft::Kind::StickyNote) {
            annotationResult = createStickyNoteAnnotation(
                page.get(),
                draft,
                pageWidth,
                pageHeight,
                coordinateMapper);
        } else if (draft.kind == SaveDraft::Kind::FreeText) {
            annotationResult = createFreeTextAnnotation(
                page.get(),
                draft,
                pageWidth,
                pageHeight,
                coordinateMapper);
        } else if (draft.kind == SaveDraft::Kind::Stamp) {
            PDFAcroFormCanonicalizer::StampAnnotation appearanceRequest;
            annotationResult = createStampAnnotation(
                page.get(),
                draft,
                pageWidth,
                pageHeight,
                coordinateMapper,
                appearanceRequest);
            if (annotationResult.succeeded()) {
                stampAnnotations.append(std::move(appearanceRequest));
            }
        } else if (draft.kind == SaveDraft::Kind::DrawingComment) {
            PDFCommentAnnotation::DrawingAnnotation appearanceRequest;
            annotationResult = prepareDrawingCommentAnnotation(
                page.get(),
                draft,
                pageWidth,
                pageHeight,
                coordinateMapper,
                true,
                appearanceRequest);
            if (annotationResult.succeeded()) {
                drawingAnnotations.append(std::move(appearanceRequest));
            }
        } else {
            continue;
        }
        if (!annotationResult.succeeded()) {
            return annotationResult;
        }
    }

    for (const SaveDraft &draft : drafts) {
        if (draft.deleted) {
            continue;
        }
        if (draft.commentStatus != draft.originalCommentStatus) {
            const QString status = commentStatusPdfName(
                draft.commentStatus);
            const PDFSaveWriter::Result stateResult
                = createCommentStateAnnotation(
                    page.get(),
                    draft,
                    status,
                    QStringLiteral("Review"),
                    statusParentAnnotationIds.value(draft.objectId),
                    annotationRelationships);
            if (!stateResult.succeeded()) {
                return stateResult;
            }
        }
        if (draft.commentMarked != draft.originalCommentMarked) {
            const PDFSaveWriter::Result stateResult
                = createCommentStateAnnotation(
                    page.get(),
                    draft,
                    draft.commentMarked
                        ? QStringLiteral("Marked")
                        : QStringLiteral("Unmarked"),
                    QStringLiteral("Marked"),
                    markedParentAnnotationIds.value(draft.objectId),
                    annotationRelationships);
            if (!stateResult.succeeded()) {
                return stateResult;
            }
        }
    }
    return {};
}

PDFSaveWriter::Result writeDocument(FPDF_DOCUMENT document,
                                    const QString &targetFilePath,
                                    bool removeSecurity = false)
{
    assertPDFiumProcessLockHeld();
    SaveFileWriter writer(targetFilePath);
    if (!writer.open()) {
        return failure(
            PDFSaveWriter::Error::OutputOpenFailed,
            -1,
            writer.errorString());
    }
    // PDFium's subsetter drops shaping/kerning tables. Embedded faces are the
    // first choice after reopen, so retain these bounded programs to avoid
    // changing advances on a later color/format edit.
    const FPDF_DWORD saveFlags = FPDF_NO_INCREMENTAL
        | (removeSecurity ? FPDF_REMOVE_SECURITY : 0);
    if (!FPDF_SaveAsCopy(document, &writer, saveFlags)) {
        return failure(
            PDFSaveWriter::Error::OutputWriteFailed,
            -1,
            writer.errorString());
    }
    if (!writer.commit()) {
        return failure(
            PDFSaveWriter::Error::OutputCommitFailed,
            -1,
            writer.errorString());
    }
    return {};
}

PDFSaveWriter::Result commitDocumentFile(const QString &sourceFilePath,
                                         const QString &targetFilePath)
{
    QFile source(sourceFilePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return failure(
            PDFSaveWriter::Error::OutputOpenFailed,
            -1,
            source.errorString());
    }

    PDFAtomicSaveFile target(targetFilePath);
    if (!target.open(QIODevice::WriteOnly)) {
        return failure(
            PDFSaveWriter::Error::OutputOpenFailed,
            -1,
            target.errorString());
    }

    constexpr qint64 chunkSize = 1024 * 1024;
    QByteArray buffer;
    buffer.resize(chunkSize);
    while (true) {
        const qint64 readCount = source.read(buffer.data(), buffer.size());
        if (readCount < 0) {
            return failure(
                PDFSaveWriter::Error::OutputWriteFailed,
                -1,
                source.errorString());
        }
        if (readCount == 0) {
            break;
        }
        if (target.write(buffer.constData(), readCount) != readCount) {
            return failure(
                PDFSaveWriter::Error::OutputWriteFailed,
                -1,
                target.errorString());
        }
    }
    if (!target.commit()) {
        return failure(
            PDFSaveWriter::Error::OutputCommitFailed,
            -1,
            target.errorString());
    }
    return {};
}

PDFSaveWriter::Result protectAndCommitDocumentFile(
    const QString &sourceFilePath,
    const QString &targetFilePath,
    const QByteArray &openPassword,
    int expectedPageCount,
    const QByteArray &sourcePassword = {},
    const QList<QByteArray> &openTypeFonts = {})
{
    assertPDFiumProcessLockHeld();
    // PDFium's Type1 loader accepts CFF but writes the wrong stream key. Fix
    // only this save's exact font programs, after all PDFium serialization, so
    // form persistence cannot undo the correction. Keep the target untouched.
    if (!openTypeFonts.isEmpty()) {
        QTemporaryDir corrected(QFileInfo(targetFilePath).absoluteDir().filePath(
            QStringLiteral(".trepdf-opentype-XXXXXX")));
        if (!corrected.isValid())
            return failure(PDFSaveWriter::Error::OutputOpenFailed, -1, corrected.errorString());
        const QString correctedPath = corrected.filePath(QStringLiteral("corrected.pdf"));
        const QByteArray stagingPassword = openPassword.isEmpty() ? sourcePassword : QByteArray();
        if (!PDFOpenTypeFontWriter::finalize(sourceFilePath, correctedPath,
                                            openTypeFonts, stagingPassword))
            return failure(PDFSaveWriter::Error::OutputWriteFailed);
        PdfDocumentHandle validation = PDFiumDocument::open(correctedPath, stagingPassword);
        if (!validation.get() || FPDF_GetPageCount(validation.get()) != expectedPageCount)
            return failure(PDFSaveWriter::Error::OutputWriteFailed);
        return protectAndCommitDocumentFile(correctedPath, targetFilePath, openPassword,
                                             expectedPageCount, sourcePassword);
    }
    if (openPassword.isEmpty()) {
        return commitDocumentFile(sourceFilePath, targetFilePath);
    }

    // All PDFium and canonicalizer changes are complete before encryption.
    // Keep both stages private until validation and the final atomic commit.
    QTemporaryDir encryptedFile(QFileInfo(targetFilePath).absoluteDir().filePath(
        QStringLiteral(".trepdf-protected-XXXXXX")));
    if (!encryptedFile.isValid()) {
        return failure(PDFSaveWriter::Error::OutputOpenFailed,
                       -1, encryptedFile.errorString());
    }
    const QString encryptedFilePath = encryptedFile.filePath(QStringLiteral("protected.pdf"));
    if (!PDFPasswordWriter::encryptFile(sourceFilePath, encryptedFilePath,
                                        openPassword, sourcePassword)) {
        return failure(PDFSaveWriter::Error::PasswordProtectionFailed);
    }

    {
        PdfDocumentHandle protectedDocument = PDFiumDocument::open(encryptedFilePath, openPassword);
        if (!protectedDocument.get()
            || FPDF_GetSecurityHandlerRevision(protectedDocument.get()) < 0
            || FPDF_GetPageCount(protectedDocument.get()) != expectedPageCount) {
            return failure(PDFSaveWriter::Error::PasswordProtectionFailed);
        }
        PdfDocumentHandle unprotectedDocument = PDFiumDocument::open(encryptedFilePath);
        if (unprotectedDocument.get() || unprotectedDocument.error() != FPDF_ERR_PASSWORD) {
            return failure(PDFSaveWriter::Error::PasswordProtectionFailed);
        }
        if (!sourcePassword.isEmpty() && sourcePassword != openPassword) {
            PdfDocumentHandle oldPasswordDocument = PDFiumDocument::open(encryptedFilePath, sourcePassword);
            if (oldPasswordDocument.get()
                || oldPasswordDocument.error() != FPDF_ERR_PASSWORD) {
                return failure(PDFSaveWriter::Error::PasswordProtectionFailed);
            }
        }
    }
    return commitDocumentFile(encryptedFilePath, targetFilePath);
}

PDFSaveWriter::Result validateAnnotationRelationships(
    const QString &filePath,
    const QVector<PDFAcroFormCanonicalizer::AnnotationRelationship>
        &relationships,
    const QByteArray &password)
{
    assertPDFiumProcessLockHeld();
    if (relationships.isEmpty()) {
        return {};
    }

    PdfDocumentHandle document = PDFiumDocument::open(filePath, password);
    if (!document.get()) {
        return failure(PDFSaveWriter::Error::CommentAnnotationCreationFailed);
    }
    const int pageCount = FPDF_GetPageCount(document.get());
    for (const PDFAcroFormCanonicalizer::AnnotationRelationship &relationship
         : relationships) {
        if (relationship.page < 0 || relationship.page >= pageCount) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                relationship.page,
                relationship.childAnnotationId);
        }
        PdfPageHandle page(FPDF_LoadPage(
            document.get(),
            relationship.page));
        if (!page.get()) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                relationship.page,
                relationship.childAnnotationId);
        }

        int parentIndex = -1;
        int childIndex = -1;
        const bool parentByThreadId
            = !relationship.parentThreadId.isEmpty();
        const int annotationCount = FPDFPage_GetAnnotCount(page.get());
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            PdfAnnotationHandle annotation(FPDFPage_GetAnnot(
                page.get(),
                annotationIndex));
            if (!annotation.get()) {
                continue;
            }
            const QString parentIdentifier = annotationString(
                annotation.get(),
                parentByThreadId ? kCommentThreadIdKey : "NM");
            const QString expectedParentIdentifier = parentByThreadId
                ? relationship.parentThreadId
                : relationship.parentAnnotationId;
            if (parentIdentifier == expectedParentIdentifier) {
                if (parentIndex >= 0) {
                    return failure(
                        PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                        relationship.page,
                        relationship.childAnnotationId);
                }
                parentIndex = annotationIndex;
            }
            if (annotationString(annotation.get(), "NM")
                == relationship.childAnnotationId) {
                if (childIndex >= 0) {
                    return failure(
                        PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                        relationship.page,
                        relationship.childAnnotationId);
                }
                childIndex = annotationIndex;
            }
        }
        if (parentIndex < 0 || childIndex < 0) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                relationship.page,
                relationship.childAnnotationId);
        }

        PdfAnnotationHandle childAnnotation(FPDFPage_GetAnnot(
            page.get(),
            childIndex));
        PdfAnnotationHandle linkedParent(childAnnotation.get()
                ? FPDFAnnot_GetLinkedAnnot(childAnnotation.get(), "IRT")
                : nullptr);
        const int linkedParentIndex = linkedParent.get()
            ? FPDFPage_GetAnnotIndex(page.get(), linkedParent.get())
            : -1;
        const QString relation = childAnnotation.get()
            ? annotationString(childAnnotation.get(), "RT").trimmed()
            : QString();
        bool valid = childAnnotation.get()
            && FPDFAnnot_GetSubtype(childAnnotation.get()) == FPDF_ANNOT_TEXT
            && linkedParentIndex == parentIndex;
        if (valid
            && relationship.kind
                == PDFAcroFormCanonicalizer::
                    AnnotationRelationshipKind::Reply) {
            valid = relation.compare(
                        QLatin1String("R"),
                        Qt::CaseInsensitive)
                == 0;
        } else if (valid) {
            valid = relation.isEmpty()
                && annotationString(childAnnotation.get(), "State").trimmed()
                    == relationship.expectedState
                && annotationString(
                       childAnnotation.get(),
                       "StateModel").trimmed()
                    == relationship.expectedStateModel;
        }
        if (!valid) {
            return failure(
                PDFSaveWriter::Error::CommentAnnotationCreationFailed,
                relationship.page,
                relationship.childAnnotationId);
        }
    }
    return {};
}

PDFSaveWriter::Result prepareAcroFormFields(
    FPDF_DOCUMENT document,
    const QVector<PDFFormDesignerController::SaveField> &sourceFields,
    QVector<PDFAcroFormWriter::Field> &targetFields)
{
    assertPDFiumProcessLockHeld();
    targetFields.clear();
    targetFields.reserve(sourceFields.size());
    const int pageCount = FPDF_GetPageCount(document);
    for (const PDFFormDesignerController::SaveField &source : sourceFields) {
        if (source.operation
            == PDFFormDesignerController::SaveField::Remove) {
            if ((source.type != PDFFormDesignerController::Text
                 && source.type != PDFFormDesignerController::Dropdown
                 && source.type != PDFFormDesignerController::ListBox
                 && source.type != PDFFormDesignerController::RadioButton)
                || source.id.isEmpty()
                || source.page < -1
                || source.page >= pageCount) {
                return failure(
                    PDFSaveWriter::Error::AcroFormCreationFailed,
                    source.page,
                    QStringLiteral(
                        "The removed form field snapshot is invalid."));
            }
            PDFAcroFormWriter::Field target;
            target.widgetId = source.id;
            target.fieldName = source.name;
            target.type = source.type == PDFFormDesignerController::Text
                ? PDFAcroFormWriter::FieldType::Text
                : source.type == PDFFormDesignerController::RadioButton
                    ? PDFAcroFormWriter::FieldType::RadioButton
                    : source.type == PDFFormDesignerController::ListBox
                        ? PDFAcroFormWriter::FieldType::ListBox
                        : PDFAcroFormWriter::FieldType::Dropdown;
            target.operation = PDFAcroFormWriter::Operation::Remove;
            target.page = source.page;
            targetFields.append(std::move(target));
            continue;
        }
        if (source.page < 0
            || source.page >= pageCount
            || source.name.isEmpty()
            || source.normalizedRect.isEmpty()) {
            return failure(
                PDFSaveWriter::Error::AcroFormCreationFailed,
                source.page,
                QStringLiteral("The form field snapshot is invalid."));
        }

        PdfPageHandle page(FPDF_LoadPage(document, source.page));
        if (!page.get()) {
            return failure(
                PDFSaveWriter::Error::PageLoadFailed,
                source.page);
        }
        const qreal pageWidth = FPDF_GetPageWidthF(page.get());
        const qreal pageHeight = FPDF_GetPageHeightF(page.get());
        if (pageWidth <= 0.0 || pageHeight <= 0.0) {
            return failure(
                PDFSaveWriter::Error::AcroFormCreationFailed,
                source.page,
                QStringLiteral("The form field page has invalid dimensions."));
        }

        const QRectF deviceRect(
            source.normalizedRect.x() * pageWidth,
            source.normalizedRect.y() * pageHeight,
            source.normalizedRect.width() * pageWidth,
            source.normalizedRect.height() * pageHeight);
        const PageCoordinateMapper mapper(page.get(), pageWidth, pageHeight);
        std::array<QPointF, 4> pagePoints;
        if (!mapper.pagePoint(deviceRect.left(), deviceRect.top(), pagePoints[0])
            || !mapper.pagePoint(deviceRect.right(), deviceRect.top(), pagePoints[1])
            || !mapper.pagePoint(deviceRect.right(), deviceRect.bottom(), pagePoints[2])
            || !mapper.pagePoint(deviceRect.left(), deviceRect.bottom(), pagePoints[3])) {
            return failure(
                PDFSaveWriter::Error::AcroFormCreationFailed,
                source.page,
                QStringLiteral("The form field coordinates could not be mapped."));
        }

        qreal left = pagePoints[0].x();
        qreal right = left;
        qreal top = pagePoints[0].y();
        qreal bottom = top;
        for (const QPointF &point : pagePoints) {
            left = qMin(left, point.x());
            right = qMax(right, point.x());
            bottom = qMin(bottom, point.y());
            top = qMax(top, point.y());
        }
        const QRectF pageRect(left, bottom, right - left, top - bottom);
        if (pageRect.isEmpty()) {
            return failure(
                PDFSaveWriter::Error::AcroFormCreationFailed,
                source.page,
                QStringLiteral("The mapped form field rectangle is empty."));
        }

        PDFAcroFormWriter::Field target;
        target.widgetId = source.id;
        target.fieldName = source.name;
        target.tooltip = source.tooltip;
        target.value = source.value;
        target.selectedValues = source.selectedValues;
        target.defaultValue = source.defaultValue;
        target.options = source.options;
        if (source.operation
            == PDFFormDesignerController::SaveField::Add) {
            target.operation = PDFAcroFormWriter::Operation::Add;
        } else if (source.changedProperties
                   == PDFFormDesignerController::NoPropertyChanges) {
            target.operation
                = PDFAcroFormWriter::Operation::NormalizeAppearance;
        } else {
            target.operation = PDFAcroFormWriter::Operation::Update;
        }
        target.page = source.page;
        target.pageRect = pageRect;
        target.required = source.flags
            & PDFFormDesignerController::Required;
        target.readOnly = source.flags
            & PDFFormDesignerController::ReadOnly;
        target.multiline = source.flags
            & PDFFormDesignerController::Multiline;
        target.comb = source.flags
            & PDFFormDesignerController::Comb;
        target.maxLength = source.maxLength;
        target.valueChanged
            = source.operation
                    == PDFFormDesignerController::SaveField::Add
            || (source.changedProperties
                & PDFFormDesignerController::ValuePropertyChanged);
        target.optionsChanged
            = source.operation
                    == PDFFormDesignerController::SaveField::Add
            || (source.changedProperties
                & PDFFormDesignerController::OptionsPropertyChanged);
        switch (source.type) {
        case PDFFormDesignerController::Text:
            target.type = PDFAcroFormWriter::FieldType::Text;
            break;
        case PDFFormDesignerController::Checkbox:
            target.type = PDFAcroFormWriter::FieldType::Checkbox;
            break;
        case PDFFormDesignerController::RadioButton:
            target.type = PDFAcroFormWriter::FieldType::RadioButton;
            break;
        case PDFFormDesignerController::Dropdown:
            target.type = PDFAcroFormWriter::FieldType::Dropdown;
            break;
        case PDFFormDesignerController::Date:
            target.type = PDFAcroFormWriter::FieldType::Date;
            break;
        case PDFFormDesignerController::Signature:
            target.type = PDFAcroFormWriter::FieldType::Signature;
            break;
        case PDFFormDesignerController::ListBox:
            target.type = PDFAcroFormWriter::FieldType::ListBox;
            break;
        }
        targetFields.append(std::move(target));
    }
    return {};
}

using WebLinkRemovals = QMap<int, QHash<QByteArray, int>>;

PDFSaveWriter::Result validateWebLinkRemovals(
    FPDF_DOCUMENT inspectionDocument,
    int sourcePageCount,
    const QVector<int> &deletedSourcePages,
    const QVector<PDFPageInsertion> &pageInsertions,
    const QVector<PDFWebLinks::SourceLink> &links,
    WebLinkRemovals &removals)
{
    assertPDFiumProcessLockHeld();
    using Error = PDFSaveWriter::Error;
    if (links.size() > PDFWebLinks::MaximumLinks) {
        return failure(Error::WebLinkRemovalFailed);
    }
    QMap<int, QVector<const PDFWebLinks::SourceLink *>> linksBySourcePage;
    for (const PDFWebLinks::SourceLink &link : links) {
        if (link.sourcePage < 0 || link.sourcePage >= sourcePageCount
            || link.annotationIndex < 0 || link.fingerprint.size() != 32) {
            return failure(Error::WebLinkRemovalFailed, link.sourcePage);
        }
        if (!std::binary_search(deletedSourcePages.cbegin(),
                                deletedSourcePages.cend(), link.sourcePage)) {
            linksBySourcePage[link.sourcePage].append(&link);
        }
    }
    for (auto entry = linksBySourcePage.cbegin(); entry != linksBySourcePage.cend(); ++entry) {
        const int sourcePage = entry.key();
        PdfPageHandle page(FPDF_LoadPage(inspectionDocument, sourcePage));
        if (!page.get()) {
            return failure(Error::PageLoadFailed, sourcePage);
        }
        const PDFWebLinks::SourceScanResult inspected = PDFWebLinks::sourceLinksForPage(
            inspectionDocument, page.get(), sourcePage);
        if (inspected.failed) {
            return failure(Error::WebLinkRemovalFailed, sourcePage);
        }
        QHash<int, QByteArray> sourceIdentities;
        QHash<QByteArray, int> sourceCounts;
        for (const PDFWebLinks::SourceLink &link : inspected.links) {
            sourceIdentities.insert(link.annotationIndex, link.fingerprint);
            ++sourceCounts[link.fingerprint];
        }
        QSet<int> requestedIndices;
        QHash<QByteArray, int> requestedCounts;
        for (const PDFWebLinks::SourceLink *link : entry.value()) {
            if (requestedIndices.contains(link->annotationIndex)
                || sourceIdentities.value(link->annotationIndex) != link->fingerprint) {
                return failure(Error::WebLinkRemovalFailed, sourcePage);
            }
            requestedIndices.insert(link->annotationIndex);
            ++requestedCounts[link->fingerprint];
        }
        for (auto identity = requestedCounts.cbegin(); identity != requestedCounts.cend();
             ++identity) {
            if (sourceCounts.value(identity.key()) != identity.value()) {
                return failure(Error::WebLinkRemovalFailed, sourcePage);
            }
        }
        const auto deleted = std::lower_bound(
            deletedSourcePages.cbegin(), deletedSourcePages.cend(), sourcePage);
        int outputPage = sourcePage - static_cast<int>(deleted - deletedSourcePages.cbegin());
        for (const PDFPageInsertion &insertion : pageInsertions) {
            if (insertion.page <= outputPage) {
                ++outputPage;
            }
        }
        removals.insert(outputPage, std::move(requestedCounts));
    }
    return {};
}

PDFSaveWriter::Result removeWebLinks(FPDF_DOCUMENT document,
                                    const WebLinkRemovals &removals)
{
    assertPDFiumProcessLockHeld();
    using Error = PDFSaveWriter::Error;
    for (auto entry = removals.cbegin(); entry != removals.cend(); ++entry) {
        const int pageIndex = entry.key();
        PdfPageHandle page(FPDF_LoadPage(document, pageIndex));
        if (!page.get()) {
            return failure(Error::PageLoadFailed, pageIndex);
        }
        const PDFWebLinks::SourceScanResult inspected = PDFWebLinks::sourceLinksForPage(
            document, page.get(), pageIndex);
        if (inspected.failed) {
            return failure(Error::WebLinkRemovalFailed, pageIndex);
        }
        QHash<QByteArray, int> actualCounts;
        QVector<int> indices;
        for (const PDFWebLinks::SourceLink &link : inspected.links) {
            if (entry->contains(link.fingerprint)) {
                ++actualCounts[link.fingerprint];
                indices.append(link.annotationIndex);
            }
        }
        if (actualCounts != entry.value()) {
            return failure(Error::WebLinkRemovalFailed, pageIndex);
        }
        // Source indices were validated before draft application. Comments may
        // have shifted them since then, so remove by revalidated identity in
        // descending current index order, after all source-index-based edits.
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int index : std::as_const(indices)) {
            if (!FPDFPage_RemoveAnnot(page.get(), index)) {
                return failure(Error::WebLinkRemovalFailed, pageIndex);
            }
        }
        // Annotation-only removal leaves the existing page content untouched.
    }
    return {};
}

struct ManualLinkEdit {
    PDFWebLinks::ManualLink link;
    QString markerId;
    int page = -1;
    int destinationPage = -1;
};

int outputPageForSource(int sourcePage, int sourcePageCount,
                        const QVector<int> &deletedSourcePages,
                        const QVector<PDFPageInsertion> &pageInsertions)
{
    if (sourcePage < 0 || sourcePage >= sourcePageCount)
        return -1;
    const auto deleted = std::lower_bound(
        deletedSourcePages.cbegin(), deletedSourcePages.cend(), sourcePage);
    if (deleted != deletedSourcePages.cend() && *deleted == sourcePage)
        return -1;
    int page = sourcePage - static_cast<int>(deleted - deletedSourcePages.cbegin());
    for (const PDFPageInsertion &insertion : pageInsertions) {
        if (insertion.page <= page)
            ++page;
    }
    return page;
}

PDFSaveWriter::Result prepareManualLinkEdits(
    FPDF_DOCUMENT document, FPDF_DOCUMENT inspectionDocument, int sourcePageCount,
    const QVector<int> &deletedSourcePages,
    const QVector<PDFPageInsertion> &pageInsertions,
    const QVector<PDFWebLinks::ManualLink> &links,
    const QVector<PDFWebLinks::SourceLink> &batchRemovals,
    QVector<ManualLinkEdit> &edits)
{
    using Error = PDFSaveWriter::Error;
    assertPDFiumProcessLockHeld();
    if (links.size() > PDFWebLinks::MaximumLinks)
        return failure(Error::ManualLinkUpdateFailed);
    QHash<int, QSet<int>> removed;
    for (const auto &link : batchRemovals)
        removed[link.sourcePage].insert(link.annotationIndex);
    QMap<int, QVector<const PDFWebLinks::ManualLink *>> bySourcePage;
    QSet<QString> ids;
    for (const auto &link : links) {
        if (ids.contains(link.id) || link.sourcePage < 0 || link.sourcePage >= sourcePageCount
            || link.annotationIndex < -1 || !link.editable)
            return failure(Error::ManualLinkUpdateFailed, link.sourcePage);
        ids.insert(link.id);
        if (std::binary_search(deletedSourcePages.cbegin(), deletedSourcePages.cend(), link.sourcePage)
            || (link.deleted && link.annotationIndex < 0)
            || (link.annotationIndex >= 0 && removed.value(link.sourcePage).contains(link.annotationIndex)))
            continue;
        if (!PDFWebLinks::validManualLink(link))
            return failure(Error::ManualLinkUpdateFailed, link.sourcePage);
        bySourcePage[link.sourcePage].append(&link);
    }
    for (auto entry = bySourcePage.cbegin(); entry != bySourcePage.cend(); ++entry) {
        const int sourcePage = entry.key();
        PdfPageHandle inspectionPage(FPDF_LoadPage(inspectionDocument, sourcePage));
        PdfPageHandle page(FPDF_LoadPage(document, sourcePage));
        if (!inspectionPage.get() || !page.get())
            return failure(Error::PageLoadFailed, sourcePage);
        const auto inspected = PDFWebLinks::editableLinksForPage(
            inspectionDocument, inspectionPage.get(), sourcePage);
        if (inspected.failed)
            return failure(Error::ManualLinkUpdateFailed, sourcePage);
        QHash<int, PDFWebLinks::ManualLink> originals;
        for (const auto &original : inspected.links)
            originals.insert(original.annotationIndex, original);
        QSet<int> sourceIndices;
        for (const auto *link : entry.value()) {
            ManualLinkEdit edit;
            edit.link = *link;
            edit.page = outputPageForSource(sourcePage, sourcePageCount, deletedSourcePages, pageInsertions);
            if (!link->deleted && link->targetSourcePage >= 0) {
                edit.destinationPage = outputPageForSource(
                    link->targetSourcePage, sourcePageCount, deletedSourcePages, pageInsertions);
                if (edit.destinationPage < 0)
                    return failure(Error::ManualLinkUpdateFailed, edit.page);
            }
            edit.markerId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (link->annotationIndex >= 0) {
                const auto original = originals.constFind(link->annotationIndex);
                if (sourceIndices.contains(link->annotationIndex) || original == originals.cend()
                    || !original->editable || link->fingerprint.size() != 32
                    || original->fingerprint != link->fingerprint)
                    return failure(Error::ManualLinkUpdateFailed, sourcePage);
                sourceIndices.insert(link->annotationIndex);
                PdfAnnotationHandle annotation(FPDFPage_GetAnnot(page.get(), link->annotationIndex));
                if (!annotation.get()
                    || FPDFAnnot_HasKey(annotation.get(), PDFWebLinks::ManualMarkerKey)
                    || !FPDFAnnot_SetStringValue(annotation.get(), PDFWebLinks::ManualMarkerKey,
                            reinterpret_cast<const FPDF_WCHAR *>(edit.markerId.utf16())))
                    return failure(Error::ManualLinkUpdateFailed, sourcePage);
            }
            edits.append(std::move(edit));
        }
    }
    return {};
}

PDFSaveWriter::Result applyManualLinkEdits(
    FPDF_DOCUMENT document, const QVector<ManualLinkEdit> &edits,
    QVector<PDFAcroFormCanonicalizer::LinkAnnotation> &annotations)
{
    using Error = PDFSaveWriter::Error;
    assertPDFiumProcessLockHeld();
    QMap<int, QVector<const ManualLinkEdit *>> byPage;
    for (const auto &edit : edits)
        byPage[edit.page].append(&edit);
    for (auto entry = byPage.cbegin(); entry != byPage.cend(); ++entry) {
        const int pageIndex = entry.key();
        PdfPageHandle page(FPDF_LoadPage(document, pageIndex));
        if (!page.get())
            return failure(Error::PageLoadFailed, pageIndex);
        const qreal width = FPDF_GetPageWidthF(page.get());
        const qreal height = FPDF_GetPageHeightF(page.get());
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
            || width > 1e6 || height > 1e6)
            return failure(Error::ManualLinkUpdateFailed, pageIndex);
        const PageCoordinateMapper mapper(page.get(), width, height);
        QHash<QString, int> markers;
        const int count = FPDFPage_GetAnnotCount(page.get());
        if (count < 0 || count > PDFWebLinks::MaximumAnnotationsPerPage + PDFWebLinks::MaximumLinks)
            return failure(Error::ManualLinkUpdateFailed, pageIndex);
        for (int index = 0; index < count; ++index) {
            PdfAnnotationHandle annotation(FPDFPage_GetAnnot(page.get(), index));
            if (!annotation.get())
                return failure(Error::ManualLinkUpdateFailed, pageIndex);
            if (!FPDFAnnot_HasKey(annotation.get(), PDFWebLinks::ManualMarkerKey))
                continue;
            const QString marker = PDFAnnotationUtils::annotationString(
                annotation.get(), PDFWebLinks::ManualMarkerKey);
            if (marker.isEmpty() || markers.contains(marker)
                || FPDFAnnot_GetSubtype(annotation.get()) != FPDF_ANNOT_LINK)
                return failure(Error::ManualLinkUpdateFailed, pageIndex);
            markers.insert(marker, index);
        }
        QVector<int> deletions;
        for (const ManualLinkEdit *edit : entry.value()) {
            if (edit->link.annotationIndex >= 0 && !markers.contains(edit->markerId))
                return failure(Error::ManualLinkUpdateFailed, pageIndex);
            if (edit->link.deleted) {
                deletions.append(markers.value(edit->markerId));
                continue;
            }
            FS_RECTF rect{};
            if (!annotationRectForFrame(edit->link.rect, width, height, mapper, rect))
                return failure(Error::ManualLinkUpdateFailed, pageIndex);
            if (edit->link.annotationIndex < 0) {
                PdfAnnotationHandle annotation(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINK));
                if (!annotation.get() || !FPDFAnnot_SetRect(annotation.get(), &rect)
                    || !FPDFAnnot_SetBorder(annotation.get(), 0.0F, 0.0F, 0.0F)
                    || !FPDFAnnot_SetStringValue(annotation.get(), PDFWebLinks::ManualMarkerKey,
                            reinterpret_cast<const FPDF_WCHAR *>(edit->markerId.utf16())))
                    return failure(Error::ManualLinkUpdateFailed, pageIndex);
            }
            PDFAcroFormCanonicalizer::LinkAnnotation annotation;
            annotation.markerId = edit->markerId;
            annotation.page = pageIndex;
            annotation.pageRect = QRectF(QPointF(rect.left, rect.bottom), QPointF(rect.right, rect.top));
            annotation.pageQuarterTurns = FPDFPage_GetRotation(page.get());
            annotation.destinationPage = edit->destinationPage;
            annotation.link = edit->link;
            annotations.append(std::move(annotation));
        }
        std::sort(deletions.begin(), deletions.end(), std::greater<int>());
        for (const int index : deletions) {
            if (!FPDFPage_RemoveAnnot(page.get(), index))
                return failure(Error::ManualLinkUpdateFailed, pageIndex);
        }
    }
    return {};
}

PDFSaveWriter::Result addWebLinks(
    FPDF_DOCUMENT document,
    int sourcePageCount,
    const QVector<int> &deletedSourcePages,
    const QVector<PDFPageInsertion> &pageInsertions,
    const QVector<PDFWebLinks::Link> &links)
{
    assertPDFiumProcessLockHeld();
    using Error = PDFSaveWriter::Error;
    if (links.size() > PDFWebLinks::MaximumLinks) {
        return failure(Error::WebLinkCreationFailed);
    }

    QMap<int, QVector<const PDFWebLinks::Link *>> linksByPage;
    qsizetype rectangleCount = 0;
    for (const PDFWebLinks::Link &link : links) {
        if (link.sourcePage < 0 || link.sourcePage >= sourcePageCount) {
            return failure(Error::WebLinkCreationFailed, link.sourcePage);
        }
        const auto deleted = std::lower_bound(
            deletedSourcePages.cbegin(), deletedSourcePages.cend(), link.sourcePage);
        if (deleted != deletedSourcePages.cend() && *deleted == link.sourcePage) {
            continue;
        }
        int page = link.sourcePage
            - static_cast<int>(deleted - deletedSourcePages.cbegin());
        for (const PDFPageInsertion &insertion : pageInsertions) {
            if (insertion.page <= page) {
                ++page;
            }
        }
        if (link.rects.isEmpty()
            || link.rects.size() > PDFWebLinks::MaximumRectanglesPerLink
            || link.rects.size() > PDFWebLinks::MaximumRectangles - rectangleCount
            || PDFWebLinks::validWebUrl(link.url).isEmpty()) {
            return failure(Error::WebLinkCreationFailed, page);
        }
        rectangleCount += link.rects.size();
        linksByPage[page].append(&link);
    }

    // Keep duplicate/overlap checks bounded even in annotation-heavy documents.
    constexpr qsizetype maximumOverlapComparisons = 16 * 1024 * 1024;
    qsizetype overlapComparisons = 0;
    for (auto entry = linksByPage.cbegin(); entry != linksByPage.cend(); ++entry) {
        const int pageIndex = entry.key();
        PdfPageHandle page(FPDF_LoadPage(document, pageIndex));
        if (!page.get()) {
            return failure(Error::PageLoadFailed, pageIndex);
        }
        const qreal pageWidth = FPDF_GetPageWidthF(page.get());
        const qreal pageHeight = FPDF_GetPageHeightF(page.get());
        // PageCoordinateMapper uses integer device coordinates at a 64x scale.
        if (!std::isfinite(pageWidth) || !std::isfinite(pageHeight)
            || pageWidth <= 0.0 || pageHeight <= 0.0
            || pageWidth > 1000000.0 || pageHeight > 1000000.0) {
            return failure(Error::WebLinkCreationFailed, pageIndex);
        }
        const PageCoordinateMapper mapper(page.get(), pageWidth, pageHeight);
        QVector<QRectF> occupiedRects;
        const int annotationCount = FPDFPage_GetAnnotCount(page.get());
        if (annotationCount < 0
            || annotationCount > PDFWebLinks::MaximumAnnotationsPerPage) {
            return failure(Error::WebLinkCreationFailed, pageIndex);
        }
        const auto appendOccupied = [&](const QRectF &rect) {
            if (!std::isfinite(rect.x()) || !std::isfinite(rect.y())
                || !std::isfinite(rect.width()) || !std::isfinite(rect.height())
                || occupiedRects.size() >= PDFWebLinks::MaximumExistingRectanglesPerPage) {
                return false;
            }
            if (!rect.isEmpty()) {
                occupiedRects.append(rect);
            }
            return true;
        };
        for (int index = 0; index < annotationCount; ++index) {
            PdfAnnotationHandle annotation(FPDFPage_GetAnnot(page.get(), index));
            if (!annotation.get()) {
                return failure(Error::WebLinkCreationFailed, pageIndex);
            }
            if (FPDFAnnot_GetSubtype(annotation.get()) != FPDF_ANNOT_LINK) {
                continue;
            }
            FS_RECTF rect{};
            if (!FPDFAnnot_GetRect(annotation.get(), &rect)
                || !appendOccupied(QRectF(
                    QPointF(rect.left, rect.bottom),
                    QPointF(rect.right, rect.top)).normalized())) {
                return failure(Error::WebLinkCreationFailed, pageIndex);
            }
            const FPDF_LINK existingLink = FPDFAnnot_GetLink(annotation.get());
            const int quadCount = FPDFLink_CountQuadPoints(existingLink);
            if (!existingLink || quadCount < 0
                || quadCount > PDFWebLinks::MaximumExistingRectanglesPerPage
                    - occupiedRects.size()) {
                return failure(Error::WebLinkCreationFailed, pageIndex);
            }
            for (int quadIndex = 0; quadIndex < quadCount; ++quadIndex) {
                FS_QUADPOINTSF quad{};
                if (!FPDFLink_GetQuadPoints(existingLink, quadIndex, &quad)) {
                    return failure(Error::WebLinkCreationFailed, pageIndex);
                }
                const std::array<QPointF, 4> points{
                    QPointF(quad.x1, quad.y1), QPointF(quad.x2, quad.y2),
                    QPointF(quad.x3, quad.y3), QPointF(quad.x4, quad.y4)};
                qreal left = points.front().x();
                qreal right = left;
                qreal bottom = points.front().y();
                qreal top = bottom;
                for (const QPointF &point : points) {
                    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                        return failure(Error::WebLinkCreationFailed, pageIndex);
                    }
                    left = qMin(left, point.x());
                    right = qMax(right, point.x());
                    bottom = qMin(bottom, point.y());
                    top = qMax(top, point.y());
                }
                if (!appendOccupied(QRectF(left, bottom, right - left, top - bottom))) {
                    return failure(Error::WebLinkCreationFailed, pageIndex);
                }
            }
        }

        qsizetype pageRectangleCount = 0;
        for (const PDFWebLinks::Link *link : entry.value()) {
            const QByteArray uri = PDFWebLinks::validWebUrl(link->url).toLatin1();
            for (const QRectF &rect : link->rects) {
                if (++pageRectangleCount > PDFWebLinks::MaximumRectanglesPerPage
                    || !std::isfinite(rect.x()) || !std::isfinite(rect.y())
                    || !std::isfinite(rect.width()) || !std::isfinite(rect.height())
                    || rect.isEmpty() || rect.left() < 0.0 || rect.top() < 0.0
                    || rect.right() > 1.0 || rect.bottom() > 1.0) {
                    return failure(Error::WebLinkCreationFailed, pageIndex);
                }
                FS_RECTF annotationRect{};
                if (!annotationRectForFrame(
                        rect, pageWidth, pageHeight, mapper, annotationRect)) {
                    return failure(Error::WebLinkCreationFailed, pageIndex);
                }
                const QRectF pdfRect(
                    QPointF(annotationRect.left, annotationRect.bottom),
                    QPointF(annotationRect.right, annotationRect.top));
                bool overlapsExisting = false;
                for (const QRectF &occupied : std::as_const(occupiedRects)) {
                    if (++overlapComparisons > maximumOverlapComparisons) {
                        return failure(Error::WebLinkCreationFailed, pageIndex);
                    }
                    if (occupied.intersects(pdfRect)) {
                        overlapsExisting = true;
                        break;
                    }
                }
                if (overlapsExisting) {
                    continue;
                }

                // Each line of a wrapped URL is its own invisible hit target;
                // a bounding union would turn unrelated intervening text into a link.
                PdfAnnotationHandle annotation(
                    FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_LINK));
                if (!annotation.get()
                    || !FPDFAnnot_SetRect(annotation.get(), &annotationRect)
                    || !FPDFAnnot_SetBorder(annotation.get(), 0.0f, 0.0f, 0.0f)
                    || !FPDFAnnot_SetURI(annotation.get(), uri.constData())) {
                    return failure(Error::WebLinkCreationFailed, pageIndex);
                }
                occupiedRects.append(pdfRect);
            }
        }
        // Link annotations do not modify page content streams, so this path
        // deliberately never calls FPDFPage_GenerateContent().
    }
    return {};
}

ApplicationFonts::FontFaceData snapshotUnicodeFormFont()
{
    // Called by prepare() on the GUI thread. The worker receives font bytes and
    // a registered layout family, never a path or a platform font lookup.
    QStringList families = ApplicationFonts::downloadedFontFamilies();
    families.prepend(QFont().family());
    families.append({QStringLiteral("Arial"), QStringLiteral("Liberation Sans"),
                     QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
    families.append(ApplicationFonts::systemFontFamilies());
    families.removeDuplicates();
    const auto suitable = [](const ApplicationFonts::FontFaceData &face) {
        if (!face.isValid() || face.data.size() > 64 * 1024 * 1024)
            return false;
        const QRawFont font(face.data, 16.0, QFont::PreferNoHinting);
        if (!font.isValid() || font.fontTable("glyf").isEmpty()
            || font.fontTable("loca").isEmpty()
            || !font.supportsCharacter(0x1ecdU))
            return false;
        for (uint character = 0x20; character < 0x7f; ++character) {
            if (!font.supportsCharacter(character))
                return false;
        }
        return true;
    };
    for (const QString &family : std::as_const(families)) {
        auto face = ApplicationFonts::downloadedFontFace(family, false, false);
        if (suitable(face))
            return face;
        face = ApplicationFonts::systemFontFace(family, false, false);
        if (suitable(face))
            return face;
    }
    return {};
}

} // namespace

struct PDFSaveWriter::Request::Data {
    QString sourceFilePath;
    QByteArray documentPassword;
    QByteArray pendingOpenPassword;
    QString targetFilePath;
    QVector<int> deletedSourcePages;
    QVector<PDFPageInsertion> pageInsertions;
    QVector<SaveDraft> drafts;
    QVector<PDFFormDesignerController::SaveField> formFields;
    ApplicationFonts::FontFaceData unicodeFormFont;
    QVector<PDFWebLinks::Link> autoWebLinks;
    QVector<PDFWebLinks::SourceLink> removedWebLinks;
    QVector<PDFWebLinks::ManualLink> manualLinks;
    QMap<int, QVector<PDFWatermarks::PageChange>> watermarks;
};

PDFSaveWriter::Request PDFSaveWriter::prepare(
    const PDFEditController &controller,
    const QString &targetFilePath,
    const QString &sourceFilePathOverride)
{
    Request request;
    if (!controller.m_document
        || !controller.m_document->isLoaded()
        || controller.m_document->renderFilePath().isEmpty()) {
        return request;
    }

    auto data = std::make_shared<Request::Data>();
    data->sourceFilePath = sourceFilePathOverride.isEmpty()
        ? controller.m_document->renderFilePath()
        : sourceFilePathOverride;
    data->targetFilePath = targetFilePath;
    data->documentPassword = controller.m_document->documentPassword();
    data->pendingOpenPassword = controller.m_pendingOpenPassword;
    data->autoWebLinks = controller.m_autoWebLinks;
    data->removedWebLinks = controller.m_removedWebLinks;
    data->manualLinks = controller.m_manualLinks;
    if (controller.watermarkChangesPending()) {
        for (int page = 0; page < controller.m_document->pageCount(); ++page) {
            auto changes = controller.watermarkChangesForPage(page);
            if (!changes.isEmpty())
                data->watermarks.insert(page, std::move(changes));
        }
    }
    if (controller.m_formDesignerController) {
        const bool normalizeSourceFieldAppearances
            = controller.m_formDesignerController->hasDrafts();
        const QSet<int> nativeAppearancePages
            = controller.m_formController
                && controller.m_formController->hasDrafts()
            ? controller.m_formController->changedPages()
            : QSet<int>();
        const bool normalizeAllNativeAppearances
            = controller.m_formController
            && controller.m_formController->hasDrafts()
            && controller.m_formController->allPagesChanged();
        data->formFields
            = controller.m_formDesignerController->saveFields(
                normalizeSourceFieldAppearances,
                nativeAppearancePages,
                normalizeAllNativeAppearances);
    }
    data->deletedSourcePages.reserve(controller.m_deletedSourcePages.size());
    for (int page : controller.m_deletedSourcePages) {
        data->deletedSourcePages.append(page);
    }
    std::sort(
        data->deletedSourcePages.begin(),
        data->deletedSourcePages.end());
    data->pageInsertions = controller.m_pageInsertions;
    std::sort(
        data->pageInsertions.begin(),
        data->pageInsertions.end(),
        [](const PDFPageInsertion &left, const PDFPageInsertion &right) {
            return left.page < right.page;
        });
    data->drafts.reserve(controller.m_drafts.size());
    QHash<QString, QByteArray> bundledFaceCache;
    QHash<QString, ApplicationFonts::FontFaceData> downloadedFaceCache;
    QHash<QString, ApplicationFonts::FontFaceData> systemFaceCache;
    const auto snapshotEmbeddableFace = [
        &bundledFaceCache,
        &downloadedFaceCache,
        &systemFaceCache](SaveFormat &format, const QString &text) {
        const auto supportsText = [&text](const QByteArray &bytes) {
            const QRawFont raw(bytes, 16.0, QFont::PreferNoHinting);
            if (!raw.isValid())
                return false;
            for (uint character : text.toUcs4()) {
                if (!QChar::isSpace(static_cast<char32_t>(character))
                    && !raw.supportsCharacter(character))
                    return false;
            }
            return true;
        };
        // Keep the first unavailable face as evidence for the existing
        // recovery/error path, while allowing the next exact local source to
        // satisfy a missing style or glyph.
        const auto consider = [&format, &supportsText](const QString &family,
                                                       const QByteArray &bytes,
                                                       const QString &layoutFamily = QString()) {
            const bool usable = !bytes.isEmpty() && supportsText(bytes);
            if (usable || format.embeddableFontFamily.isEmpty()) {
                format.embeddableFontFamily = family;
                format.embeddableFontData = bytes;
                format.embeddableLayoutFamily = layoutFamily;
            }
            return usable;
        };
        QString family = matchingSystemFamily(format.semanticFontFamily);
        if (!family.isEmpty()) {
            SaveFormat keyFormat = format;
            keyFormat.fontFamily = family;
            const QString key = fontFaceKey(keyFormat);
            auto face = systemFaceCache.constFind(key);
            if (face == systemFaceCache.cend()) {
                face = systemFaceCache.insert(key, ApplicationFonts::systemFontFace(
                    family, format.fontBold, format.fontItalic));
            }
            if (consider(face->isValid() ? face->family : family, face->data))
                return;
        }

        const QString downloadedFamily = matchingKnownFamily(format.semanticFontFamily,
            ApplicationFonts::downloadedFontFamilies());
        if (!downloadedFamily.isEmpty()) {
            SaveFormat keyFormat = format;
            keyFormat.fontFamily = downloadedFamily;
            const QString key = fontFaceKey(keyFormat);
            auto face = downloadedFaceCache.constFind(key);
            if (face == downloadedFaceCache.cend()) {
                face = downloadedFaceCache.insert(key, ApplicationFonts::downloadedFontFace(
                    downloadedFamily, format.fontBold, format.fontItalic));
            }
            if (consider(face->isValid() ? face->family : downloadedFamily,
                         face->data, face->layoutFamily))
                return;
        }

        const auto snapshotBundledFace = [&](const QString &bundledFamily) {
            SaveFormat keyFormat = format;
            keyFormat.fontFamily = bundledFamily;
            const QString key = fontFaceKey(keyFormat);
            auto bytes = bundledFaceCache.constFind(key);
            if (bytes == bundledFaceCache.cend()) {
                bytes = bundledFaceCache.insert(key, ApplicationFonts::bundledFontData(
                    bundledFamily, format.fontBold, format.fontItalic));
            }
            return consider(bundledFamily, bytes.value());
        };
        family = matchingBundledFamily(format.semanticFontFamily);
        if (!family.isEmpty() && snapshotBundledFace(family))
            return;
        if (isArialCompatibleFamily(format.semanticFontFamily)) {
            family = matchingBundledFamily(QStringLiteral("Arial"));
            if (!family.isEmpty())
                snapshotBundledFace(family);
        }
    };
    const auto copiedFormat = [&snapshotEmbeddableFace](
                                  const PDFEditController::CharacterFormat &format,
                                  const QString &text) {
        SaveFormat result;
        result.fontFamily = format.fontFamily;
        result.semanticFontFamily = PDFFontAliases::semanticFamily(format.fontFamily);
        result.displaySourceFingerprint = PDFFontAliases::sourceFingerprint(format.fontFamily);
        result.fontSizeRatio = format.fontSizeRatio;
        result.displayFontSizeRatio = format.displayFontSizeRatio;
        result.letterSpacingRatio = format.letterSpacingRatio;
        result.wordSpacingRatio = format.wordSpacingRatio;
        result.sourceAdvanceRatio = format.sourceAdvanceRatio;
        result.fontStretch = format.fontStretch;
        result.fontBold = format.fontBold;
        result.fontItalic = format.fontItalic;
        result.fontUnderline = format.fontUnderline;
        result.color = format.color;
        snapshotEmbeddableFace(result, text);
        return result;
    };
    for (auto draft = controller.m_drafts.cbegin();
         draft != controller.m_drafts.cend();
         ++draft) {
        if (!draft->candidate.replyToObjectId.isEmpty()
            && !controller.commentReplyIsVisible(draft->candidate)) {
            continue;
        }
        SaveDraft saveDraft;
        const bool sourceTextMoveOnly
            = PDFEditController::isSourceTextMoveOnly(*draft);
        const bool imageDraft = draft->candidate.kind == QLatin1String("addedImage")
            || draft->candidate.kind == QLatin1String("image");
        const bool shapeDraft
            = draft->candidate.kind == QLatin1String("addedShape")
            || draft->candidate.kind == QLatin1String("sourceShape");
        const bool textMarkupDraft
            = draft->candidate.kind == QLatin1String("highlight")
            || draft->candidate.kind == QLatin1String("underline")
            || draft->candidate.kind == QLatin1String("squiggly")
            || draft->candidate.kind == QLatin1String("strikeout");
        const bool stickyNoteDraft
            = draft->candidate.kind == QLatin1String("stickyNote");
        const bool freeTextDraft
            = draft->candidate.kind == QLatin1String("freeText");
        const bool stampDraft
            = draft->candidate.kind == QLatin1String("stamp");
        const bool drawingCommentDraft
            = draft->candidate.kind == QLatin1String("square")
            || draft->candidate.kind == QLatin1String("circle")
            || draft->candidate.kind == QLatin1String("line")
            || draft->candidate.kind == QLatin1String("polygon")
            || draft->candidate.kind == QLatin1String("polyLine")
            || draft->candidate.kind == QLatin1String("ink");
        saveDraft.kind = drawingCommentDraft
            ? SaveDraft::Kind::DrawingComment
            : stampDraft
            ? SaveDraft::Kind::Stamp
            : freeTextDraft
            ? SaveDraft::Kind::FreeText
            : stickyNoteDraft
            ? SaveDraft::Kind::StickyNote
            : textMarkupDraft
            ? SaveDraft::Kind::TextMarkup
            : shapeDraft
                ? SaveDraft::Kind::Shape
                : imageDraft
                    ? SaveDraft::Kind::Image
                    : SaveDraft::Kind::Text;
        saveDraft.page = draft->candidate.page;
        saveDraft.objectId = draft->candidate.objectId;
        saveDraft.headerGroupId = draft->candidate.headerGroupId;
        saveDraft.headerRole = draft->candidate.headerRole;
        saveDraft.originalText = draft->candidate.text;
        if (saveDraft.kind == SaveDraft::Kind::Text
            && !draft->candidate.parts.isEmpty()) {
            // Grouped candidates include inferred separators. Only characters
            // encoded in the source parts prove that the font can reuse them.
            saveDraft.originalText.clear();
            for (const auto &part : draft->candidate.parts) {
                saveDraft.originalText.append(part.text);
            }
        }
        saveDraft.text = draft->text;
        saveDraft.sourceAnnotationName
            = draft->candidate.sourceAnnotationName;
        saveDraft.sourceAnnotationHasName
            = draft->candidate.sourceAnnotationHasName;
        saveDraft.sourceRect = sourceTextMoveOnly
            ? (draft->candidate.initialDraftFrameRect.isEmpty()
                   ? draft->candidate.geometry.sourceRect
                   : draft->candidate.initialDraftFrameRect)
            : draft->candidate.geometry.sourceRect;
        saveDraft.frameRect = draft->frameRect;
        saveDraft.highlightRects
            = PDFEditController::highlightRectsForDraft(*draft);
        saveDraft.textMarkupQuads
            = PDFEditController::textMarkupQuadsForDraft(*draft);
        saveDraft.highlightColor = draft->candidate.highlightColor;
        saveDraft.drawingType = draft->candidate.drawingType;
        saveDraft.drawingVertices
            = PDFEditController::drawingVerticesForDraft(*draft);
        saveDraft.inkStrokes
            = PDFEditController::inkStrokesForDraft(*draft);
        saveDraft.originalCommentProperties
            = draft->candidate.commentProperties;
        saveDraft.commentProperties = draft->commentProperties;
        saveDraft.sourceDrawingAppearanceFingerprint
            = draft->candidate.sourceDrawingAppearanceFingerprint;
        saveDraft.stampName = draft->commentProperties.name;
        saveDraft.stampText = draft->candidate.stampText;
        saveDraft.stampDetail = draft->candidate.stampDetail;
        saveDraft.stampColor = draft->candidate.highlightColor;
        saveDraft.sourceAnnotationIndex
            = draft->candidate.sourceAnnotationIndex;
        saveDraft.commentAuthor = draft->candidate.commentAuthor;
        saveDraft.commentCreationDate
            = draft->candidate.commentCreationDate;
        saveDraft.commentModifiedDate
            = draft->candidate.commentModifiedDate;
        saveDraft.commentThreadId = draft->candidate.commentThreadId;
        saveDraft.replyToThreadId = draft->candidate.replyToThreadId;
        saveDraft.replyToSourceAnnotationIndex
            = draft->candidate.replyToSourceAnnotationIndex;
        saveDraft.reply = !draft->candidate.replyToObjectId.isEmpty();
        saveDraft.originalCommentStatus
            = draft->candidate.commentStatus;
        saveDraft.commentStatus = draft->commentStatus;
        saveDraft.originalCommentMarked
            = draft->candidate.commentMarked;
        saveDraft.commentMarked = draft->commentMarked;
        saveDraft.commentStatusParentAnnotationIndex
            = draft->candidate.commentStatusAnnotationIndex;
        saveDraft.commentMarkedParentAnnotationIndex
            = draft->candidate.commentMarkedAnnotationIndex;
        saveDraft.commentStateAuthor = draft->commentStateAuthor;
        saveDraft.commentStateModifiedDate
            = draft->commentStateModifiedDate;
        if ((saveDraft.commentStatus
                 != saveDraft.originalCommentStatus
             || saveDraft.commentMarked
                 != saveDraft.originalCommentMarked)
            && !saveDraft.commentStateModifiedDate.isValid()) {
            saveDraft.commentStateModifiedDate
                = QDateTime::currentDateTime();
        }
        if (draft->candidate.kind == QLatin1String("underline")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_UNDERLINE;
        } else if (draft->candidate.kind == QLatin1String("squiggly")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_SQUIGGLY;
        } else if (draft->candidate.kind == QLatin1String("strikeout")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_STRIKEOUT;
        } else if (freeTextDraft) {
            saveDraft.annotationSubtype = FPDF_ANNOT_FREETEXT;
        } else if (stampDraft) {
            saveDraft.annotationSubtype = FPDF_ANNOT_STAMP;
        } else if (draft->candidate.kind == QLatin1String("square")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_SQUARE;
        } else if (draft->candidate.kind == QLatin1String("circle")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_CIRCLE;
        } else if (draft->candidate.kind == QLatin1String("line")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_LINE;
        } else if (draft->candidate.kind == QLatin1String("polygon")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_POLYGON;
        } else if (draft->candidate.kind == QLatin1String("polyLine")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_POLYLINE;
        } else if (draft->candidate.kind == QLatin1String("ink")) {
            saveDraft.annotationSubtype = FPDF_ANNOT_INK;
        }
        saveDraft.shapeType = draft->candidate.shapeType;
        saveDraft.shapeStartRatio = draft->candidate.shapeStartRatio;
        saveDraft.shapeEndRatio = draft->candidate.shapeEndRatio;
        saveDraft.sourceShapeMetadata = draft->candidate.sourceShapeMetadata;
        saveDraft.shapeStrokeWidthRatio
            = draft->candidate.shapeStrokeWidthRatio;
        saveDraft.layoutOrigin = PDFEditController::layoutOriginForDraft(*draft);
        saveDraft.hasBaseline = draft->candidate.hasBaseline;
        saveDraft.added = draft->candidate.added;
        saveDraft.addedOrder = draft->candidate.addedOrder;
        saveDraft.deleted = draft->deleted;
        saveDraft.movesSourceObjects = sourceTextMoveOnly
            || (saveDraft.kind == SaveDraft::Kind::Image
                && !saveDraft.added
                && !saveDraft.deleted);
        saveDraft.image = draft->deleted ? QImage() : draft->candidate.image;
        saveDraft.imageQuarterTurns = draft->imageQuarterTurns;
        if (saveDraft.kind == SaveDraft::Kind::TextMarkup
            || saveDraft.kind == SaveDraft::Kind::StickyNote
            || saveDraft.kind == SaveDraft::Kind::FreeText
            || saveDraft.kind == SaveDraft::Kind::Stamp
            || saveDraft.kind == SaveDraft::Kind::DrawingComment) {
            data->drafts.append(std::move(saveDraft));
            continue;
        }
        if (!draft->candidate.parts.isEmpty()) {
            for (const PDFEditController::CandidatePart &part : draft->candidate.parts) {
                if (!part.objectPath.isEmpty()) {
                    saveDraft.sourceObjectPaths.append(part.objectPath);
                }
                if (!part.underlineObjectPath.isEmpty()) {
                    saveDraft.sourceUnderlineObjectPaths.append(
                        part.underlineObjectPath);
                }
            }
        } else if (!draft->candidate.objectPath.isEmpty()) {
            saveDraft.sourceObjectPaths.append(draft->candidate.objectPath);
        }
        if (saveDraft.kind == SaveDraft::Kind::Image
            || saveDraft.kind == SaveDraft::Kind::Shape
            || sourceTextMoveOnly) {
            data->drafts.append(std::move(saveDraft));
            continue;
        }
        saveDraft.originalFormat = copiedFormat(
            PDFEditController::characterFormatForCandidate(draft->candidate), draft->text);
        saveDraft.originalHorizontalScale = draft->candidate.horizontalScale;
        saveDraft.originalFillColor = draft->candidate.fillColor;
        saveDraft.originalStrokeColor = draft->candidate.strokeColor;
        saveDraft.originalStrokeWidth = draft->candidate.strokeWidth;
        saveDraft.originalRenderMode = draft->candidate.textRenderMode;
        for (const PDFEditController::CharacterSpan &span : draft->characterSpans) {
            saveDraft.characterSpans.append(SaveSpan{
                span.start,
                span.length,
                copiedFormat(span.format, draft->text.mid(span.start, span.length))});
        }
        for (const PDFEditController::ParagraphFormat &paragraph : draft->paragraphFormats) {
            Qt::Alignment alignment = Qt::AlignLeft;
            switch (paragraph.alignment) {
            case PDFEditController::LeftAlignment:
                alignment = Qt::AlignLeft;
                break;
            case PDFEditController::CenterAlignment:
                alignment = Qt::AlignHCenter;
                break;
            case PDFEditController::RightAlignment:
                alignment = Qt::AlignRight;
                break;
            case PDFEditController::JustifyAlignment:
                alignment = Qt::AlignJustify;
                break;
            }
            saveDraft.paragraphFormats.append(SaveParagraph{
                paragraph.start,
                paragraph.length,
                alignment,
                paragraph.lineSpacing,
                paragraph.spaceBeforeRatio,
                paragraph.spaceAfterRatio,
                paragraph.firstLineIndentRatio});
        }
        saveDraft.sourceLineBreaks = draft->sourceLineBreaks;
        saveDraft.sourceParagraphLayout = !draft->candidate.sourceParagraphFormats.isEmpty();
        data->drafts.append(std::move(saveDraft));
    }

    const bool hasUnicodeFields = std::any_of(data->formFields.cbegin(), data->formFields.cend(),
        [](const PDFFormDesignerController::SaveField &field) {
            return field.operation != PDFFormDesignerController::SaveField::Remove
                && (field.type == PDFFormDesignerController::Text
                    || field.type == PDFFormDesignerController::Dropdown
                    || field.type == PDFFormDesignerController::ListBox);
        });
    const bool hasFreeText = std::any_of(data->drafts.cbegin(), data->drafts.cend(),
        [](const SaveDraft &draft) {
            return !draft.deleted && draft.kind == SaveDraft::Kind::FreeText;
        });
    if (hasUnicodeFields || hasFreeText)
        data->unicodeFormFont = snapshotUnicodeFormFont();

    std::sort(data->drafts.begin(), data->drafts.end(), [](const SaveDraft &left,
                                                           const SaveDraft &right) {
        if (left.page != right.page) {
            return left.page < right.page;
        }
        if (left.added && right.added && left.addedOrder != right.addedOrder) {
            return left.addedOrder < right.addedOrder;
        }
        return left.objectId < right.objectId;
    });
    request.m_data = std::move(data);
    return request;
}

PDFSaveWriter::Result PDFSaveWriter::save(const Request &request)
{
    if (!request.m_data) {
        return failure(Error::SourceUnavailable);
    }

    // This is the sole owner of the writer PDFium transaction. The request is
    // an immutable GUI-thread snapshot, and completion is posted only after
    // this function returns and releases the process lock.
    PDFiumProcessLock pdfiumLock;
    PdfDocumentHandle document = PDFiumDocument::open(
        request.m_data->sourceFilePath, request.m_data->documentPassword);
    if (!document.get()) {
        return failure(Error::SourceOpenFailed);
    }
    const bool replacingPassword = !request.m_data->pendingOpenPassword.isEmpty();
    const int securityRevision = FPDF_GetSecurityHandlerRevision(document.get());
    if (replacingPassword && securityRevision >= 0) {
        // Recheck the on-disk permissions, including when a native form save
        // supplied a temporary snapshot. Opening as owner must not bypass this.
        const unsigned long requiredPermissions = securityRevision < 3
            ? 0x3cUL : 0xf3cUL;
        if ((FPDF_GetDocUserPermissions(document.get()) & requiredPermissions)
            != requiredPermissions) {
            return failure(Error::PasswordProtectionFailed);
        }
    }
    const int sourcePageCount = FPDF_GetPageCount(document.get());
    if (sourcePageCount < 0) {
        return failure(Error::SourceOpenFailed);
    }
    if (replacingPassword
        && request.m_data->drafts.isEmpty()
        && request.m_data->formFields.isEmpty()
        && request.m_data->deletedSourcePages.isEmpty()
        && request.m_data->pageInsertions.isEmpty()
        && request.m_data->autoWebLinks.isEmpty()
        && request.m_data->removedWebLinks.isEmpty()
        && request.m_data->manualLinks.isEmpty()
        && request.m_data->watermarks.isEmpty()) {
        // Password-only saves preserve source annotations, forms and appearances
        // directly through qpdf, without rewriting any page content in PDFium.
        return protectAndCommitDocumentFile(
            request.m_data->sourceFilePath,
            request.m_data->targetFilePath,
            request.m_data->pendingOpenPassword,
            sourcePageCount,
            request.m_data->documentPassword);
    }
    // Render-only inspection happens on a second worker-owned handle so that
    // temporary page-object activation changes never dirty the writer page.
    PdfDocumentHandle inspectionDocument = PDFiumDocument::open(
        request.m_data->sourceFilePath, request.m_data->documentPassword);
    if (!inspectionDocument.get()) {
        return failure(Error::SourceOpenFailed);
    }

    if (FPDF_GetPageCount(inspectionDocument.get()) != sourcePageCount) {
        return failure(Error::SourceOpenFailed);
    }
    WebLinkRemovals webLinkRemovals;
    const Result removalValidation = validateWebLinkRemovals(
        inspectionDocument.get(), sourcePageCount,
        request.m_data->deletedSourcePages, request.m_data->pageInsertions,
        request.m_data->removedWebLinks, webLinkRemovals);
    if (!removalValidation.succeeded()) {
        return removalValidation;
    }
    QVector<ManualLinkEdit> manualLinkEdits;
    const Result manualValidation = prepareManualLinkEdits(
        document.get(), inspectionDocument.get(), sourcePageCount,
        request.m_data->deletedSourcePages, request.m_data->pageInsertions,
        request.m_data->manualLinks, request.m_data->removedWebLinks, manualLinkEdits);
    if (!manualValidation.succeeded()) {
        return manualValidation;
    }
    for (auto page = request.m_data->deletedSourcePages.crbegin();
         page != request.m_data->deletedSourcePages.crend();
         ++page) {
        if (*page < 0 || *page >= sourcePageCount) {
            return failure(Error::PageDeletionFailed, *page);
        }
        FPDFPage_Delete(document.get(), *page);
        FPDFPage_Delete(inspectionDocument.get(), *page);
    }
    const int remainingSourcePageCount = sourcePageCount
        - static_cast<int>(request.m_data->deletedSourcePages.size());
    if (FPDF_GetPageCount(document.get()) != remainingSourcePageCount
        || FPDF_GetPageCount(inspectionDocument.get())
            != remainingSourcePageCount) {
        const int failedPage = request.m_data->deletedSourcePages.isEmpty()
            ? -1
            : request.m_data->deletedSourcePages.constFirst();
        return failure(Error::PageDeletionFailed, failedPage);
    }
    int previousInsertionPage = -1;
    for (const PDFPageInsertion &insertion
         : request.m_data->pageInsertions) {
        const int pageIndex = insertion.page;
        const int currentPageCount = FPDF_GetPageCount(document.get());
        if (pageIndex <= previousInsertionPage
            || pageIndex < 0
            || pageIndex > currentPageCount
            || FPDF_GetPageCount(inspectionDocument.get())
                != currentPageCount
            || insertion.size.isEmpty()) {
            return failure(Error::PageCreationFailed, pageIndex);
        }

        PdfPageHandle page(FPDFPage_New(
            document.get(),
            pageIndex,
            insertion.size.width(),
            insertion.size.height()));
        PdfPageHandle inspectionPage(FPDFPage_New(
            inspectionDocument.get(),
            pageIndex,
            insertion.size.width(),
            insertion.size.height()));
        if (!page.get() || !inspectionPage.get()) {
            return failure(Error::PageCreationFailed, pageIndex);
        }
        previousInsertionPage = pageIndex;
    }

    QSet<int> imageFormPages;
    FontResolver fontResolver(document.get(), request.m_data->drafts);
    QVector<PDFAcroFormCanonicalizer::AnnotationRelationship>
        annotationRelationships;
    QVector<PDFAcroFormCanonicalizer::StampAnnotation> stampAnnotations;
    QVector<PDFCommentAnnotation::DrawingAnnotation> drawingAnnotations;
    QVector<PDFCommentAnnotation::SourceDrawingGeometryUpdate>
        sourceDrawingGeometryUpdates;
    QVector<PDFAcroFormCanonicalizer::SourceCommentGeometryUpdate>
        sourceCommentGeometryUpdates;
    qsizetype start = 0;
    while (start < request.m_data->drafts.size()) {
        qsizetype end = start + 1;
        while (end < request.m_data->drafts.size()
               && request.m_data->drafts.at(end).page
                   == request.m_data->drafts.at(start).page) {
            ++end;
        }
        const QVector<SaveDraft> pageDrafts(
            request.m_data->drafts.cbegin() + start,
            request.m_data->drafts.cbegin() + end);
        const Result result = applyPageDrafts(
            document.get(),
            inspectionDocument.get(),
            request.m_data->drafts.at(start).page,
            pageDrafts,
            fontResolver,
            annotationRelationships,
            stampAnnotations,
            drawingAnnotations,
            sourceDrawingGeometryUpdates,
            sourceCommentGeometryUpdates,
            imageFormPages);
        if (!result.succeeded()) {
            return result;
        }
        start = end;
    }

    if (!request.m_data->watermarks.isEmpty()) {
        if (!(FPDF_GetDocPermissions(document.get()) & (1UL << 3)))
            return failure(Error::WatermarkUpdateFailed);
        for (auto it = request.m_data->watermarks.cbegin(); it != request.m_data->watermarks.cend(); ++it) {
            PdfPageHandle page(FPDF_LoadPage(document.get(), it.key()));
            QString error;
            if (!page.get() || !PDFWatermarks::apply(document.get(), page.get(), it.value(), false, error))
                return failure(Error::WatermarkUpdateFailed, it.key(), error);
        }
        QString validationError;
        if (!PDFWatermarks::validateDocument(document.get(), validationError))
            return failure(Error::WatermarkUpdateFailed, -1, validationError);
    }

    const Result removalResult = removeWebLinks(document.get(), webLinkRemovals);
    if (!removalResult.succeeded()) {
        return removalResult;
    }
    QVector<PDFAcroFormCanonicalizer::LinkAnnotation> manualLinkAnnotations;
    const Result manualResult = applyManualLinkEdits(
        document.get(), manualLinkEdits, manualLinkAnnotations);
    if (!manualResult.succeeded()) {
        return manualResult;
    }
    const Result webLinkResult = addWebLinks(
        document.get(),
        sourcePageCount,
        request.m_data->deletedSourcePages,
        request.m_data->pageInsertions,
        request.m_data->autoWebLinks);
    if (!webLinkResult.succeeded()) {
        return webLinkResult;
    }

    QVector<PDFAcroFormCanonicalizer::FreeTextAnnotation>
        freeTextAnnotations;
    QVector<PDFAcroFormCanonicalizer::TextMarkupAnnotation>
        textMarkupAnnotations;
    for (const SaveDraft &draft : request.m_data->drafts) {
        if (draft.kind == SaveDraft::Kind::TextMarkup
            && draft.added
            && !draft.deleted
            && !draft.reply) {
            PDFAcroFormCanonicalizer::TextMarkupType type;
            switch (draft.annotationSubtype) {
            case FPDF_ANNOT_HIGHLIGHT:
                type = PDFAcroFormCanonicalizer::TextMarkupType::Highlight;
                break;
            case FPDF_ANNOT_UNDERLINE:
                type = PDFAcroFormCanonicalizer::TextMarkupType::Underline;
                break;
            case FPDF_ANNOT_SQUIGGLY:
                type = PDFAcroFormCanonicalizer::TextMarkupType::Squiggly;
                break;
            case FPDF_ANNOT_STRIKEOUT:
                type = PDFAcroFormCanonicalizer::TextMarkupType::StrikeOut;
                break;
            default:
                return failure(
                    Error::TextMarkupAnnotationCreationFailed,
                    draft.page,
                    draft.objectId);
            }
            if (draft.objectId.isEmpty()) {
                return failure(
                    Error::TextMarkupAnnotationCreationFailed,
                    draft.page,
                    draft.objectId);
            }
            textMarkupAnnotations.append({
                draft.page,
                draft.objectId,
                type});
        }
        if (draft.kind == SaveDraft::Kind::FreeText
            && !draft.deleted) {
            const QString annotationId = freeTextAppearanceId(draft);
            if (annotationId.isEmpty()) {
                return failure(
                    Error::CommentAnnotationCreationFailed,
                    draft.page,
                    draft.objectId);
            }
            freeTextAnnotations.append({
                draft.page,
                annotationId,
                draft.text,
                !draft.added});
        }
        if (!draft.reply || draft.deleted || !draft.added) {
            continue;
        }
        if (draft.replyToThreadId.isEmpty() || draft.objectId.isEmpty()) {
            return failure(
                Error::CommentAnnotationCreationFailed,
                draft.page,
                draft.objectId);
        }
        PDFAcroFormCanonicalizer::AnnotationRelationship relationship;
        relationship.page = draft.page;
        relationship.parentThreadId = draft.replyToThreadId;
        relationship.childAnnotationId = draft.objectId;
        relationship.kind = PDFAcroFormCanonicalizer::
            AnnotationRelationshipKind::Reply;
        annotationRelationships.append(std::move(relationship));
    }

    if (request.m_data->pendingOpenPassword.isEmpty()
        && imageFormPages.isEmpty()
        && fontResolver.openTypeFonts().isEmpty()
        && request.m_data->formFields.isEmpty()
        && annotationRelationships.isEmpty()
        && freeTextAnnotations.isEmpty()
        && textMarkupAnnotations.isEmpty()
        && stampAnnotations.isEmpty()
        && drawingAnnotations.isEmpty()
        && sourceDrawingGeometryUpdates.isEmpty()
        && sourceCommentGeometryUpdates.isEmpty()
        && manualLinkAnnotations.isEmpty()) {
        return writeDocument(document.get(), request.m_data->targetFilePath);
    }

    QVector<PDFAcroFormWriter::Field> acroFormFields;
    if (!request.m_data->formFields.isEmpty()) {
        const Result fieldResult = prepareAcroFormFields(
            document.get(),
            request.m_data->formFields,
            acroFormFields);
        if (!fieldResult.succeeded()) {
            return fieldResult;
        }
    }

    const QFileInfo targetInfo(request.m_data->targetFilePath);
    QTemporaryDir contentFile(
        targetInfo.absoluteDir().filePath(
            QStringLiteral(".trepdf-content-XXXXXX")));
    if (!contentFile.isValid()) {
        return failure(
            Error::OutputOpenFailed,
            -1,
            contentFile.errorString());
    }
    QString contentFilePath = contentFile.filePath(QStringLiteral("content.pdf"));
    // Canonicalizers need plaintext PDF dictionaries. Remove security only in
    // this private stage of a password replacement; commit always re-encrypts.
    const QByteArray stagingPassword = replacingPassword
        ? QByteArray() : request.m_data->documentPassword;
    const Result contentResult = writeDocument(
        document.get(),
        contentFilePath,
        replacingPassword);
    if (!contentResult.succeeded()) {
        return contentResult;
    }

    if (!imageFormPages.isEmpty()) {
        QMap<int, int> pageMap;
        for (int sourcePage = 0; sourcePage < sourcePageCount; ++sourcePage) {
            const int outputPage = outputPageForSource(sourcePage, sourcePageCount,
                request.m_data->deletedSourcePages, request.m_data->pageInsertions);
            if (imageFormPages.contains(outputPage))
                pageMap.insert(outputPage, sourcePage);
        }
        const QString restoredPath = contentFile.filePath(QStringLiteral("resources.pdf"));
        bool changed = false;
        if (pageMap.size() != imageFormPages.size()
            || !PDFImageFormResources::restoreMissing(request.m_data->sourceFilePath,
                contentFilePath, restoredPath, pageMap, request.m_data->documentPassword,
                stagingPassword, changed)) {
            return failure(Error::PageContentGenerationFailed);
        }
        if (changed)
            contentFilePath = restoredPath;
    }

    if (!manualLinkAnnotations.isEmpty()) {
        QString linkError;
        if (!PDFAcroFormCanonicalizer::finalizeLinkAnnotations(
                contentFilePath, manualLinkAnnotations, linkError)) {
            return failure(Error::ManualLinkUpdateFailed, -1, linkError);
        }
    }

    if (!annotationRelationships.isEmpty()) {
        QString relationshipError;
        if (!PDFAcroFormCanonicalizer::finalizeAnnotationRelationships(
                contentFilePath,
                annotationRelationships,
                relationshipError)) {
            return failure(
                Error::CommentAnnotationCreationFailed,
                -1,
                relationshipError);
        }
    }
    if (request.m_data->formFields.isEmpty()) {
        if (!stampAnnotations.isEmpty()) {
            QString appearanceError;
            if (!PDFAcroFormCanonicalizer::finalizeStampAnnotations(
                    contentFilePath,
                    stampAnnotations,
                    appearanceError)) {
                return failure(
                    Error::CommentAnnotationCreationFailed,
                    -1,
                    appearanceError);
            }
        }
        if (!drawingAnnotations.isEmpty()) {
            QString appearanceError;
            if (!PDFAcroFormCanonicalizer::finalizeDrawingAnnotations(
                    contentFilePath,
                    drawingAnnotations,
                    appearanceError)) {
                return failure(
                    Error::CommentAnnotationCreationFailed,
                    -1,
                    appearanceError);
            }
        }
        if (!textMarkupAnnotations.isEmpty()) {
            QString appearanceError;
            if (!PDFAcroFormCanonicalizer::finalizeTextMarkupAnnotations(
                    contentFilePath,
                    textMarkupAnnotations,
                    appearanceError)) {
                return failure(
                    Error::TextMarkupAnnotationCreationFailed,
                    -1,
                    appearanceError);
            }
        }
        if (!freeTextAnnotations.isEmpty()) {
            QString appearanceError;
            if (!PDFAcroFormCanonicalizer::finalizeFreeTextAnnotations(
                    contentFilePath,
                    freeTextAnnotations,
                    request.m_data->unicodeFormFont,
                    appearanceError)) {
                return failure(
                    Error::CommentAnnotationCreationFailed,
                    -1,
                    appearanceError);
            }
        }
        if (!sourceDrawingGeometryUpdates.isEmpty()) {
            QString geometryError;
            if (!PDFAcroFormCanonicalizer::finalizeSourceDrawingGeometryUpdates(
                    contentFilePath,
                    sourceDrawingGeometryUpdates,
                    geometryError)) {
                return failure(
                    Error::CommentAnnotationUpdateFailed,
                    -1,
                    geometryError);
            }
        }
        if (!sourceCommentGeometryUpdates.isEmpty()) {
            QString geometryError;
            if (!PDFAcroFormCanonicalizer::finalizeSourceCommentGeometryUpdates(
                    contentFilePath,
                    sourceCommentGeometryUpdates,
                    geometryError)) {
                return failure(
                    Error::CommentAnnotationUpdateFailed,
                    -1,
                    geometryError);
            }
        }
        const Result relationshipValidation = validateAnnotationRelationships(
            contentFilePath,
            annotationRelationships,
            stagingPassword);
        if (!relationshipValidation.succeeded()) {
            return relationshipValidation;
        }
        return protectAndCommitDocumentFile(
            contentFilePath,
            request.m_data->targetFilePath,
            request.m_data->pendingOpenPassword,
            FPDF_GetPageCount(document.get()),
            request.m_data->documentPassword,
            fontResolver.openTypeFonts());
    }

    QTemporaryDir formFile(
        targetInfo.absoluteDir().filePath(
            QStringLiteral(".trepdf-acroform-XXXXXX")));
    if (!formFile.isValid()) {
        return failure(
            Error::OutputOpenFailed,
            -1,
            formFile.errorString());
    }
    const QString formFilePath = formFile.filePath(QStringLiteral("acroform.pdf"));
    const PDFAcroFormWriter::Result acroFormResult
        = PDFAcroFormWriter::write(
            contentFilePath,
            formFilePath,
            acroFormFields,
            request.m_data->unicodeFormFont,
            stagingPassword);
    if (!acroFormResult.succeeded) {
        return failure(
            Error::AcroFormCreationFailed,
            -1,
            acroFormResult.error);
    }
    if (!textMarkupAnnotations.isEmpty()) {
        QString appearanceError;
        if (!PDFAcroFormCanonicalizer::finalizeTextMarkupAnnotations(
                formFilePath,
                textMarkupAnnotations,
                appearanceError)) {
            return failure(
                Error::TextMarkupAnnotationCreationFailed,
                -1,
                appearanceError);
        }
    }
    if (!stampAnnotations.isEmpty()) {
        QString appearanceError;
        if (!PDFAcroFormCanonicalizer::finalizeStampAnnotations(
                formFilePath,
                stampAnnotations,
                appearanceError)) {
            return failure(
                Error::CommentAnnotationCreationFailed,
                -1,
                appearanceError);
        }
    }
    if (!drawingAnnotations.isEmpty()) {
        QString appearanceError;
        if (!PDFAcroFormCanonicalizer::finalizeDrawingAnnotations(
                formFilePath,
                drawingAnnotations,
                appearanceError)) {
            return failure(
                Error::CommentAnnotationCreationFailed,
                -1,
                appearanceError);
        }
    }
    if (!freeTextAnnotations.isEmpty()) {
        QString appearanceError;
        if (!PDFAcroFormCanonicalizer::finalizeFreeTextAnnotations(
                formFilePath,
                freeTextAnnotations,
                request.m_data->unicodeFormFont,
                appearanceError)) {
            return failure(
                Error::CommentAnnotationCreationFailed,
                -1,
                appearanceError);
        }
    }
    if (!sourceDrawingGeometryUpdates.isEmpty()) {
        QString geometryError;
        if (!PDFAcroFormCanonicalizer::finalizeSourceDrawingGeometryUpdates(
                formFilePath,
                sourceDrawingGeometryUpdates,
                geometryError)) {
            return failure(
                Error::CommentAnnotationUpdateFailed,
                -1,
                geometryError);
        }
    }
    if (!sourceCommentGeometryUpdates.isEmpty()) {
        QString geometryError;
        if (!PDFAcroFormCanonicalizer::finalizeSourceCommentGeometryUpdates(
                formFilePath,
                sourceCommentGeometryUpdates,
                geometryError)) {
            return failure(
                Error::CommentAnnotationUpdateFailed,
                -1,
                geometryError);
        }
    }
    const Result relationshipValidation = validateAnnotationRelationships(
        formFilePath,
        annotationRelationships,
        stagingPassword);
    if (!relationshipValidation.succeeded()) {
        return relationshipValidation;
    }
    return protectAndCommitDocumentFile(
        formFilePath,
        request.m_data->targetFilePath,
        request.m_data->pendingOpenPassword,
        FPDF_GetPageCount(document.get()),
        request.m_data->documentPassword,
        fontResolver.openTypeFonts());
}

PDFSaveWriter::Result PDFSaveWriter::save(const PDFEditController &controller,
                                          const QString &targetFilePath)
{
    return save(prepare(controller, targetFilePath));
}
