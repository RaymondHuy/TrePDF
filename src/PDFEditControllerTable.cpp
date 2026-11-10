#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFTableCellLayout.h"

#include <QJsonDocument>
#include <QTextBoundaryFinder>
#include <QUuid>
#include <QtTranslation>
#include <utility>

using namespace PDFEditControllerPrivate;

QVector<QRectF> PDFEditController::tableCellRects(const Draft &draft) const
{
    if (!m_document || !isShapeCandidateKind(draft.candidate.kind)
        || draft.candidate.shapeType != TableShape)
        return {};
    return PDFTableCellLayout::cellRects(draft.frameRect, m_document->pageSize(draft.candidate.page),
        draft.candidate.shapeStrokeWidthRatio, draft.candidate.tableRows, draft.candidate.tableColumns,
        draft.tableRowStops, draft.tableColumnStops);
}

bool PDFEditController::tableCellTextFits(const Draft &draft) const
{
    if (draft.tableCellTexts.isEmpty())
        return true;
    if (!draft.tableCellFormats.isEmpty() && draft.tableCellFormats.size() != draft.tableCellTexts.size())
        return false;
    const auto rects = tableCellRects(draft);
    if (rects.size() != draft.tableCellTexts.size())
        return false;
    const QSizeF pageSize = m_document->pageSize(draft.candidate.page);
    const QFont font = fontForDisplay(draft.tableFontFamily, 100, false, false, false, draft.tableFontSize);
    for (qsizetype index = 0; index < rects.size(); ++index) {
        const QSizeF size(rects.at(index).width() * pageSize.width(),
                          rects.at(index).height() * pageSize.height());
        if (!PDFTableCellLayout::fits(draft.tableCellTexts.at(index), font, size,
                draft.tableCellFormats.isEmpty() ? PDFTableCellFormat::Cell{} : draft.tableCellFormats.at(index)))
            return false;
    }
    return true;
}

namespace {
using namespace PDFTableCellFormat;

void appendSpan(Cell &cell, int start, int length, const Style &style)
{
    if (length <= 0)
        return;
    if (!cell.spans.isEmpty() && cell.spans.back().style == style)
        cell.spans.back().length += length;
    else
        cell.spans.append({start, length, style});
}

// NFC can combine multiple UTF-16 units into one. Keep formatting aligned to
// grapheme boundaries instead of retaining offsets into the pre-normalized text.
QString normalizedCell(const QString &text, Cell &format)
{
    QString normalized;
    Cell result{format.defaultStyle, {}};
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    int start = 0;
    for (int end = boundaries.toNextBoundary(); end >= 0; end = boundaries.toNextBoundary()) {
        QString part = text.sliced(start, end - start);
        part.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        part.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        part.replace(QChar::LineSeparator, QLatin1Char('\n'));
        part.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        part = part.normalized(QString::NormalizationForm_C);
        if (part == text.sliced(start, end - start)) {
            for (const auto &span : format.spans) {
                const int from = qMax(start, span.start);
                const int to = qMin(end, span.start + span.length);
                if (to > from)
                    appendSpan(result, int(normalized.size()) + from - start, to - from, span.style);
            }
        } else {
            appendSpan(result, int(normalized.size()), int(part.size()), styleAt(format, start));
        }
        normalized += part;
        start = end;
    }
    format = std::move(result);
    return normalized;
}

bool supports(const Style &style, const QString &text)
{
    return ApplicationFonts::isFontFaceSelectable(style.family, style.bold, style.italic)
        && ApplicationFonts::fontFaceSupportsText(style.family, style.bold, style.italic, text);
}
} // namespace

QString PDFEditController::setSelectedTableCellText(int row, int column, const QString &text)
{
    return setSelectedTableCellContent(row, column, text, {});
}

