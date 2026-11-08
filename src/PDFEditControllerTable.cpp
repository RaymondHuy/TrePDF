#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFTableCellLayout.h"

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
    const auto rects = tableCellRects(draft);
    if (rects.size() != draft.tableCellTexts.size())
        return false;
    const QSizeF pageSize = m_document->pageSize(draft.candidate.page);
    const QFont font = fontForDisplay(draft.tableFontFamily, 100, false, false, false, draft.tableFontSize);
    for (qsizetype index = 0; index < rects.size(); ++index) {
        const QSizeF size(rects.at(index).width() * pageSize.width(),
                          rects.at(index).height() * pageSize.height());
        if (!PDFTableCellLayout::fits(draft.tableCellTexts.at(index), font, size))
            return false;
    }
    return true;
}

QString PDFEditController::setSelectedTableCellText(int row, int column, const QString &text)
{
    if (m_saving || m_frameResizeActive || m_textEditActive || !m_document || !m_hasSelection
        || !m_selectedCandidate.editable || !documentAllowsEditing()
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
    QString normalized = text;
    if (normalized.size() <= PDFTableCellLayout::MaximumCellCharacters * 2) {
        normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        normalized.replace(QChar::LineSeparator, QLatin1Char('\n'));
        normalized.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        normalized = normalized.normalized(QString::NormalizationForm_C);
    }
    if (!PDFTableCellLayout::validText(normalized)) {
        //% "Use at most 1,024 characters per cell, without unsupported control characters."
        return qtTrId("TableCellInvalidText");
    }
    Draft after = before;
    const int index = row * columns + column;
    if ((after.tableCellTexts.isEmpty() && normalized.isEmpty())
        || (!after.tableCellTexts.isEmpty() && after.tableCellTexts.at(index) == normalized))
        return {};
    if (after.tableCellTexts.isEmpty())
        after.tableCellTexts.resize(rows * columns);
    after.tableCellTexts[index] = normalized;
    qsizetype total = 0;
    for (const QString &cell : std::as_const(after.tableCellTexts))
        total += cell.size();
    if (total > PDFTableCellLayout::MaximumTableCharacters) {
        //% "This table supports up to 8,192 characters. Shorten the cell text."
        return qtTrId("TableTextTooLong");
    }
    if (total == 0) {
        after.tableCellTexts.clear();
        // A cleared source group still owns its saved children until Save.
        after.tableId = after.candidate.tableId;
        after.tableFontFamily = after.candidate.tableFontFamily;
        after.tableFontSize = after.candidate.tableFontSize;
    } else {
        if (after.tableFontFamily.isEmpty())
            after.tableFontFamily = headerTextFontFamily();
        if (after.tableId.isEmpty())
            after.tableId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!ApplicationFonts::isFontFaceSelectable(after.tableFontFamily, false, false)
            || !ApplicationFonts::fontFaceSupportsText(after.tableFontFamily, false, false, normalized)) {
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
