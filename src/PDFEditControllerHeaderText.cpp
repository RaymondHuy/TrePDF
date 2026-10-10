#include "PDFEditController.h"
#include "ApplicationFonts.h"
#include "PDFHeaderMetadata.h"

#include <QBuffer>
#include <QFileInfo>
#include <QFontInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextOption>
#include <QtTranslation>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr qsizetype kMaximumHeaderCharacters = 700;
constexpr int kMaximumHeaderPages = 10000;
constexpr qsizetype kMaximumHeaderCharacterWork = 1000000;
constexpr qsizetype kMaximumPageExpressionLength = 10000;
constexpr int kMaximumPageRangeWork = 100000;
constexpr qreal kHeaderHeight = 100.0;
constexpr qreal kHeaderSideMargin = 24.0;
constexpr qreal kHeaderFontSize = 12.0;
constexpr qreal kWriterTopPadding = 1.0;
constexpr qint64 kMaximumHeaderImageFileBytes = 64 * 1024 * 1024;
constexpr qint64 kMaximumHeaderImagePixels = 40000000;
constexpr int kMaximumHeaderImagePreviewDimension = 1024;

enum class PageParseResult { Valid, Invalid, TooLarge };

enum class PageNumberRangeResult { Valid, Invalid, RomanLimit, TooLarge };

PageNumberRangeResult validatePageNumberRange(int pageCount, int firstPage,
                                             int lastPage, int romanStartPage,
                                             int decimalStartPage)
{
    const auto validStart = [firstPage, lastPage](int page) {
        return page == 0 || (page >= firstPage && page <= lastPage);
    };
    if (firstPage < 1 || lastPage < firstPage || lastPage > pageCount
        || !validStart(romanStartPage) || !validStart(decimalStartPage)
        || (romanStartPage == 0 && decimalStartPage == 0)
        || (romanStartPage > 0 && decimalStartPage > 0
            && romanStartPage >= decimalStartPage))
        return PageNumberRangeResult::Invalid;
    if (romanStartPage > 0) {
        const int romanLast = decimalStartPage > 0 ? decimalStartPage - 1 : lastPage;
        if (romanLast - romanStartPage + 1 > 3999)
            return PageNumberRangeResult::RomanLimit;
    }
    const int firstNumberedPage = romanStartPage > 0 ? romanStartPage : decimalStartPage;
    return lastPage - firstNumberedPage + 1 > kMaximumHeaderPages
        ? PageNumberRangeResult::TooLarge : PageNumberRangeResult::Valid;
}