QString PDFEditController::setSelectedTableCellContent(int row, int column, const QString &text,
                                                       const QVariantMap &formatMap)
{
    if (m_saving || m_frameResizeActive || m_textEditActive || !m_document || !m_hasSelection
        || !m_selectedCandidate.editable || m_selectedCandidate.tableFrameOnly || !documentAllowsEditing()
        || !isShapeCandidateKind(m_selectedCandidate.kind) || m_selectedCandidate.shapeType != TableShape) {
        //% "Select an editable table before editing a cell."
        return qtTrId("TableCellNotEditable");
    }
    const Draft before = selectedDraftForDisplay();
    const int rows = before.candidate.tableRows;
    const int columns = before.candidate.tableColumns;
    if (row < 0 || row >= rows || column < 0 || column >= columns) {
        //% "This cell is no longer available. Select the table again."
        return qtTrId("TableCellUnavailable");
    }
    if (text.size() > PDFTableCellLayout::MaximumCellCharacters * 2 || !text.isValidUtf16()) {
        //% "Use at most 1,024 characters per cell, without unsupported control characters."
        return qtTrId("TableCellInvalidText");
    }
    const int index = row * columns + column;
    const QString family = before.tableFontFamily.isEmpty() ? headerTextFontFamily() : before.tableFontFamily;
    const Style fallback{family};
    const Cell previous = before.tableCellFormats.isEmpty()
        ? plain(before.tableCellTexts.isEmpty() ? QString() : before.tableCellTexts.at(index), fallback)
        : before.tableCellFormats.at(index);
    Cell format = plain(text, previous.defaultStyle);
    if (!formatMap.isEmpty() && !fromVariant(formatMap, text, format)) {
        //% "The cell formatting is invalid or too complex. Use fewer font or style changes."
        return qtTrId("TableCellInvalidFormat");
    }
    if (formatMap.isEmpty() && !before.tableCellTexts.isEmpty() && text == before.tableCellTexts.at(index))
        format = previous;
    const QString normalized = normalizedCell(text, format);
    if (!PDFTableCellLayout::validText(normalized)) {
        //% "Use at most 1,024 characters per cell, without unsupported control characters."
        return qtTrId("TableCellInvalidText");
    }
    Draft after = before;
    if (after.tableCellTexts.isEmpty())
        after.tableCellTexts.resize(rows * columns);
    after.tableCellTexts[index] = normalized;
    if (after.tableCellFormats.isEmpty()) {
        after.tableCellFormats.reserve(rows * columns);
        for (const auto &cell : std::as_const(after.tableCellTexts))
            after.tableCellFormats.append(plain(cell, fallback));
    }
    after.tableCellFormats[index] = format;
    qsizetype total = 0;
    int spanCount = 0;
    bool allPlain = true;
    QJsonArray serializedFormats;
    for (int cell = 0; cell < after.tableCellTexts.size(); ++cell) {
        const auto &content = after.tableCellTexts.at(cell);
        const auto &cellFormat = after.tableCellFormats.at(cell);
        total += content.size();
        spanCount += int(cellFormat.spans.size());
        allPlain = allPlain && cellFormat == plain(content, fallback);
        serializedFormats.append(toJson(cellFormat));
    }
    if (total > PDFTableCellLayout::MaximumTableCharacters) {
        //% "This table supports up to 8,192 characters. Shorten the cell text."
        return qtTrId("TableTextTooLong");
    }
    if (spanCount > MaximumSpansPerTable || !valid(format, normalized)
        || QJsonDocument(QJsonObject{{QStringLiteral("cells"), QJsonArray::fromStringList(after.tableCellTexts)},
                                     {QStringLiteral("cellFormats"), serializedFormats}})
               .toJson(QJsonDocument::Compact).size() > 112 * 1024) {
        //% "The cell formatting is invalid or too complex. Use fewer font or style changes."
        return qtTrId("TableCellInvalidFormat");
    }
    if (allPlain)
        after.tableCellFormats.clear();
    if (total == 0 && after.tableCellFormats.isEmpty()) {
        after.tableCellTexts.clear();
        after.tableId = after.candidate.tableId;
        after.tableFontFamily = after.candidate.tableFontFamily;
        after.tableFontSize = after.candidate.tableFontSize;
    } else {
        after.tableFontFamily = family;
        if (after.tableId.isEmpty())
            after.tableId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (sameDraftContent(before, after))
        return {};
    if (!after.tableCellTexts.isEmpty()) {
        bool supported = supports(format.defaultStyle, QString());
        for (const auto &span : format.spans)
            supported = supported && supports(span.style, normalized.sliced(span.start, span.length));
        if (!supported) {
            //% "The table font is unavailable or does not support this text. Install the font in Settings > Font."
            return qtTrId("TableCellFontUnavailable");
        }
        if (!tableCellTextFits(after)) {
            //% "The text does not fit in this cell. Shorten it or enlarge the row or column."
            return qtTrId("TableCellTextDoesNotFit");
        }
    }
    Command command;
    command.key = draftKey(before.candidate.page, before.candidate.objectId);
    command.page = before.candidate.page;
    command.objectId = before.candidate.objectId;
    command.hadBefore = m_drafts.contains(command.key);
    command.before = before;
    command.after = after;
    command.hadAfter = before.candidate.added
        || !sameDraftContent(after, initialDraftForCandidate(before.candidate));
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return {};
}
