#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFTableCellLayout.h"
#include "PDFShapeUtils.h"

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
        draft.candidate.shapeStrokeWidthRatio, draft.tableRows, draft.tableColumns,
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
    Cell result{format.defaultStyle, {}, format.rotation};
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
    const int rows = before.tableRows;
    const int columns = before.tableColumns;
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
    format.rotation = previous.rotation;
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

QString PDFEditController::insertSelectedTableRow(int beforeRow)
{
    return insertSelectedTableRows(beforeRow, 1);
}

QString PDFEditController::insertSelectedTableColumn(int beforeColumn)
{
    return insertSelectedTableColumns(beforeColumn, 1);
}

QString PDFEditController::insertSelectedTableRows(int beforeRow, int count)
{
    return insertSelectedTableTrack(false, beforeRow, count);
}

QString PDFEditController::insertSelectedTableColumns(int beforeColumn, int count)
{
    return insertSelectedTableTrack(true, beforeColumn, count);
}

QString PDFEditController::removeSelectedTableRow()
{
    return removeSelectedTableTrack(false, {}, true);
}

QString PDFEditController::removeSelectedTableColumn()
{
    return removeSelectedTableTrack(true, {}, true);
}

QString PDFEditController::removeSelectedTableRows(const QString &indices)
{
    return removeSelectedTableTrack(false, indices);
}

QString PDFEditController::removeSelectedTableColumns(const QString &indices)
{
    return removeSelectedTableTrack(true, indices);
}

QString PDFEditController::removeSelectedTableTrack(bool column, const QString &indices, bool lastOnly)
{
    if (m_saving || m_frameResizeActive || m_textEditActive || !m_document || !m_hasSelection
        || !m_selectedCandidate.editable || !documentAllowsEditing()
        || !isShapeCandidateKind(m_selectedCandidate.kind) || m_selectedCandidate.shapeType != TableShape)
        return qtTrId("TableCellNotEditable");

    const Draft before = selectedDraftForDisplay();
    const int dimension = column ? before.tableColumns : before.tableRows;
    if (before.deleted || before.tableRows < 1 || before.tableColumns < 1
        || before.tableRows > MaximumTableDimension || before.tableColumns > MaximumTableDimension)
        return qtTrId("TableCellUnavailable");
    QVector<bool> removed(dimension, false);
    if (lastOnly) {
        removed[dimension - 1] = true;
    } else {
        //% "Enter valid row or column numbers separated by commas, for example 1,3,5."
        const QString invalidIndices = qtTrId("TableRemoveInvalidIndices");
        if (indices.isEmpty() || indices.size() > 256)
            return invalidIndices;
        const auto parts = indices.split(QLatin1Char(','));
        for (const auto &part : parts) {
            const QString token = part.trimmed();
            if (token.isEmpty() || !std::all_of(token.cbegin(), token.cend(), [](QChar c) {
                    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
                }))
                return invalidIndices;
            bool ok = false;
            const int index = token.toInt(&ok);
            if (!ok || index < 1 || index > dimension)
                return invalidIndices;
            removed[index - 1] = true;
        }
    }
    QVector<int> retained;
    for (int track = 0; track < dimension; ++track) {
        if (!removed.at(track))
            retained.append(track);
    }
    if (retained.isEmpty()) {
        //% "Keep at least one row and one column. To remove the whole table, select it and press Delete."
        return qtTrId("TableRemoveMinimumDimension");
    }

    const QSizeF pageSize = m_document->pageSize(before.candidate.page);
    if (pageSize.isEmpty() || !std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height()))
        return qtTrId("TableCellUnavailable");
    const qreal stroke = qMax<qreal>(.75, before.candidate.shapeStrokeWidthRatio * pageSize.height());
    const QSizeF physicalSize(before.frameRect.width() * pageSize.width(),
                              before.frameRect.height() * pageSize.height());
    const qreal inset = std::min({stroke / 2, physicalSize.width() / 4, physicalSize.height() / 4});
    const qreal extent = (column ? physicalSize.width() : physicalSize.height()) - 2 * inset;
    const auto &oldStops = column ? before.tableColumnStops : before.tableRowStops;
    if (!std::isfinite(extent) || extent <= 0 || !PDFShapeUtils::validTableStops(oldStops, dimension))
        return qtTrId("TableCellInvalidFormat");

    // Compact surviving tracks in their original order while retaining each
    // physical extent. Requested indices always refer to the original table.
    QVector<qreal> lengths;
    qreal retainedExtent = 0;
    for (const int track : retained) {
        const qreal start = track == 0 ? 0.0 : PDFShapeUtils::tableStop(oldStops, dimension, track);
        const qreal end = track == dimension - 1 ? 1.0
            : PDFShapeUtils::tableStop(oldStops, dimension, track + 1);
        const qreal length = (end - start) * extent;
        lengths.append(length);
        retainedExtent += length;
    }
    Draft after = before;
    if (column) {
        after.tableColumns = int(retained.size());
        after.frameRect.setWidth((retainedExtent + 2 * inset) / pageSize.width());
    } else {
        after.tableRows = int(retained.size());
        after.frameRect.setHeight((retainedExtent + 2 * inset) / pageSize.height());
    }
    const qreal newInset = std::min({stroke / 2, after.frameRect.width() * pageSize.width() / 4,
                                     after.frameRect.height() * pageSize.height() / 4});
    if (!std::isfinite(retainedExtent) || retainedExtent <= 0 || std::abs(newInset - inset) > .000001) {
        //% "The remaining table would be too small. Enlarge its rows or columns first."
        return qtTrId("TableRemoveTooSmall");
    }
    after.contentBounds = after.frameRect;
    auto &newStops = column ? after.tableColumnStops : after.tableRowStops;
    newStops.clear();
    qreal offset = 0;
    for (int track = 0; track < lengths.size() - 1; ++track) {
        offset += lengths.at(track);
        newStops.append(offset / retainedExtent);
    }
    if (!PDFShapeUtils::validTableStops(newStops, int(retained.size())))
        return qtTrId("TableCellInvalidFormat");

    const int oldCount = before.tableRows * before.tableColumns;
    if ((!before.tableCellTexts.isEmpty() && before.tableCellTexts.size() != oldCount)
        || (!before.tableCellFormats.isEmpty()
            && (before.tableCellFormats.size() != oldCount || before.tableCellTexts.isEmpty())))
        return qtTrId("TableCellInvalidFormat");
    if (!before.tableCellTexts.isEmpty()) {
        after.tableCellTexts.clear();
        after.tableCellFormats.clear();
        after.tableCellTexts.reserve(after.tableRows * after.tableColumns);
        if (!before.tableCellFormats.isEmpty())
            after.tableCellFormats.reserve(after.tableRows * after.tableColumns);
        for (int row = 0; row < after.tableRows; ++row) {
            for (int col = 0; col < after.tableColumns; ++col) {
                const int oldRow = column ? row : retained.at(row);
                const int oldColumn = column ? retained.at(col) : col;
                const int oldIndex = oldRow * before.tableColumns + oldColumn;
                after.tableCellTexts.append(before.tableCellTexts.at(oldIndex));
                if (!before.tableCellFormats.isEmpty())
                    after.tableCellFormats.append(before.tableCellFormats.at(oldIndex));
            }
        }
    }
    if (!tableCellTextFits(after))
        return qtTrId("TableCellTextDoesNotFit");

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

