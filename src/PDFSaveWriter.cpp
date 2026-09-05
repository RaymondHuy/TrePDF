#include "PDFSaveWriter.h"

#include "ApplicationFonts.h"
#include "PDFAcroFormCanonicalizer.h"
#include "PDFAcroFormWriter.h"
#include "PDFAnnotationUtils.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QRawFont>
#include <QSaveFile>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QTemporaryFile>
#include <QUuid>
#include <QVector>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>

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
struct SaveFormat {
    QString fontFamily;
    QString embeddableFontFamily;
    QByteArray embeddableFontData;
    qreal fontSizeRatio = 0.0;
    qreal displayFontSizeRatio = 0.0;
    int fontStretch = QFont::Unstretched;
    bool fontBold = false;
    bool fontItalic = false;
    bool fontUnderline = false;
    bool forceEmbeddedFont = false;
    bool allowOriginalFontReuse = true;
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
    QVector<QVector<int>> sourceObjectPaths;
    QVector<QVector<int>> sourceUnderlineObjectPaths;
    QString originalText;
    QString text;
    QString sourceAnnotationName;
    bool sourceAnnotationHasName = false;
    QVector<SaveSpan> characterSpans;
    QVector<SaveParagraph> paragraphFormats;
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
    PDFEditController::ShapeType shapeType
        = PDFEditController::RectangleShape;
    QPointF shapeStartRatio;
    QPointF shapeEndRatio = QPointF(1.0, 1.0);
    QColor shapeFillColor;
    QColor shapeStrokeColor;
    qreal shapeStrokeWidthRatio = 0.0;
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
    int insertionIndex = 0;
    bool append = false;
    bool movesSourceObjects = false;
    QRectF sourceRect;
    QRectF frameRect;
};

class PdfDocumentHandle
{
public:
    explicit PdfDocumentHandle(FPDF_DOCUMENT document = nullptr)
        : m_document(document)
    {
    }

    ~PdfDocumentHandle()
    {
        if (m_document) {
            assertPDFiumProcessLockHeld();
            FPDF_CloseDocument(m_document);
        }
    }

    FPDF_DOCUMENT get() const
    {
        return m_document;
    }