QString pageNumberText(int oneBasedPage, int romanStartPage, int decimalStartPage)
{
    if (decimalStartPage > 0 && oneBasedPage >= decimalStartPage)
        return QString::number(oneBasedPage - decimalStartPage + 1);
    if (romanStartPage <= 0 || oneBasedPage < romanStartPage)
        return {};
    int number = oneBasedPage - romanStartPage + 1;
    static constexpr struct {
        int value;
        const char *text;
    } numerals[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
        {100, "c"}, {90, "xc"}, {50, "l"}, {40, "xl"},
        {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    QString result;
    for (const auto &numeral : numerals) {
        while (number >= numeral.value) {
            result.append(QLatin1String(numeral.text));
            number -= numeral.value;
        }
    }
    return result;
}

PageParseResult headerPages(const QString &expression, int pageCount,
                            QList<int> &pages)
{
    if (expression.size() > kMaximumPageExpressionLength)
        return PageParseResult::TooLarge;
    const QString value = expression.trimmed();
    if (value.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
        if (pageCount > kMaximumHeaderPages)
            return PageParseResult::TooLarge;
        pages.reserve(pageCount);
        for (int page = 0; page < pageCount; ++page)
            pages.append(page);
        return pages.isEmpty() ? PageParseResult::Invalid : PageParseResult::Valid;
    }
    if (value.isEmpty())
        return PageParseResult::Invalid;

    static const QRegularExpression rangePattern(
        QStringLiteral("^([0-9]+)(?:\\s*-\\s*([0-9]+))?$"));
    QSet<int> selectedPages;
    int rangeWork = 0;
    const QStringList ranges = value.split(QLatin1Char(','), Qt::KeepEmptyParts);
    for (const QString &range : ranges) {
        const auto match = rangePattern.match(range.trimmed());
        if (!match.hasMatch())
            return PageParseResult::Invalid;
        bool validFirst = false;
        bool validLast = false;
        const int first = match.captured(1).toInt(&validFirst);
        const int last = match.captured(2).isEmpty()
            ? first : match.captured(2).toInt(&validLast);
        if (!validFirst || (!match.captured(2).isEmpty() && !validLast)
            || first < 1 || last < first || last > pageCount)
            return PageParseResult::Invalid;
        if (last - first + 1 > kMaximumPageRangeWork - rangeWork)
            return PageParseResult::TooLarge;
        rangeWork += last - first + 1;
        // Zero-based iteration avoids an overflowing increment for INT_MAX.
        for (int page = first - 1; page < last; ++page) {
            selectedPages.insert(page);
            if (selectedPages.size() > kMaximumHeaderPages)
                return PageParseResult::TooLarge;
        }
    }
    pages = selectedPages.values();
    std::sort(pages.begin(), pages.end());
    return pages.isEmpty() ? PageParseResult::Invalid : PageParseResult::Valid;
}

bool validHeaderText(const QString &text)
{
    if (text.isEmpty() || text.size() > kMaximumHeaderCharacters
        || text.trimmed().isEmpty())
        return false;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character.isHighSurrogate()) {
            if (++index >= text.size() || !text.at(index).isLowSurrogate())
                return false;
        } else if (character.isLowSurrogate()
                   || (character.category() == QChar::Other_Control
                       && character != QLatin1Char('\n'))) {
            return false;
        }
    }
    return true;
}

QVector<QColor> normalizedHeaderColors(const QString &text, const QVariantList &colors)
{
    QString lineText;
    QVector<QColor> lineColors;
    lineText.reserve(text.size());
    lineColors.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        lineColors.append(colors.at(index).value<QColor>());
        if (character == QLatin1Char('\r')) {
            lineText.append(QLatin1Char('\n'));
            if (index + 1 < text.size() && text.at(index + 1) == QLatin1Char('\n'))
                ++index;
        } else {
            lineText.append(character == QChar::LineSeparator
                    || character == QChar::ParagraphSeparator
                ? QLatin1Char('\n') : character);
        }
    }

    QVector<QColor> result;
    result.reserve(lineText.size());
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, lineText);
    qsizetype start = 0;
    for (qsizetype end = boundaries.toNextBoundary(); end >= 0;
         end = boundaries.toNextBoundary()) {
        const QString cluster = lineText.sliced(start, end - start);
        const QString normalized = cluster.normalized(QString::NormalizationForm_C);
        // A composed character belongs to its base character. Unchanged
        // clusters retain the original UTF-16 colors without shifting suffixes.
        for (qsizetype index = 0; index < normalized.size(); ++index)
            result.append(lineColors.at(cluster == normalized ? start + index : start));
        start = end;
    }
    return result;
}

qreal headerLayoutHeight(const QString &text, const QFont &font,
                        qreal width, qreal maximumHeight)
{
    // Match the plain, left-aligned added-text writer, including line advance
    // quantization and its one-point top inset. Measure before creating drafts.
    qreal height = kWriterTopPadding;
    const QStringList paragraphs = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString &paragraph : paragraphs) {
        QTextLayout layout(paragraph.isEmpty() ? QStringLiteral(" ") : paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(Qt::AlignLeft);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width);
            if (line.naturalTextWidth() > width + 0.001) {
                layout.endLayout();
                return -1.0;
            }
            height += std::floor(line.height() * 64.0) / 64.0;
            if (!std::isfinite(height) || height > maximumHeight + 0.001) {
                layout.endLayout();
                return -1.0;
            }
        }
        layout.endLayout();
    }
    return height;
}

} // namespace

