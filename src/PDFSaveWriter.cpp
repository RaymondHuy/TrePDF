#include "PDFSaveWriter.h"

#include "ApplicationFonts.h"
#include "PDFEditController.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <QFile>
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
#include <QVector>

#include <fpdf_edit.h>
#include <fpdf_save.h>

namespace {

constexpr qreal kMinimumLayoutWidth = 1.0;
constexpr qreal kMinimumObjectWidth = 0.01;
constexpr qreal kMinimumScale = 0.1;
constexpr qreal kMaximumScale = 10.0;
constexpr int kCoordinateScale = 64;

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
        Image
    };

    Kind kind = Kind::Text;
    int page = -1;
    QString objectId;
    QVector<QVector<int>> sourceObjectPaths;
    QString originalText;
    QString text;
    QVector<SaveSpan> characterSpans;
    QVector<SaveParagraph> paragraphFormats;
    QRectF sourceRect;
    QRectF frameRect;
    QImage image;
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
    bool movesSourceImage = false;
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

PDFSaveWriter::Result moveSourceImageObject(
    const SaveDraft &draft,
    const SourceObject &source,
    qreal pageWidth,
    qreal pageHeight,
    const PageCoordinateMapper &coordinateMapper)
{
    if (!source.object
        || source.parentForm
        || draft.sourceRect.isEmpty()
        || draft.frameRect.isEmpty()) {
        return failure(
            PDFSaveWriter::Error::ImageObjectCreationFailed,
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
            PDFSaveWriter::Error::ImageObjectCreationFailed,
            draft.page,
            draft.objectId);
    }

    matrix.e += static_cast<float>(targetPagePoint.x() - sourcePagePoint.x());
    matrix.f += static_cast<float>(targetPagePoint.y() - sourcePagePoint.y());
    if (!FPDFPageObj_SetMatrix(source.object, &matrix)) {
        return failure(
            PDFSaveWriter::Error::ImageObjectCreationFailed,
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
            if (!underline) {
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

PDFSaveWriter::Result applyPageDrafts(FPDF_DOCUMENT document,
                                      int pageIndex,
                                      const QVector<SaveDraft> &drafts,
                                      FontResolver &fontResolver)
{
    assertPDFiumProcessLockHeld();
    FPDF_PAGE loadedPage = FPDF_LoadPage(document, pageIndex);
    PdfPageHandle page(loadedPage);
    if (!page.get()) {
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
    const int originalObjectCount = FPDFPage_CountObjects(page.get());
    QVector<PreparedDraft> preparedDrafts;
    preparedDrafts.reserve(drafts.size());
    for (const SaveDraft &draft : drafts) {
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
        prepared.movesSourceImage = prepared.kind == SaveDraft::Kind::Image
            && !draft.added
            && !draft.deleted;
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
        if (!prepared.movesSourceImage) {
            continue;
        }
        if (prepared.sources.size() != 1) {
            destroyPreparedObjects(preparedDrafts);
            return failure(
                PDFSaveWriter::Error::UnsupportedSourceObject,
                pageIndex);
        }

        SaveDraft moveDraft;
        moveDraft.kind = SaveDraft::Kind::Image;
        moveDraft.page = pageIndex;
        moveDraft.objectId = prepared.objectId;
        moveDraft.sourceRect = prepared.sourceRect;
        moveDraft.frameRect = prepared.frameRect;
        const PDFSaveWriter::Result moveResult = moveSourceImageObject(
            moveDraft,
            prepared.sources.constFirst(),
            pageWidth,
            pageHeight,
            coordinateMapper);
        if (!moveResult.succeeded()) {
            destroyPreparedObjects(preparedDrafts);
            return moveResult;
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
        if (!prepared.movesSourceImage) {
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
                return failure(
                    prepared.kind == SaveDraft::Kind::Image
                        ? PDFSaveWriter::Error::ImageObjectCreationFailed
                        : PDFSaveWriter::Error::TextObjectCreationFailed,
                    pageIndex);
            }
            prepared.objects[index] = nullptr;
        }
    }

    if (!FPDFPage_GenerateContent(page.get())) {
        destroyPreparedObjects(preparedDrafts);
        return failure(
            PDFSaveWriter::Error::PageContentGenerationFailed,
            pageIndex);
    }
    destroyPreparedObjects(preparedDrafts);
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

} // namespace

struct PDFSaveWriter::Request::Data {
    QString sourceFilePath;
    QString targetFilePath;
    QVector<SaveDraft> drafts;
};

PDFSaveWriter::Request PDFSaveWriter::prepare(
    const PDFEditController &controller,
    const QString &targetFilePath)
{
    Request request;
    if (!controller.m_document
        || !controller.m_document->isLoaded()
        || controller.m_document->renderFilePath().isEmpty()) {
        return request;
    }

    auto data = std::make_shared<Request::Data>();
    data->sourceFilePath = controller.m_document->renderFilePath();
    data->targetFilePath = targetFilePath;
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
        SaveDraft saveDraft;
        const bool imageDraft = draft->candidate.kind == QLatin1String("addedImage")
            || draft->candidate.kind == QLatin1String("image");
        saveDraft.kind = imageDraft
            ? SaveDraft::Kind::Image
            : SaveDraft::Kind::Text;
        saveDraft.page = draft->candidate.page;
        saveDraft.objectId = draft->candidate.objectId;
        saveDraft.originalText = draft->candidate.text;
        saveDraft.text = draft->text;
        saveDraft.sourceRect = draft->candidate.geometry.sourceRect;
        saveDraft.frameRect = draft->frameRect;
        saveDraft.layoutOrigin = PDFEditController::layoutOriginForDraft(*draft);
        saveDraft.hasBaseline = draft->candidate.hasBaseline;
        saveDraft.added = draft->candidate.added;
        saveDraft.addedOrder = draft->candidate.addedOrder;
        saveDraft.deleted = draft->deleted;
        saveDraft.image = draft->deleted ? QImage() : draft->candidate.image;
        if (!draft->candidate.parts.isEmpty()) {
            for (const PDFEditController::CandidatePart &part : draft->candidate.parts) {
                if (!part.objectPath.isEmpty()) {
                    saveDraft.sourceObjectPaths.append(part.objectPath);
                }
            }
        } else if (!draft->candidate.objectPath.isEmpty()) {
            saveDraft.sourceObjectPaths.append(draft->candidate.objectPath);
        }
        if (saveDraft.kind == SaveDraft::Kind::Image) {
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
            case PDFEditController::ParagraphAlignment::Left:
                alignment = Qt::AlignLeft;
                break;
            case PDFEditController::ParagraphAlignment::Center:
                alignment = Qt::AlignHCenter;
                break;
            case PDFEditController::ParagraphAlignment::Right:
                alignment = Qt::AlignRight;
                break;
            case PDFEditController::ParagraphAlignment::Justify:
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

    FontResolver fontResolver(document.get());
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
            request.m_data->drafts.at(start).page,
            pageDrafts,
            fontResolver);
        if (!result.succeeded()) {
            return result;
        }
        start = end;
    }

    return writeDocument(document.get(), request.m_data->targetFilePath);
}

PDFSaveWriter::Result PDFSaveWriter::save(const PDFEditController &controller,
                                          const QString &targetFilePath)
{
    return save(prepare(controller, targetFilePath));
}
