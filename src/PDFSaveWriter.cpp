#include "PDFSaveWriter.h"

#include "ApplicationFonts.h"
#include "PDFEditController.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
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
    qreal fontSizeRatio = 0.0;
    qreal displayFontSizeRatio = 0.0;
    int fontStretch = QFont::Unstretched;
    bool fontBold = false;
    bool fontItalic = false;
    bool fontUnderline = false;
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
    int page = -1;
    QString objectId;
    QVector<QVector<int>> sourceObjectPaths;
    QString text;
    QVector<SaveSpan> characterSpans;
    QVector<SaveParagraph> paragraphFormats;
    QRectF frameRect;
    QPointF layoutOrigin;
    SaveFormat originalFormat;
    QColor originalFillColor = Qt::black;
    QColor originalStrokeColor = Qt::black;
    qreal originalStrokeWidth = 1.0;
    FPDF_TEXT_RENDERMODE originalRenderMode = FPDF_TEXTRENDERMODE_FILL;
    quint64 addedOrder = 0;
    bool hasBaseline = false;
    bool added = false;
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
    QVector<SourceObject> sources;
    QVector<FPDF_PAGEOBJECT> objects;
    int insertionIndex = 0;
    bool append = false;
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
            FPDF_CloseDocument(m_document);
        }
    }

    FPDF_DOCUMENT get() const
    {
        return m_document;
    }

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
            FPDF_ClosePage(m_page);
        }
    }

    FPDF_PAGE get() const
    {
        return m_page;
    }

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

QFont displayFont(const SaveFormat &format, qreal pageHeight)
{
    QFont font;
    if (!format.fontFamily.isEmpty()) {
        font.setFamily(format.fontFamily);
    }
    font.setStretch(qBound(
        static_cast<int>(QFont::UltraCondensed),
        format.fontStretch,
        static_cast<int>(QFont::UltraExpanded)));
    font.setBold(format.fontBold);
    font.setItalic(format.fontItalic);
    font.setUnderline(format.fontUnderline);
    const qreal sizeRatio = format.displayFontSizeRatio > 0.0
        ? format.displayFontSizeRatio
        : format.fontSizeRatio;
    font.setPixelSize(qMax(1, qRound(sizeRatio * pageHeight)));
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
    if (!result.object || FPDFPageObj_GetType(result.object) != FPDF_PAGEOBJ_TEXT) {
        return {};
    }
    return result;
}

class FontResolver
{
public:
    explicit FontResolver(FPDF_DOCUMENT document)
        : m_document(document)
    {
    }

    ~FontResolver()
    {
        for (FPDF_FONT font : std::as_const(m_loadedFonts)) {
            FPDFFont_Close(font);
        }
    }

    FPDF_FONT resolve(const SaveFormat &format,
                      const SaveFormat &originalFormat,
                      FPDF_FONT originalFont)
    {
        if (originalFont && canReuseOriginalFont(format, originalFormat)) {
            return originalFont;
        }

        const QString key = format.fontFamily.toCaseFolded()
            + QLatin1Char('|')
            + QString::number(format.fontBold)
            + QString::number(format.fontItalic);
        const auto cached = m_fontCache.constFind(key);
        if (cached != m_fontCache.cend()) {
            return cached.value();
        }

        FPDF_FONT font = nullptr;
        const QByteArray fontData = ApplicationFonts::bundledFontData(
            format.fontFamily,
            format.fontBold,
            format.fontItalic);
        if (!fontData.isEmpty()) {
            font = FPDFText_LoadFont(
                m_document,
                reinterpret_cast<const uint8_t *>(fontData.constData()),
                static_cast<uint32_t>(fontData.size()),
                FPDF_FONT_TRUETYPE,
                true);
        }
        if (!font) {
            const QByteArray standardName = standardFontName(format).toLatin1();
            if (!standardName.isEmpty()) {
                font = FPDFText_LoadStandardFont(m_document, standardName.constData());
            }
        }
        if (!font) {
            return nullptr;
        }

        m_fontCache.insert(key, font);
        m_loadedFonts.append(font);
        return font;
    }

private:
    FPDF_DOCUMENT m_document = nullptr;
    QHash<QString, FPDF_FONT> m_fontCache;
    QVector<FPDF_FONT> m_loadedFonts;
};