QString PDFEditController::headerTextFontFamily() const
{
    const QStringList families = ApplicationFonts::bundledFontFamilies();
    return families.contains(QStringLiteral("Arial"), Qt::CaseInsensitive)
        ? QStringLiteral("Arial")
        : families.isEmpty() ? QFontInfo(QFont()).family() : families.constFirst();
}

QStringList PDFEditController::headerTextFontFamilies(int filter, bool bold, bool italic) const
{
    using Language = ApplicationFonts::FontLanguage;
    switch (filter) {
    case VietnameseFonts:
        return ApplicationFonts::fontFamiliesForLanguage(Language::Vietnamese, bold, italic);
    case EnglishFonts:
        return ApplicationFonts::fontFamiliesForLanguage(Language::English, bold, italic);
    case ChineseFonts:
        return ApplicationFonts::fontFamiliesForLanguage(Language::Chinese, bold, italic);
    case JapaneseFonts:
        return ApplicationFonts::fontFamiliesForLanguage(Language::Japanese, bold, italic);
    case KoreanFonts:
        return ApplicationFonts::fontFamiliesForLanguage(Language::Korean, bold, italic);
    default:
        return ApplicationFonts::availableFontFamilies(bold, italic);
    }
}

bool PDFEditController::addHeaderText(const QString &text, const QString &pages,
                                    qreal xPoints, qreal yPoints, qreal widthPoints,
                                    const QVariantMap &format)
{
    if (!m_document || !m_document->isLoaded() || m_saving)
        return false;
    QList<int> selectedPages;
    const PageParseResult parsed = headerPages(pages, m_document->pageCount(), selectedPages);
    if (parsed == PageParseResult::Invalid) {
        //% "Enter valid page numbers or ranges, such as 1-3, 5."
        emit statusMessage(qtTrId("HeaderTextInvalidPages"));
        return false;
    }
    if (parsed == PageParseResult::TooLarge) {
        //% "This header batch is too large. Apply it to fewer pages."
        emit statusMessage(qtTrId("HeaderTextBatchTooLarge"));
        return false;
    }
    return addHeaderTextBatch(selectedPages, {text}, xPoints, yPoints, widthPoints, format);
}

QString PDFEditController::headerPageNumberText(int page, int firstPage, int lastPage,
                                               int romanStartPage, int decimalStartPage) const
{
    if (!m_document || !m_document->isLoaded() || page < 0 || page >= m_document->pageCount()
        || page < firstPage - qint64(1) || page >= lastPage
        || validatePageNumberRange(m_document->pageCount(), firstPage, lastPage,
                                   romanStartPage, decimalStartPage) != PageNumberRangeResult::Valid)
        return {};
    return pageNumberText(page + 1, romanStartPage, decimalStartPage);
}

bool PDFEditController::addHeaderPageNumbers(int firstPage, int lastPage,
                                           int romanStartPage, int decimalStartPage,
                                           qreal xPoints, qreal yPoints, qreal widthPoints,
                                           const QVariantMap &format)
{
    if (!m_document || !m_document->isLoaded() || m_saving)
        return false;
    switch (validatePageNumberRange(m_document->pageCount(), firstPage, lastPage,
                                    romanStartPage, decimalStartPage)) {
    case PageNumberRangeResult::Invalid:
        //% "Choose valid start and stop pages, with Roman numerals before Arabic numbers."
        emit statusMessage(qtTrId("HeaderPageNumbersInvalidRange"));
        return false;
    case PageNumberRangeResult::RomanLimit:
        //% "Roman numerals support at most 3,999 numbered pages."
        emit statusMessage(qtTrId("HeaderPageNumbersRomanLimit"));
        return false;
    case PageNumberRangeResult::TooLarge:
        //% "This header batch is too large. Apply it to fewer pages."
        emit statusMessage(qtTrId("HeaderTextBatchTooLarge"));
        return false;
    case PageNumberRangeResult::Valid:
        break;
    }
    const int firstNumberedPage = romanStartPage > 0 ? romanStartPage : decimalStartPage;
    QList<int> pages;
    QStringList texts;
    pages.reserve(lastPage - firstNumberedPage + 1);
    texts.reserve(lastPage - firstNumberedPage + 1);
    // Use zero-based iteration so a document ending at INT_MAX cannot overflow.
    for (int page = firstNumberedPage - 1; page < lastPage; ++page) {
        pages.append(page);
        texts.append(pageNumberText(page + 1, romanStartPage, decimalStartPage));
    }
    return addHeaderTextBatch(pages, texts, xPoints, yPoints, widthPoints, format, true);
}

