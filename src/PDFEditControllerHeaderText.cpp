#include "PDFEditController.h"
#include "ApplicationFonts.h"

#include <QFontInfo>
#include <QRegularExpression>
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

enum class PageParseResult { Valid, Invalid, TooLarge };

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

    const QVariant sizeValue = format.value(QStringLiteral("fontSize"), kHeaderFontSize);
    bool validSize = false;
    const qreal fontSize = sizeValue.toDouble(&validSize);
    const QVariant familyValue = format.value(QStringLiteral("fontFamily"), headerTextFontFamily());
    const QString family = familyValue.toString().trimmed();
    const QVariant colorValue = format.value(QStringLiteral("color"), QColor(Qt::black));
    const QColor color = colorValue.value<QColor>();
    const auto validStyleValue = [&format](const QString &key) {
        return !format.contains(key) || format.value(key).metaType().id() == QMetaType::Bool;
    };
    if (!validSize || sizeValue.metaType().id() == QMetaType::Bool
        || !std::isfinite(fontSize) || fontSize < 1.0 || fontSize > 512.0
        || std::floor(fontSize) != fontSize
        || familyValue.metaType().id() != QMetaType::QString
        || family.isEmpty() || family.size() > 256
        || !color.isValid() || color.alpha() != 255
        || !validStyleValue(QStringLiteral("fontBold"))
        || !validStyleValue(QStringLiteral("fontItalic"))
        || !validStyleValue(QStringLiteral("fontUnderline"))) {
        //% "Choose a valid font, a whole-number size from 1 to 512 pt, and an opaque text color."
        emit statusMessage(qtTrId("HeaderTextInvalidFormat"));
        return false;
    }
    const bool bold = format.value(QStringLiteral("fontBold"), false).toBool();
    const bool italic = format.value(QStringLiteral("fontItalic"), false).toBool();
    const bool underline = format.value(QStringLiteral("fontUnderline"), false).toBool();
    if (!ApplicationFonts::isFontFaceSelectable(family, bold, italic)
        || !ApplicationFonts::fontFaceSupportsText(family, bold, italic, normalizedText)) {
        //% "The selected header font or style is unavailable or does not support this text. Choose another font."
        emit statusMessage(qtTrId("HeaderTextFontUnavailable"));
        return false;
    }
    const QFont layoutFont = fontForDisplay(family, 100, bold, italic, underline, fontSize);

    QList<int> selectedPages;
    const PageParseResult parsed = headerPages(pages, m_document->pageCount(), selectedPages);
    if (parsed == PageParseResult::Invalid) {
        //% "Enter valid page numbers or ranges, such as 1-3, 5."
        emit statusMessage(qtTrId("HeaderTextInvalidPages"));
        return false;
    }
    if (parsed == PageParseResult::TooLarge
        || selectedPages.size() > kMaximumHeaderCharacterWork / normalizedText.size()
        || m_nextAddedObjectId > std::numeric_limits<quint64>::max()
            - static_cast<quint64>(selectedPages.size())) {
        //% "This header batch is too large. Apply it to fewer pages."
        emit statusMessage(qtTrId("HeaderTextBatchTooLarge"));
        return false;
    }

    Candidate prototype;
    prototype.kind = QStringLiteral("addedText");
    //% "Add Text"
    prototype.label = qtTrId("AddText");
    prototype.fontFamily = family;
    prototype.fontBold = bold;
    prototype.fontItalic = italic;
    prototype.fontUnderline = underline;
    prototype.color = color;
    prototype.fillColor = color;
    prototype.added = true;
    prototype.editable = true;

    Command command;
    command.kind = Command::Kind::ObjectDraftBatch;
    command.addedDrafts.reserve(selectedPages.size());
    quint64 nextAddedId = m_nextAddedObjectId;
    QHash<qreal, qreal> heightByWidth;
    for (int page : std::as_const(selectedPages)) {
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
            const auto cached = heightByWidth.constFind(width);
            height = cached == heightByWidth.cend()
                ? headerLayoutHeight(normalizedText, layoutFont, width,
                                     kHeaderHeight - yPoints)
                : *cached;
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