PDFSaveWriter::Result failure(PDFSaveWriter::Error error,
                              int page = -1,
                              const QString &detail = {})
{
    PDFSaveWriter::Result result;
    result.error = error;
    result.page = page;
    result.detail = detail;
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

PDFSaveWriter::Result createDraftObjects(FPDF_DOCUMENT document,
                                         FPDF_PAGE page,
                                         const SaveDraft &draft,
                                         qreal pageWidth,
                                         qreal pageHeight,
                                         FPDF_FONT originalFont,
                                         FontResolver &fontResolver,
                                         const PageCoordinateMapper &coordinateMapper,
                                         QVector<FPDF_PAGEOBJECT> &objects)
{
    const QVector<LayoutRun> runs = layoutRuns(draft, pageWidth, pageHeight);
    for (const LayoutRun &run : runs) {
        FPDF_FONT font = fontResolver.resolve(
            run.format,
            draft.originalFormat,
            originalFont);
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
        if (configured
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

void destroyPreparedObjects(QVector<PreparedDraft> &preparedDrafts)
{
    for (PreparedDraft &prepared : preparedDrafts) {
        destroyObjects(prepared.objects);
    }
}

PDFSaveWriter::Result applyPageDrafts(FPDF_DOCUMENT document,
                                      int pageIndex,
                                      const QVector<SaveDraft> &drafts,
                                      FontResolver &fontResolver)
{
    FPDF_PAGE loadedPage = nullptr;
    {
        QMutexLocker pageLoadLock(&pdfiumPageLoadMutex());
        loadedPage = FPDF_LoadPage(document, pageIndex);
    }
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
        prepared.insertionIndex = prepared.append
            ? originalObjectCount
            : prepared.sources.constFirst().topLevelIndex;
        for (const SourceObject &source : std::as_const(prepared.sources)) {
            prepared.insertionIndex = qMin(
                prepared.insertionIndex,
                source.topLevelIndex);
        }

        FPDF_FONT originalFont = prepared.sources.isEmpty()
            ? nullptr
            : FPDFTextObj_GetFont(prepared.sources.constFirst().object);
        const PDFSaveWriter::Result createResult = createDraftObjects(
            document,
            page.get(),
            draft,
            pageWidth,
            pageHeight,
            originalFont,
            fontResolver,
            coordinateMapper,
            prepared.objects);
        if (!createResult.succeeded()) {
            destroyObjects(prepared.objects);
            destroyPreparedObjects(preparedDrafts);
            return createResult;
        }
        preparedDrafts.append(std::move(prepared));
    }

    std::stable_sort(
        preparedDrafts.begin(),
        preparedDrafts.end(),
        [](const PreparedDraft &left, const PreparedDraft &right) {
            return left.insertionIndex > right.insertionIndex;
        });

    QSet<FPDF_PAGEOBJECT> removed;
    for (PreparedDraft &prepared : preparedDrafts) {
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
                    PDFSaveWriter::Error::TextObjectCreationFailed,
                    pageIndex);
            }
            prepared.objects[index] = nullptr;
        }
    }

    if (!FPDFPage_GenerateContent(page.get())) {
        return failure(
            PDFSaveWriter::Error::PageContentGenerationFailed,
            pageIndex);
    }
    return {};
}

PDFSaveWriter::Result writeDocument(FPDF_DOCUMENT document,
                                    const QString &targetFilePath)
{
    SaveFileWriter writer(targetFilePath);
    if (!writer.open()) {
        return failure(
            PDFSaveWriter::Error::OutputOpenFailed,
            -1,
            writer.errorString());
    }
    if (!FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL)) {
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
    const auto copiedFormat = [](const PDFEditController::CharacterFormat &format) {
        SaveFormat result;
        result.fontFamily = format.fontFamily;
        result.fontSizeRatio = format.fontSizeRatio;
        result.displayFontSizeRatio = format.displayFontSizeRatio;
        result.fontStretch = format.fontStretch;
        result.fontBold = format.fontBold;
        result.fontItalic = format.fontItalic;
        result.fontUnderline = format.fontUnderline;
        result.color = format.color;
        return result;
    };
    for (auto draft = controller.m_drafts.cbegin();
         draft != controller.m_drafts.cend();
         ++draft) {
        SaveDraft saveDraft;
        saveDraft.page = draft->candidate.page;
        saveDraft.objectId = draft->candidate.objectId;
        saveDraft.text = draft->text;
        saveDraft.frameRect = draft->frameRect;
        saveDraft.layoutOrigin = PDFEditController::layoutOriginForDraft(*draft);
        saveDraft.hasBaseline = draft->candidate.hasBaseline;
        saveDraft.added = draft->candidate.added;
        saveDraft.addedOrder = draft->candidate.addedOrder;
        saveDraft.originalFormat = copiedFormat(
            PDFEditController::characterFormatForCandidate(draft->candidate));
        saveDraft.originalFillColor = draft->candidate.fillColor;
        saveDraft.originalStrokeColor = draft->candidate.strokeColor;
        saveDraft.originalStrokeWidth = draft->candidate.strokeWidth;
        saveDraft.originalRenderMode = draft->candidate.textRenderMode;
        if (!draft->candidate.parts.isEmpty()) {
            for (const PDFEditController::CandidatePart &part : draft->candidate.parts) {
                if (!part.objectPath.isEmpty()) {
                    saveDraft.sourceObjectPaths.append(part.objectPath);
                }
            }
        } else if (!draft->candidate.objectPath.isEmpty()) {
            saveDraft.sourceObjectPaths.append(draft->candidate.objectPath);
        }
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