bool PDFEditController::addHeaderTextBatch(const QList<int> &selectedPages,
                                         const QStringList &texts, qreal xPoints,
                                         qreal yPoints, qreal widthPoints,
                                         const QVariantMap &format, bool pageNumbers)
{
    if (!m_document || !m_document->isLoaded() || m_saving)
        return false;
    if (watermarkChangesPending()) {
        //% "Save or discard watermark changes before editing page content."
        emit statusMessage(qtTrId("SaveWatermarkBeforeEditing"));
        return false;
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return false;
    }
    if (!std::isfinite(xPoints) || !std::isfinite(yPoints) || !std::isfinite(widthPoints)
        || xPoints < 0.0 || yPoints < 0.0 || widthPoints < 0.0
        || yPoints >= kHeaderHeight) {
        //% "Choose a text position inside the first 100 pt of the page."
        emit statusMessage(qtTrId("HeaderTextInvalidPosition"));
        return false;
    }

    Q_ASSERT(!selectedPages.isEmpty());
    Q_ASSERT(texts.size() == 1 || texts.size() == selectedPages.size());
    QStringList normalizedTexts;
    normalizedTexts.reserve(texts.size());
    qsizetype characterWork = 0;
    for (const QString &text : texts) {
        // Bound work even when an API caller bypasses the panel's input limit.
        QString normalizedText;
        if (text.size() <= kMaximumHeaderCharacters * 4) {
            normalizedText = text;
            normalizedText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            normalizedText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            normalizedText.replace(QChar::LineSeparator, QLatin1Char('\n'));
            normalizedText.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            normalizedText = normalizedText.normalized(QString::NormalizationForm_C);
        }
        if (!validHeaderText(normalizedText)) {
            //% "Enter header text using at most 700 characters, without unsupported control characters."
            emit statusMessage(qtTrId("HeaderTextInvalidContent"));
            return false;
        }
        characterWork += normalizedText.size();
        normalizedTexts.append(std::move(normalizedText));
    }
    if (texts.size() == 1)
        characterWork *= selectedPages.size();
    if (characterWork > kMaximumHeaderCharacterWork
        || m_nextAddedObjectId > std::numeric_limits<quint64>::max()
            - static_cast<quint64>(selectedPages.size())) {
        //% "This header batch is too large. Apply it to fewer pages."
        emit statusMessage(qtTrId("HeaderTextBatchTooLarge"));
        return false;
    }

    const QVariant sizeValue = format.value(QStringLiteral("fontSize"), kHeaderFontSize);
    bool validSize = false;
    const qreal fontSize = sizeValue.toDouble(&validSize);
    const QVariant familyValue = format.value(QStringLiteral("fontFamily"), headerTextFontFamily());
    const QString family = familyValue.toString().trimmed();
    const QVariant colorValue = format.value(QStringLiteral("color"), QColor(Qt::black));
    const QColor color = colorValue.value<QColor>();
    const QVariant colorsValue = format.value(QStringLiteral("colors"), QVariantList{});
    const QVariantList colors = colorsValue.toList();
    const bool validColors = colorsValue.metaType().id() == QMetaType::QVariantList
        && (colors.isEmpty() || (texts.size() == 1 && colors.size() == texts.constFirst().size()
            && std::all_of(colors.cbegin(), colors.cend(), [](const QVariant &value) {
                const QColor characterColor = value.value<QColor>();
                return characterColor.isValid() && characterColor.alpha() == 255;
            })));
    const auto validStyleValue = [&format](const QString &key) {
        return !format.contains(key) || format.value(key).metaType().id() == QMetaType::Bool;
    };
    if (!validSize || sizeValue.metaType().id() == QMetaType::Bool
        || !std::isfinite(fontSize) || fontSize < 1.0 || fontSize > 512.0
        || std::floor(fontSize) != fontSize
        || familyValue.metaType().id() != QMetaType::QString
        || family.isEmpty() || family.size() > 256
        || !color.isValid() || color.alpha() != 255 || !validColors
        || !validStyleValue(QStringLiteral("fontBold"))
        || !validStyleValue(QStringLiteral("fontItalic"))
        || !validStyleValue(QStringLiteral("fontUnderline"))) {
        //% "Choose a valid font, a whole-number size from 1 to 512 pt, and an opaque text color."
        emit statusMessage(qtTrId("HeaderTextInvalidFormat"));
        return false;
    }
    const QVector<QColor> normalizedColors = colors.isEmpty()
        ? QVector<QColor>() : normalizedHeaderColors(texts.constFirst(), colors);
    if (!colors.isEmpty() && normalizedColors.size() != normalizedTexts.constFirst().size()) {
        //% "Choose a valid font, a whole-number size from 1 to 512 pt, and an opaque text color."
        emit statusMessage(qtTrId("HeaderTextInvalidFormat"));
        return false;
    }
    const bool bold = format.value(QStringLiteral("fontBold"), false).toBool();
    const bool italic = format.value(QStringLiteral("fontItalic"), false).toBool();
    const bool underline = format.value(QStringLiteral("fontUnderline"), false).toBool();
    if (!ApplicationFonts::isFontFaceSelectable(family, bold, italic)
        || !ApplicationFonts::fontFaceSupportsText(
            family, bold, italic, normalizedTexts.join(QLatin1Char('\n')))) {
        //% "The selected header font or style is unavailable or does not support this text. Choose another font."
        emit statusMessage(qtTrId("HeaderTextFontUnavailable"));
        return false;
    }
    const QFont layoutFont = fontForDisplay(family, 100, bold, italic, underline, fontSize);

    Candidate prototype;
    prototype.kind = QStringLiteral("addedText");
    //% "Add Text"
    prototype.label = qtTrId("AddText");
    prototype.fontFamily = family;
    prototype.fontBold = bold;
    prototype.fontItalic = italic;
    prototype.fontUnderline = underline;
    prototype.color = normalizedColors.isEmpty() ? color : normalizedColors.constFirst();
    prototype.fillColor = prototype.color;
    prototype.added = true;
    prototype.editable = true;
    prototype.headerGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    prototype.headerRole = pageNumbers ? QStringLiteral("pageNumber") : QStringLiteral("text");

    Command command;
    command.kind = Command::Kind::ObjectDraftBatch;
    command.addedDrafts.reserve(selectedPages.size());
    quint64 nextAddedId = m_nextAddedObjectId;
    QHash<qreal, qreal> heightByWidth;
    for (qsizetype pageIndex = 0; pageIndex < selectedPages.size(); ++pageIndex) {
        const int page = selectedPages.at(pageIndex);
        const QString &normalizedText = normalizedTexts.at(normalizedTexts.size() == 1 ? 0 : pageIndex);
        const QSizeF size = m_document->pageSize(page);
        // Explicit placement keeps its physical origin on every target page.
        // A narrower page reduces only the width; it never shifts the origin.
        const qreal desiredWidth = widthPoints > 0.0
            ? widthPoints : size.width() - 2.0 * kHeaderSideMargin;
        const qreal width = qMin(desiredWidth, size.width() - xPoints);
        const qreal maximumHeight = qMin(kHeaderHeight, size.height()) - yPoints;
        qreal height = -1.0;
        if (std::isfinite(size.width()) && std::isfinite(size.height())
            && width > 0.0 && maximumHeight > 0.0) {
            const auto cached = normalizedTexts.size() == 1
                ? heightByWidth.constFind(width) : heightByWidth.cend();
            height = cached == heightByWidth.cend()
                ? headerLayoutHeight(normalizedText, layoutFont, width,
                                     kHeaderHeight - yPoints)
                : *cached;
            if (normalizedTexts.size() == 1)
                heightByWidth.insert(width, height);
        }
        if (height <= 0.0 || height > maximumHeight + 0.001) {
            //% "Header text does not fit within the first 100 pt on page %1. Shorten the text."
            emit statusMessage(qtTrId("HeaderTextDoesNotFit").arg(page + 1));
            return false;
        }

        Candidate candidate = prototype;
        candidate.page = page;
        candidate.addedOrder = ++nextAddedId;
        candidate.objectId = QStringLiteral("added:") + QString::number(candidate.addedOrder);
        candidate.fontSizeRatio = fontSize / size.height();
        candidate.displayFontSizeRatio = candidate.fontSizeRatio;
        setInitialGeometry(candidate, QRectF(xPoints / size.width(),
                                            yPoints / size.height(),
                                            width / size.width(), height / size.height()));
        Draft draft = initialDraftForCandidate(candidate);
        setDraftText(draft, normalizedText);
        if (!normalizedColors.isEmpty()) {
            CharacterFormat characterFormat = primaryCharacterFormat(draft);
            draft.characterSpans.clear();
            qsizetype start = 0;
            for (qsizetype end = 1; end <= normalizedColors.size(); ++end) {
                if (end < normalizedColors.size()
                    && normalizedColors.at(end) == normalizedColors.at(start))
                    continue;
                characterFormat.color = normalizedColors.at(start);
                draft.characterSpans.append(CharacterSpan{start, end - start, characterFormat});
                start = end;
            }
        }
        command.addedDrafts.append(std::move(draft));
    }

    // Invalid input above leaves the editor, redo stack, and object IDs untouched.
    finishSelectedFrameResize();
    finishSelectedTextEdit();
    m_nextAddedObjectId = nextAddedId;
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}