    Q_DISABLE_COPY_MOVE(PdfDocumentHandle)

private:
    FPDF_DOCUMENT m_document = nullptr;
};

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

    QSaveFile m_file;
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

    bool imageMatrix(const QRectF &rect, FS_MATRIX &matrix) const
    {
        QPointF bottomLeft;
        QPointF bottomRight;
        QPointF topLeft;
        if (!pagePoint(rect.left(), rect.bottom(), bottomLeft)
            || !pagePoint(rect.right(), rect.bottom(), bottomRight)
            || !pagePoint(rect.left(), rect.top(), topLeft)) {
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
        && left.fontSizeRatio == right.fontSizeRatio
        && left.displayFontSizeRatio == right.displayFontSizeRatio
        && left.fontStretch == right.fontStretch
        && left.fontBold == right.fontBold
        && left.fontItalic == right.fontItalic
        && left.fontUnderline == right.fontUnderline
        && left.forceEmbeddedFont == right.forceEmbeddedFont
        && left.color == right.color;
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
    QString normalized = format.fontFamily.toLower();
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
    QFont font;
    if (!format.fontFamily.isEmpty()) {
        font.setFamily(format.fontFamily);
    }
    font.setBold(format.fontBold);
    font.setItalic(format.fontItalic);
    font.setUnderline(format.fontUnderline);
    font.setPixelSize(pixelSize);
    font.setStretch(ApplicationFonts::calibratedFontStretch(
        format.fontFamily,
        format.fontStretch,
        format.fontBold,
        format.fontItalic,
        pixelSize));
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

Qt::Alignment paragraphAlignmentAt(const SaveDraft &draft, qsizetype position)
{
    for (const SaveParagraph &paragraph : draft.paragraphFormats) {
        if (position >= paragraph.start
            && position <= paragraph.start + paragraph.length) {
            return paragraph.alignment;
        }
    }
    return Qt::AlignLeft;
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

    QVector<Qt::Alignment> paragraphAlignments;
    qsizetype paragraphStart = 0;
    while (paragraphStart <= originalText.size()) {
        paragraphAlignments.append(paragraphAlignmentAt(draft, paragraphStart));
        const qsizetype newline = originalText.indexOf(
            QLatin1Char('\n'),
            paragraphStart);
        if (newline < 0) {
            break;
        }
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
    paragraphStart = 0;
    qsizetype paragraphIndex = 0;
    while (paragraphStart <= rebuiltText.size()) {
        const qsizetype newline = rebuiltText.indexOf(
            QLatin1Char('\n'),
            paragraphStart);
        const qsizetype paragraphEnd = newline < 0
            ? rebuiltText.size()
            : newline;
        const Qt::Alignment alignment = paragraphIndex < paragraphAlignments.size()
            ? paragraphAlignments.at(paragraphIndex)
            : Qt::AlignLeft;
        rebuiltParagraphs.append(SaveParagraph{
            paragraphStart,
            paragraphEnd - paragraphStart,
            alignment});
        if (newline < 0) {
            break;
        }
        paragraphStart = newline + 1;
        ++paragraphIndex;
    }

    draft.text = std::move(rebuiltText);
    draft.characterSpans = std::move(rebuiltSpans);
    draft.paragraphFormats = std::move(rebuiltParagraphs);
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
        if (!QChar::isSpace(static_cast<char32_t>(character))) {
            originalCharacters.insert(character);
        }
    }
    for (uint character : text.toUcs4()) {
        if (!QChar::isSpace(static_cast<char32_t>(character))
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

QVector<LayoutRun> layoutRuns(const SaveDraft &draft,
                              qreal pageWidth,
                              qreal pageHeight)
{
    QVector<LayoutRun> runs;
    if (draft.text.isEmpty()) {
        return runs;
    }

    const qreal contentLeft = draft.hasBaseline && !draft.added
        ? draft.layoutOrigin.x() * pageWidth
        : draft.frameRect.x() * pageWidth;
    const qreal frameRight = draft.frameRect.right() * pageWidth;
    const qreal layoutWidth = qMax(kMinimumLayoutWidth, frameRight - contentLeft);

    qreal verticalOffset = 0.0;
    qreal firstBaseline = -1.0;
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
        QTextLayout layout(paragraphText, displayFont(baseFormat, pageHeight));
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(paragraphAlignmentAt(draft, paragraphStart));
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
            line.setLineWidth(layoutWidth);
            line.setPosition(QPointF(0.0, paragraphHeight));
            const qreal baseline = verticalOffset + line.y() + line.ascent();
            if (firstBaseline < 0.0) {
                firstBaseline = baseline;
            }

            if (!emptyParagraph) {
                const qsizetype lineStart = paragraphStart + line.textStart();
                const qsizetype lineEnd = qMin(
                    paragraphEnd,
                    lineStart + line.textLength());
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
                    run.text = text;
                    run.format = formatAt(draft, start);
                    run.x = contentLeft + qMin(startX, endX);
                    run.baselineFromTop = baseline;
                    run.width = qMax(kMinimumObjectWidth, std::abs(endX - startX));
                    runs.append(std::move(run));
                }
            }
            paragraphHeight = line.y() + line.height();
        }
        layout.endLayout();

        if (paragraphHeight <= 0.0) {
            const QFontMetricsF metrics(displayFont(baseFormat, pageHeight));
            if (firstBaseline < 0.0) {
                firstBaseline = verticalOffset + metrics.ascent();
            }
            paragraphHeight = metrics.height();
        }
        verticalOffset += paragraphHeight;

        if (newline < 0) {
            break;
        }
        paragraphStart = newline + 1;
    }

    const qreal topOffset = draft.hasBaseline && !draft.added && firstBaseline >= 0.0
        ? draft.layoutOrigin.y() * pageHeight - firstBaseline
        : draft.frameRect.y() * pageHeight + 1.0;
    for (LayoutRun &run : runs) {
        run.baselineFromTop += topOffset;
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

    explicit FontResolver(FPDF_DOCUMENT document)
        : m_document(document)
    {
        assertPDFiumProcessLockHeld();
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

private:
    bool embeddedFontSupportsText(const SaveFormat &format,
                                  const QString &text,
                                  uint &unsupportedCodePoint)
    {
        const QString key = fontFaceKey(format);
        auto rawFont = m_rawFontCache.constFind(key);
        if (rawFont == m_rawFontCache.cend()) {
            const QByteArray &fontData = format.embeddableFontData;
            rawFont = m_rawFontCache.insert(
                key,
                fontData.isEmpty()
                    ? QRawFont()
                    : QRawFont(fontData, 16.0, QFont::PreferNoHinting));
        }
        if (!rawFont->isValid()) {
            return false;
        }
        for (uint character : text.toUcs4()) {
            if (!QChar::isSpace(static_cast<char32_t>(character))
                && !rawFont->supportsCharacter(character)) {
                unsupportedCodePoint = character;
                return false;
            }
        }
        return true;
    }

    FPDF_FONT loadEmbeddedFont(const SaveFormat &format)
    {
        const QString key = QStringLiteral("embedded|") + fontFaceKey(format);
        const auto cached = m_fontCache.constFind(key);
        if (cached != m_fontCache.cend()) {
            return cached.value();
        }

        const QByteArray &fontData = format.embeddableFontData;
        if (fontData.isEmpty()) {
            return nullptr;
        }
        FPDF_FONT font = FPDFText_LoadFont(
            m_document,
            reinterpret_cast<const uint8_t *>(fontData.constData()),
            static_cast<uint32_t>(fontData.size()),
            FPDF_FONT_TRUETYPE,
            true);
        if (font) {
            m_fontCache.insert(key, font);
            m_loadedFonts.append(font);
        }
        return font;
    }

    FPDF_DOCUMENT m_document = nullptr;
    QHash<QString, QRawFont> m_rawFontCache;
    QHash<QString, FPDF_FONT> m_fontCache;
    QVector<FPDF_FONT> m_loadedFonts;
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
            : FPDF_PAGEOBJ_TEXT;
        if (FPDFPageObj_GetType(source.object) != expectedType) {
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
        && coordinateMapper.imageMatrix(frame, matrix)
        && FPDFPageObj_SetMatrix(imageObject, &matrix);
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
    const SaveDraft &draft,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper,
    QVector<FPDF_PAGEOBJECT> &objects)
{
    if (draft.frameRect.isEmpty()
        || !std::isfinite(draft.frameRect.x())
        || !std::isfinite(draft.frameRect.y())
        || !std::isfinite(draft.frameRect.width())
        || !std::isfinite(draft.frameRect.height())) {
        return failure(
            PDFSaveWriter::Error::ShapeObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    const QRectF frame(
        draft.frameRect.x() * pageWidth,
        draft.frameRect.y() * pageHeight,
        draft.frameRect.width() * pageWidth,
        draft.frameRect.height() * pageHeight);
    const qreal strokeDeviceWidth = qMax<qreal>(
        0.75,
        draft.shapeStrokeWidthRatio * pageHeight);
    const qreal inset = std::min({
        strokeDeviceWidth / 2.0,
        frame.width() / 4.0,
        frame.height() / 4.0});
    const QRectF drawingRect = frame.adjusted(
        inset,
        inset,
        -inset,
        -inset);
    if (drawingRect.isEmpty()) {
        return failure(
            PDFSaveWriter::Error::ShapeObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    const auto mapPoint = [&coordinateMapper](
                              const QPointF &devicePoint,
                              QPointF &pagePoint) {
        return coordinateMapper.pagePoint(
            devicePoint.x(),
            devicePoint.y(),
            pagePoint);
    };
    FPDF_PAGEOBJECT path = nullptr;
    const auto createPath = [&path, &mapPoint](const QPointF &start) {
        QPointF pagePoint;
        if (!mapPoint(start, pagePoint)) {
            return false;
        }
        path = FPDFPageObj_CreateNewPath(
            static_cast<float>(pagePoint.x()),
            static_cast<float>(pagePoint.y()));
        return path != nullptr;
    };
    const auto lineTo = [&path, &mapPoint](const QPointF &end) {
        QPointF pagePoint;
        return path
            && mapPoint(end, pagePoint)
            && FPDFPath_LineTo(
                path,
                static_cast<float>(pagePoint.x()),
                static_cast<float>(pagePoint.y()));
    };
    const auto moveTo = [&path, &mapPoint](const QPointF &point) {
        QPointF pagePoint;
        return path
            && mapPoint(point, pagePoint)
            && FPDFPath_MoveTo(
                path,
                static_cast<float>(pagePoint.x()),
                static_cast<float>(pagePoint.y()));
    };
    const auto bezierTo = [&path, &mapPoint](
                              const QPointF &control1,
                              const QPointF &control2,
                              const QPointF &end) {
        QPointF pageControl1;
        QPointF pageControl2;
        QPointF pageEnd;
        return path
            && mapPoint(control1, pageControl1)
            && mapPoint(control2, pageControl2)
            && mapPoint(end, pageEnd)
            && FPDFPath_BezierTo(
                path,
                static_cast<float>(pageControl1.x()),
                static_cast<float>(pageControl1.y()),
                static_cast<float>(pageControl2.x()),
                static_cast<float>(pageControl2.y()),
                static_cast<float>(pageEnd.x()),
                static_cast<float>(pageEnd.y()));
    };
    const auto ratioPoint = [&frame](const QPointF &ratio) {
        return QPointF(
            frame.x()
                + qBound<qreal>(0.0, ratio.x(), 1.0)
                    * frame.width(),
            frame.y()
                + qBound<qreal>(0.0, ratio.y(), 1.0)
                    * frame.height());
    };

    bool closedShape = false;
    bool shapeCreated = false;
    switch (draft.shapeType) {
    case PDFEditController::RectangleShape: {
        const QPointF topLeft = drawingRect.topLeft();
        shapeCreated = createPath(topLeft)
            && lineTo(drawingRect.topRight())
            && lineTo(drawingRect.bottomRight())
            && lineTo(drawingRect.bottomLeft())
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    case PDFEditController::EllipseShape: {
        constexpr qreal kappa = 0.5522847498307936;
        const qreal centerX = drawingRect.center().x();
        const qreal centerY = drawingRect.center().y();
        const qreal radiusX = drawingRect.width() / 2.0;
        const qreal radiusY = drawingRect.height() / 2.0;
        shapeCreated = createPath(QPointF(centerX, drawingRect.top()))
            && bezierTo(
                QPointF(centerX + kappa * radiusX, drawingRect.top()),
                QPointF(drawingRect.right(), centerY - kappa * radiusY),
                QPointF(drawingRect.right(), centerY))
            && bezierTo(
                QPointF(drawingRect.right(), centerY + kappa * radiusY),
                QPointF(centerX + kappa * radiusX, drawingRect.bottom()),
                QPointF(centerX, drawingRect.bottom()))
            && bezierTo(
                QPointF(centerX - kappa * radiusX, drawingRect.bottom()),
                QPointF(drawingRect.left(), centerY + kappa * radiusY),
                QPointF(drawingRect.left(), centerY))
            && bezierTo(
                QPointF(drawingRect.left(), centerY - kappa * radiusY),
                QPointF(centerX - kappa * radiusX, drawingRect.top()),
                QPointF(centerX, drawingRect.top()))
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    case PDFEditController::LineShape:
    case PDFEditController::ArrowShape: {
        const QPointF start = ratioPoint(draft.shapeStartRatio);
        const QPointF end = ratioPoint(draft.shapeEndRatio);
        shapeCreated = createPath(start) && lineTo(end);
        if (shapeCreated
            && draft.shapeType == PDFEditController::ArrowShape) {
            const QPointF direction = end - start;
            const qreal length = std::hypot(
                direction.x(),
                direction.y());
            if (length <= 0.0) {
                shapeCreated = false;
                break;
            }
            const QPointF unit = direction / length;
            const QPointF normal(-unit.y(), unit.x());
            const qreal headLength = qMin(
                qMax<qreal>(6.0, strokeDeviceWidth * 4.0),
                length * 0.35);
            const qreal halfWidth = headLength * 0.55;
            const QPointF headBase = end - unit * headLength;
            const QPointF headLeft = headBase + normal * halfWidth;
            const QPointF headRight = headBase - normal * halfWidth;
            shapeCreated = moveTo(headLeft)
                && lineTo(end)
                && lineTo(headRight);
        }
        break;
    }
    case PDFEditController::TriangleShape: {
        shapeCreated = createPath(
                           QPointF(
                               drawingRect.center().x(),
                               drawingRect.top()))
            && lineTo(drawingRect.bottomRight())
            && lineTo(drawingRect.bottomLeft())
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    case PDFEditController::DiamondShape: {
        shapeCreated = createPath(
                           QPointF(
                               drawingRect.center().x(),
                               drawingRect.top()))
            && lineTo(
                QPointF(
                    drawingRect.right(),
                    drawingRect.center().y()))
            && lineTo(
                QPointF(
                    drawingRect.center().x(),
                    drawingRect.bottom()))
            && lineTo(
                QPointF(
                    drawingRect.left(),
                    drawingRect.center().y()))
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    }

    const QColor strokeColor = draft.shapeStrokeColor.isValid()
        ? draft.shapeStrokeColor.toRgb()
        : QColor(Qt::black);
    const QColor fillColor = draft.shapeFillColor.isValid()
        ? draft.shapeFillColor.toRgb()
        : QColor(Qt::transparent);
    const bool configured = shapeCreated
        && FPDFPageObj_SetStrokeColor(
            path,
            static_cast<unsigned int>(strokeColor.red()),
            static_cast<unsigned int>(strokeColor.green()),
            static_cast<unsigned int>(strokeColor.blue()),
            static_cast<unsigned int>(strokeColor.alpha()))
        && FPDFPageObj_SetStrokeWidth(
            path,
            static_cast<float>(
                coordinateMapper.strokeWidth(strokeDeviceWidth)))
        && FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND)
        && FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND)
        && (!closedShape
            || FPDFPageObj_SetFillColor(
                path,
                static_cast<unsigned int>(fillColor.red()),
                static_cast<unsigned int>(fillColor.green()),
                static_cast<unsigned int>(fillColor.blue()),
                static_cast<unsigned int>(fillColor.alpha())))
        && FPDFPath_SetDrawMode(
            path,
            closedShape ? FPDF_FILLMODE_WINDING : FPDF_FILLMODE_NONE,
            true);
    if (!configured) {
        if (path) {
            FPDFPageObj_Destroy(path);
        }
        return failure(
            PDFSaveWriter::Error::ShapeObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    objects.append(path);
    return {};
}

PDFSaveWriter::Result moveSourceObject(
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
            draft,
            pageWidth,
            pageHeight,
            coordinateMapper,
            objects);
    }

    SaveDraft preparedDraft = draft;
    normalizeDraftText(preparedDraft);
    const auto prepareFormat = [&](SaveFormat &format,
                                   const QString &text,
                                   uint &unsupportedCodePoint) {
        unsupportedCodePoint = 0;
        if (text.isEmpty()) {
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

        const QString family = fontResolver.embeddedFamilyForText(
            format,
            text,
            unsupportedCodePoint);
        if (!family.isEmpty()) {
            format.fontFamily = family;
            format.forceEmbeddedFont = true;
            return true;
        }
        return !requiresEmbeddedUnicodeFont(text)
            && format.embeddableFontFamily.isEmpty();
    };

    uint unsupportedCodePoint = 0;
    if (preparedDraft.characterSpans.isEmpty()) {
        if (!prepareFormat(
                preparedDraft.originalFormat,
                preparedDraft.text,
                unsupportedCodePoint)) {
            return failure(
                PDFSaveWriter::Error::FontUnavailable,
                draft.page,
                preparedDraft.originalFormat.fontFamily,
                unsupportedCodePoint);
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
            return failure(
                PDFSaveWriter::Error::FontUnavailable,
                draft.page,
                span.format.fontFamily,
                unsupportedCodePoint);
        }
    }

    const QVector<LayoutRun> runs = layoutRuns(
        preparedDraft,
        pageWidth,
        pageHeight);
    for (const LayoutRun &run : runs) {
        const FontResolver::ResolvedFont resolvedFont = fontResolver.resolve(
            run.format,
            draft.originalFormat,
            originalFont);
        FPDF_FONT font = resolvedFont.handle;
        if (!font) {
            return failure(
                PDFSaveWriter::Error::FontUnavailable,
                draft.page,
                run.format.fontFamily);
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

        const auto *wideText = reinterpret_cast<const FPDF_WCHAR *>(run.text.utf16());
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
        bool configured = FPDFText_SetText(textObject, wideText)
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
            && run.format.fontStretch == draft.originalFormat.fontStretch;
        if (configured && preservesOriginalHorizontalScale) {
            horizontalScale = qBound(
                kMinimumScale,
                qIsFinite(draft.originalHorizontalScale)
                    ? draft.originalHorizontalScale
                    : 1.0,
                kMaximumScale);
        } else if (configured
            && FPDFPageObj_GetBounds(textObject, &left, &bottom, &right, &top)
            && right - left > kMinimumObjectWidth) {
            horizontalScale = qBound(
                kMinimumScale,
                run.width / static_cast<qreal>(right - left),
                kMaximumScale);
        }
        FS_MATRIX matrix{};
        configured = configured
            && coordinateMapper.textMatrix(
                run.x,
                run.baselineFromTop,
                horizontalScale,
                matrix)
            && FPDFPageObj_SetMatrix(textObject, &matrix);
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
                                          &sourceCommentGeometryUpdates)
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
            ? FPDFTextObj_GetFont(prepared.sources.constFirst().object)
            : nullptr;
        if (prepared.kind == SaveDraft::Kind::Text) {
            for (const SourceObject &source : std::as_const(prepared.sources)) {
                if (FPDFTextObj_GetFont(source.object) != originalFont) {
                    originalFont = nullptr;
                    break;
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
        for (const SourceObject &source : std::as_const(prepared.sources)) {
            const PDFSaveWriter::Result moveResult = moveSourceObject(
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

    std::stable_sort(
        preparedDrafts.begin(),
        preparedDrafts.end(),
        [](const PreparedDraft &left, const PreparedDraft &right) {
            return left.insertionIndex > right.insertionIndex;
        });

    QSet<FPDF_PAGEOBJECT> removed;
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
                FPDFPageObj_Destroy(source.object);
            }
        }

        const int insertionIndex = prepared.append
            ? FPDFPage_CountObjects(page.get())
            : prepared.insertionIndex;
        for (qsizetype index = 0; index < prepared.objects.size(); ++index) {
            FPDF_PAGEOBJECT object = prepared.objects.at(index);
            if (!FPDFPage_InsertObjectAtIndex(
                    page.get(),
                    object,
                    static_cast<size_t>(insertionIndex + index))) {
                prepared.objects[index] = nullptr;
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
            prepared.objects[index] = nullptr;
        }
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
                                    const QString &targetFilePath)
{
    assertPDFiumProcessLockHeld();
    SaveFileWriter writer(targetFilePath);
    if (!writer.open()) {
        return failure(
            PDFSaveWriter::Error::OutputOpenFailed,
            -1,
            writer.errorString());
    }
    constexpr FPDF_DWORD saveFlags =
        FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS;
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

    QSaveFile target(targetFilePath);
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

PDFSaveWriter::Result validateAnnotationRelationships(
    const QString &filePath,
    const QVector<PDFAcroFormCanonicalizer::AnnotationRelationship>
        &relationships)
{
    assertPDFiumProcessLockHeld();
    if (relationships.isEmpty()) {
        return {};
    }

    const QByteArray encodedPath = QFile::encodeName(filePath);
    PdfDocumentHandle document(FPDF_LoadDocument(encodedPath.constData(), nullptr));
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

} // namespace

struct PDFSaveWriter::Request::Data {
    QString sourceFilePath;
    QString targetFilePath;
    QVector<int> deletedSourcePages;
    QVector<PDFPageInsertion> pageInsertions;
    QVector<SaveDraft> drafts;
    QVector<PDFFormDesignerController::SaveField> formFields;
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
    QHash<QString, ApplicationFonts::FontFaceData> systemFaceCache;
    const auto snapshotEmbeddableFace = [
        &bundledFaceCache,
        &systemFaceCache](SaveFormat &format) {
        const auto snapshotBundledFace = [
            &bundledFaceCache,
            &format](const QString &family) {
            SaveFormat faceKeyFormat = format;
            faceKeyFormat.fontFamily = family;
            const QString key = fontFaceKey(faceKeyFormat);
            auto cachedFace = bundledFaceCache.constFind(key);
            if (cachedFace == bundledFaceCache.cend()) {
                cachedFace = bundledFaceCache.insert(
                    key,
                    ApplicationFonts::bundledFontData(
                        family,
                        format.fontBold,
                        format.fontItalic));
            }
            format.embeddableFontFamily = family;
            format.embeddableFontData = cachedFace.value();
        };

        QString family = matchingBundledFamily(format.fontFamily);
        if (!family.isEmpty()) {
            snapshotBundledFace(family);
            return;
        }

        family = matchingSystemFamily(format.fontFamily);
        if (!family.isEmpty()) {
            format.embeddableFontFamily = family;
            SaveFormat faceKeyFormat = format;
            faceKeyFormat.fontFamily = family;
            const QString key = fontFaceKey(faceKeyFormat);
            auto cachedFace = systemFaceCache.constFind(key);
            if (cachedFace == systemFaceCache.cend()) {
                cachedFace = systemFaceCache.insert(
                    key,
                    ApplicationFonts::systemFontFace(
                        family,
                        format.fontBold,
                        format.fontItalic));
            }
            if (cachedFace->isValid()) {
                format.embeddableFontFamily = cachedFace->family;
                format.embeddableFontData = cachedFace->data;
            }
            return;
        }

        if (isArialCompatibleFamily(format.fontFamily)) {
            family = matchingBundledFamily(QStringLiteral("Arial"));
            if (!family.isEmpty()) {
                snapshotBundledFace(family);
            }
        }
    };
    const auto copiedFormat = [&snapshotEmbeddableFace](
                                  const PDFEditController::CharacterFormat &format) {
        SaveFormat result;
        result.fontFamily = format.fontFamily;
        result.fontSizeRatio = format.fontSizeRatio;
        result.displayFontSizeRatio = format.displayFontSizeRatio;
        result.fontStretch = format.fontStretch;
        result.fontBold = format.fontBold;
        result.fontItalic = format.fontItalic;
        result.fontUnderline = format.fontUnderline;
        result.color = format.color;
        snapshotEmbeddableFace(result);
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
            = draft->candidate.kind == QLatin1String("addedShape");
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
        saveDraft.originalText = draft->candidate.text;
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
        saveDraft.shapeFillColor = draft->candidate.fillColor;
        saveDraft.shapeStrokeColor = draft->candidate.strokeColor;
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
            PDFEditController::characterFormatForCandidate(draft->candidate));
        saveDraft.originalHorizontalScale = draft->candidate.horizontalScale;
        saveDraft.originalFillColor = draft->candidate.fillColor;
        saveDraft.originalStrokeColor = draft->candidate.strokeColor;
        saveDraft.originalStrokeWidth = draft->candidate.strokeWidth;
        saveDraft.originalRenderMode = draft->candidate.textRenderMode;
        for (const PDFEditController::CharacterSpan &span : draft->characterSpans) {
            saveDraft.characterSpans.append(SaveSpan{
                span.start,
                span.length,
                copiedFormat(span.format)});
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
                alignment});
        }
        data->drafts.append(std::move(saveDraft));
    }

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

    const QByteArray encodedSourcePath = QFile::encodeName(
        request.m_data->sourceFilePath);
    // This is the sole owner of the writer PDFium transaction. The request is
    // an immutable GUI-thread snapshot, and completion is posted only after
    // this function returns and releases the process lock.
    PDFiumProcessLock pdfiumLock;
    PdfDocumentHandle document(FPDF_LoadDocument(encodedSourcePath.constData(), nullptr));
    if (!document.get()) {
        return failure(Error::SourceOpenFailed);
    }
    // Render-only inspection happens on a second worker-owned handle so that
    // temporary page-object activation changes never dirty the writer page.
    PdfDocumentHandle inspectionDocument(
        FPDF_LoadDocument(encodedSourcePath.constData(), nullptr));
    if (!inspectionDocument.get()) {
        return failure(Error::SourceOpenFailed);
    }

    const int sourcePageCount = FPDF_GetPageCount(document.get());
    if (sourcePageCount < 0
        || FPDF_GetPageCount(inspectionDocument.get()) != sourcePageCount) {
        return failure(Error::SourceOpenFailed);
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

    FontResolver fontResolver(document.get());
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
            sourceCommentGeometryUpdates);
        if (!result.succeeded()) {
            return result;
        }
        start = end;
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

    if (request.m_data->formFields.isEmpty()
        && annotationRelationships.isEmpty()
        && freeTextAnnotations.isEmpty()
        && textMarkupAnnotations.isEmpty()
        && stampAnnotations.isEmpty()
        && drawingAnnotations.isEmpty()
        && sourceDrawingGeometryUpdates.isEmpty()
        && sourceCommentGeometryUpdates.isEmpty()) {
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
    QTemporaryFile contentFile(
        targetInfo.absoluteDir().filePath(
            QStringLiteral(".trepdf-content-XXXXXX.pdf")));
    if (!contentFile.open()) {
        return failure(
            Error::OutputOpenFailed,
            -1,
            contentFile.errorString());
    }
    const QString contentFilePath = contentFile.fileName();
    contentFile.close();
    const Result contentResult = writeDocument(
        document.get(),
        contentFilePath);
    if (!contentResult.succeeded()) {
        return contentResult;
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
            annotationRelationships);
        if (!relationshipValidation.succeeded()) {
            return relationshipValidation;
        }
        return commitDocumentFile(
            contentFilePath,
            request.m_data->targetFilePath);
    }

    QTemporaryFile formFile(
        targetInfo.absoluteDir().filePath(
            QStringLiteral(".trepdf-acroform-XXXXXX.pdf")));
    if (!formFile.open()) {
        return failure(
            Error::OutputOpenFailed,
            -1,
            formFile.errorString());
    }
    const QString formFilePath = formFile.fileName();
    formFile.close();
    const PDFAcroFormWriter::Result acroFormResult
        = PDFAcroFormWriter::write(
            contentFilePath,
            formFilePath,
            acroFormFields);
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
        annotationRelationships);
    if (!relationshipValidation.succeeded()) {
        return relationshipValidation;
    }
    return commitDocumentFile(
        formFilePath,
        request.m_data->targetFilePath);
}

PDFSaveWriter::Result PDFSaveWriter::save(const PDFEditController &controller,
                                          const QString &targetFilePath)
{
    return save(prepare(controller, targetFilePath));
}