QString PDFEditController::insertSelectedTableTrack(bool column, int beforeIndex, int count)
{
    if (m_saving || m_frameResizeActive || m_textEditActive || !m_document || !m_hasSelection
        || !m_selectedCandidate.editable || !documentAllowsEditing()
        || !isShapeCandidateKind(m_selectedCandidate.kind) || m_selectedCandidate.shapeType != TableShape)
        return qtTrId("TableCellNotEditable");

    const Draft before = selectedDraftForDisplay();
    const int dimension = column ? before.tableColumns : before.tableRows;
    if (before.deleted || before.tableRows < 1 || before.tableColumns < 1
        || before.tableRows > MaximumTableDimension || before.tableColumns > MaximumTableDimension
        || beforeIndex < 0 || beforeIndex > dimension)
        return qtTrId("TableCellUnavailable");
    if (count < 1 || count > MaximumTableDimension) {
        //% "Enter a whole-number quantity from 1 to 20."
        return qtTrId("TableInsertInvalidCount");
    }
    if (count > MaximumTableDimension - dimension) {
        //% "A table supports at most 20 rows and 20 columns."
        return qtTrId("TableInsertDimensionLimit");
    }

    const QSizeF pageSize = m_document->pageSize(before.candidate.page);
    if (pageSize.isEmpty() || !std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height()))
        return qtTrId("TableCellUnavailable");
    const qreal stroke = qMax<qreal>(.75, before.candidate.shapeStrokeWidthRatio * pageSize.height());
    const QSizeF physicalSize(before.frameRect.width() * pageSize.width(),
                              before.frameRect.height() * pageSize.height());
    const qreal inset = std::min({stroke / 2, physicalSize.width() / 4, physicalSize.height() / 4});
    const qreal extent = (column ? physicalSize.width() : physicalSize.height()) - 2 * inset;
    const auto &oldStops = column ? before.tableColumnStops : before.tableRowStops;
    if (!std::isfinite(extent) || extent <= 0 || !PDFShapeUtils::validTableStops(oldStops, dimension))
        return qtTrId("TableCellInvalidFormat");

    QVector<qreal> lengths;
    lengths.reserve(dimension + count);
    qreal previous = 0;
    for (int track = 0; track < dimension; ++track) {
        const qreal end = track + 1 == dimension ? 1.0
            : PDFShapeUtils::tableStop(oldStops, dimension, track + 1);
        lengths.append((end - previous) * extent);
        previous = end;
    }
    const qreal insertedLength = lengths.at(qMin(beforeIndex, dimension - 1));
    const qreal extra = insertedLength * count;
    lengths.insert(beforeIndex, count, insertedLength);
    Draft after = before;
    if (column) {
        after.tableColumns += count;
        after.frameRect.setWidth(before.frameRect.width() + extra / pageSize.width());
    } else {
        after.tableRows += count;
        after.frameRect.setHeight(before.frameRect.height() + extra / pageSize.height());
    }
    const qreal newInset = std::min({stroke / 2, after.frameRect.width() * pageSize.width() / 4,
                                     after.frameRect.height() * pageSize.height() / 4});
    if (after.frameRect.left() < 0 || after.frameRect.top() < selectionTopRatio(before.candidate.page)
        || after.frameRect.right() > 1.0
        || after.frameRect.bottom() > selectionBottomRatio(before.candidate.page)
        || std::abs(newInset - inset) > .000001) {
        //% "There is not enough space on this page to add a row or column. Move or resize the table first."
        return qtTrId("TableInsertOutsidePage");
    }
    after.contentBounds = after.frameRect;
    auto &newStops = column ? after.tableColumnStops : after.tableRowStops;
    newStops.clear();
    qreal offset = 0;
    for (int track = 0; track < dimension + count - 1; ++track) {
        offset += lengths[track];
        newStops.append(offset / (extent + extra));
    }
    if (!PDFShapeUtils::validTableStops(newStops, dimension + count))
        return qtTrId("TableCellInvalidFormat");

    // Source candidate metadata stays immutable for Save validation. Only draft
    // dimensions and row-major cells change; new cells inherit a neighbor's
    // default style and rotation without copying its text or character spans.
    const int oldCount = before.tableRows * before.tableColumns;
    if ((!before.tableCellTexts.isEmpty() && before.tableCellTexts.size() != oldCount)
        || (!before.tableCellFormats.isEmpty() && before.tableCellFormats.size() != oldCount))
        return qtTrId("TableCellInvalidFormat");
    if (!before.tableCellTexts.isEmpty()) {
        after.tableCellTexts.clear();
        after.tableCellFormats.clear();
        const int newCount = after.tableRows * after.tableColumns;
        after.tableCellTexts.reserve(newCount);
        if (!before.tableCellFormats.isEmpty())
            after.tableCellFormats.reserve(newCount);
        for (int row = 0; row < after.tableRows; ++row) {
            for (int col = 0; col < after.tableColumns; ++col) {
                const int track = column ? col : row;
                const bool inserted = track >= beforeIndex && track < beforeIndex + count;
                const int oldTrack = inserted ? qMin(beforeIndex, dimension - 1)
                    : track >= beforeIndex + count ? track - count : track;
                const int oldRow = column ? row : oldTrack;
                const int oldColumn = column ? oldTrack : col;
                const int oldIndex = oldRow * before.tableColumns + oldColumn;
                after.tableCellTexts.append(inserted ? QString() : before.tableCellTexts.at(oldIndex));
                if (!before.tableCellFormats.isEmpty()) {
                    Cell format = before.tableCellFormats.at(oldIndex);
                    if (inserted)
                        format.spans.clear();
                    after.tableCellFormats.append(std::move(format));
                }
            }
        }
    }
    QJsonArray formats;
    for (const auto &format : std::as_const(after.tableCellFormats))
        formats.append(toJson(format));
    if (QJsonDocument(QJsonObject{{QStringLiteral("cells"), QJsonArray::fromStringList(after.tableCellTexts)},
                                  {QStringLiteral("cellFormats"), formats}})
            .toJson(QJsonDocument::Compact).size() > 112 * 1024)
        return qtTrId("TableCellInvalidFormat");
    if (!tableCellTextFits(after))
        return qtTrId("TableCellTextDoesNotFit");

    Command command;
    command.key = draftKey(before.candidate.page, before.candidate.objectId);
    command.page = before.candidate.page;
    command.objectId = before.candidate.objectId;
    command.hadBefore = m_drafts.contains(command.key);
    command.before = before;
    command.after = after;
    command.hadAfter = true;
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    return {};
}