QVariantMap PDFEditController::prepareHeaderImage(const QUrl &source)
{
    if (!m_document || !m_document->isLoaded() || m_saving)
        return {};
    if (watermarkChangesPending()) {
        //% "Save or discard watermark changes before editing page content."
        emit statusMessage(qtTrId("SaveWatermarkBeforeEditing"));
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
    if (!imageFile.isFile() || !imageFile.isReadable() || imageFile.size() <= 0) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }
    if (imageFile.size() > kMaximumHeaderImageFileBytes) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }

    QImageReader reader(imageFile.absoluteFilePath());
    reader.setAutoTransform(true);
    const auto exceedsPixelLimit = [](const QSize &size) {
        return size.isValid()
            && qint64(size.width()) * size.height() > kMaximumHeaderImagePixels;
    };
    if (exceedsPixelLimit(reader.size())) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }
    QImage image = reader.read();
    if (exceedsPixelLimit(image.size())) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    image.setDevicePixelRatio(1.0);
    if (image.isNull()) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }

    const QImage preview = image.width() > kMaximumHeaderImagePreviewDimension
            || image.height() > kMaximumHeaderImagePreviewDimension
        ? image.scaled(kMaximumHeaderImagePreviewDimension,
                       kMaximumHeaderImagePreviewDimension,
                       Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : image;
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !preview.save(&buffer, "PNG")) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }
    const QVariantMap result{
        {QStringLiteral("previewSource"), QStringLiteral("data:image/png;base64,")
             + QString::fromLatin1(png.toBase64())},
        {QStringLiteral("pixelWidth"), image.width()},
        {QStringLiteral("pixelHeight"), image.height()}};
    m_headerImage = std::move(image);
    return result;
}

void PDFEditController::clearHeaderImage()
{
    m_headerImage = QImage();
}

bool PDFEditController::addHeaderImage(const QString &pages, qreal xPoints,
                                       qreal yPoints, qreal widthPoints,
                                       qreal heightPoints, int quarterTurns)
{
    if (!m_document || !m_document->isLoaded() || m_saving)
        return false;
    if (watermarkChangesPending()) {
        //% "Save or discard watermark changes before editing page content."
        emit statusMessage(qtTrId("SaveWatermarkBeforeEditing"));
        return false;
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return false;
    }
    if (m_headerImage.isNull()) {
        //% "Choose an image to add to the header."
        emit statusMessage(qtTrId("HeaderImageRequired"));
        return false;
    }
    if (!std::isfinite(xPoints) || !std::isfinite(yPoints)
        || !std::isfinite(widthPoints) || !std::isfinite(heightPoints)
        || xPoints < 0.0 || yPoints < 0.0 || widthPoints <= 0.0 || heightPoints <= 0.0
        || yPoints >= kHeaderHeight || heightPoints > kHeaderHeight - yPoints
        || quarterTurns < 0 || quarterTurns > 3) {
        //% "Keep the image within the first 100 pt of the page."
        emit statusMessage(qtTrId("HeaderImageInvalidPosition"));
        return false;
    }

    QList<int> selectedPages;
    const PageParseResult parsed = headerPages(pages, m_document->pageCount(), selectedPages);
    if (parsed == PageParseResult::Invalid) {
        //% "Enter valid page numbers or ranges, such as 1-3, 5."
        emit statusMessage(qtTrId("HeaderTextInvalidPages"));
        return false;
    }
    if (parsed == PageParseResult::TooLarge
        || m_nextAddedObjectId > std::numeric_limits<quint64>::max()
            - static_cast<quint64>(selectedPages.size())) {
        //% "This header batch is too large. Apply it to fewer pages."
        emit statusMessage(qtTrId("HeaderTextBatchTooLarge"));
        return false;
    }

    Candidate prototype;
    prototype.kind = QStringLiteral("addedImage");
    //% "Image"
    prototype.label = qtTrId("Image");
    prototype.editable = true;
    prototype.added = true;
    prototype.image = m_headerImage;
    prototype.headerGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    prototype.headerRole = QStringLiteral("image");
    prototype.headerImageDigest = PDFHeaderMetadata::imageDigest(m_headerImage);

    Command command;
    command.kind = Command::Kind::ObjectDraftBatch;
    command.addedDrafts.reserve(selectedPages.size());
    quint64 nextAddedId = m_nextAddedObjectId;
    for (int page : std::as_const(selectedPages)) {
        const QSizeF size = m_document->pageSize(page);
        if (!std::isfinite(size.width()) || !std::isfinite(size.height())
            || size.width() <= 0.0 || size.height() <= 0.0
            || widthPoints > size.width() - xPoints
            || heightPoints > qMin(kHeaderHeight, size.height()) - yPoints) {
            //% "The header image does not fit within the first 100 pt on page %1. Resize or move it."
            emit statusMessage(qtTrId("HeaderImageDoesNotFit").arg(page + 1));
            return false;
        }
        Candidate candidate = prototype;
        candidate.page = page;
        candidate.addedOrder = ++nextAddedId;
        candidate.objectId = QStringLiteral("added-image:")
            + QString::number(candidate.addedOrder);
        setInitialGeometry(candidate, QRectF(xPoints / size.width(),
                                            yPoints / size.height(),
                                            widthPoints / size.width(),
                                            heightPoints / size.height()));
        Draft draft = initialDraftForCandidate(candidate);
        draft.imageQuarterTurns = quarterTurns;
        command.addedDrafts.append(std::move(draft));
    }

    // Validate the full batch before mutating editor state, IDs, or history.
    finishSelectedFrameResize();
    finishSelectedTextEdit();
    m_nextAddedObjectId = nextAddedId;
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return true;
}
