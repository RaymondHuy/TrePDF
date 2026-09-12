#include "SpreadsheetWorkbook.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <xlsxwriter.h>

#include <cmath>
#include <utility>

namespace {

bool validText(const QString &text)
{
    // Embedded NUL would silently truncate the C string. Invalid UTF-16 and
    // XML noncharacters must also fail instead of silently changing the data.
    return text.isValidUtf16()
        && !text.contains(QChar::Null)
        && !text.contains(QChar(0xfffe))
        && !text.contains(QChar(0xffff));
}

QString libraryError(const QString &operation, lxw_error code)
{
    return QStringLiteral("%1: %2").arg(
        operation, QString::fromUtf8(lxw_strerror(code)));
}

class LibxlsxwriterWorkbook final : public SpreadsheetWorkbook
{
public:
    ~LibxlsxwriterWorkbook() override
    {
        discard();
    }

    bool open(const QString &stagingPath, QString &error) override
    {
        error.clear();
        if (m_started) {
            return fail(QStringLiteral("A workbook can only be opened once."), error);
        }
        m_started = true;
        if (stagingPath.isEmpty() || !validText(stagingPath)) {
            return fail(QStringLiteral("The workbook staging path is invalid."), error);
        }

        m_stagingPath = stagingPath;
        const QByteArray encodedPath = QFile::encodeName(stagingPath);
        // Package files stay next to the staging output, including on systems
        // where the default temporary directory is backed by RAM.
        const QByteArray encodedDirectory = QFile::encodeName(
            QFileInfo(stagingPath).absolutePath());
        lxw_workbook_options options = {};
        // libxlsxwriter does not support general merged ranges in its streaming
        // mode. Explicit aggregate cell/text limits below bound normal mode.
        options.constant_memory = LXW_FALSE;
        options.tmpdir = const_cast<char *>(encodedDirectory.constData());
        m_workbook = workbook_new_opt(encodedPath.constData(), &options);
        if (!m_workbook) {
            return fail(QStringLiteral("Could not allocate the Excel workbook."), error);
        }

        m_cellFormat = workbook_add_format(m_workbook);
        if (!m_cellFormat) {
            return fail(QStringLiteral("Could not allocate the cell format."), error);
        }
        format_set_text_wrap(m_cellFormat);
        format_set_align(m_cellFormat, LXW_ALIGN_VERTICAL_TOP);
        format_set_border(m_cellFormat, LXW_BORDER_THIN);
        format_set_border_color(m_cellFormat, 0xd9d9d9);
        return true;
    }

    bool writeSheet(const SpreadsheetSheet &sheet, QString &error) override
    {
        error.clear();
        if (!m_workbook) {
            return fail(QStringLiteral("The workbook is not open."), error);
        }
        if (m_sheetNames.size() >= MaximumSheets) {
            return fail(QStringLiteral("The workbook exceeds the worksheet limit."), error);
        }
        if (sheet.name.isEmpty() || !validText(sheet.name)
            || sheet.name.size() > 31
            || sheet.name.compare(QStringLiteral("History"), Qt::CaseInsensitive) == 0
            || m_sheetNames.contains(sheet.name.toCaseFolded())) {
            return fail(QStringLiteral("The worksheet name is invalid or duplicated."), error);
        }
        if (sheet.rows.isEmpty() || sheet.rows.size() > MaximumRows
            || sheet.columnWidths.size() > MaximumColumns
            || sheet.mergedRanges.size() > MaximumMergedRanges) {
            return fail(QStringLiteral("The worksheet dimensions are invalid."), error);
        }

        auto cellKey = [](int row, int column) {
            return (static_cast<quint64>(row) << 32) | static_cast<quint32>(column);
        };
        // The map is bounded by the workbook cell budget and validates arbitrary
        // rectangular merges without quadratic range-overlap comparisons.
        QHash<quint64, qsizetype> mergedCells;
        qint64 sheetCellCount = 0;
        qint64 sheetTextBytes = 0;
        for (qsizetype index = 0; index < sheet.mergedRanges.size(); ++index) {
            const SpreadsheetMerge &merge = sheet.mergedRanges.at(index);
            if (merge.firstRow < 0 || merge.firstColumn < 0
                || merge.lastRow < merge.firstRow || merge.lastColumn < merge.firstColumn
                || merge.lastRow >= sheet.rows.size() || merge.lastColumn >= MaximumColumns
                || (merge.firstRow == merge.lastRow && merge.firstColumn == merge.lastColumn)) {
                return fail(QStringLiteral("A merged range has invalid coordinates."), error);
            }
            const qint64 area = (qint64(merge.lastRow) - merge.firstRow + 1)
                * (qint64(merge.lastColumn) - merge.firstColumn + 1);
            if (area > MaximumWorkbookCells - m_cellCount - sheetCellCount) {
                return fail(QStringLiteral("The workbook exceeds its cell memory limit."), error);
            }
            sheetCellCount += area;
            for (int row = merge.firstRow; row <= merge.lastRow; ++row) {
                for (int column = merge.firstColumn; column <= merge.lastColumn; ++column) {
                    const quint64 key = cellKey(row, column);
                    if (mergedCells.contains(key)) {
                        return fail(QStringLiteral("Merged worksheet ranges overlap."), error);
                    }
                    mergedCells.insert(key, index);
                }
            }
        }
        for (qsizetype rowIndex = 0; rowIndex < sheet.rows.size(); ++rowIndex) {
            const SpreadsheetRow &row = sheet.rows.at(rowIndex);
            if (row.cells.size() > MaximumColumns) {
                return fail(QStringLiteral("A worksheet row has too many cells."), error);
            }
            QSet<int> columns;
            for (const SpreadsheetCell &cell : row.cells) {
                if (cell.column < 0 || cell.column >= MaximumColumns
                    || cell.text.size() > MaximumCellTextLength || !validText(cell.text)) {
                    return fail(QStringLiteral("A worksheet cell has invalid coordinates or text."), error);
                }
                if (columns.contains(cell.column)) {
                    return fail(QStringLiteral("A worksheet row contains duplicate columns."), error);
                }
                columns.insert(cell.column);
                const auto merged = mergedCells.constFind(cellKey(int(rowIndex), cell.column));
                if (merged != mergedCells.cend()) {
                    const SpreadsheetMerge &merge = sheet.mergedRanges.at(merged.value());
                    if ((rowIndex != merge.firstRow || cell.column != merge.firstColumn)
                        && !cell.text.isEmpty()) {
                        return fail(QStringLiteral("A merged range would hide nonempty cell text."), error);
                    }
                } else {
                    ++sheetCellCount;
                }
                sheetTextBytes += cell.text.toUtf8().size();
                if (sheetCellCount > MaximumWorkbookCells - m_cellCount
                    || sheetTextBytes > MaximumWorkbookTextBytes - m_textBytes) {
                    return fail(QStringLiteral("The workbook exceeds its cell or text memory limit."), error);
                }
            }
        }
        for (double width : sheet.columnWidths) {
            if (!std::isfinite(width) || width <= 0.0 || width > 255.0) {
                return fail(QStringLiteral("A worksheet column width is invalid."), error);
            }
        }

        const QByteArray encodedName = sheet.name.toUtf8();
        lxw_error result = workbook_validate_sheet_name(m_workbook, encodedName.constData());
        if (result != LXW_NO_ERROR) {
            return fail(libraryError(QStringLiteral("Invalid worksheet name"), result), error);
        }
        lxw_worksheet *worksheet = workbook_add_worksheet(m_workbook, encodedName.constData());
        if (!worksheet) {
            return fail(QStringLiteral("Could not allocate the worksheet."), error);
        }

        for (qsizetype column = 0; column < sheet.columnWidths.size(); ++column) {
            const double width = sheet.columnWidths.at(column);
            result = worksheet_set_column(worksheet,
                static_cast<lxw_col_t>(column), static_cast<lxw_col_t>(column), width, nullptr);
            if (result != LXW_NO_ERROR) {
                return fail(libraryError(QStringLiteral("Could not set column width"), result), error);
            }
        }

        for (const SpreadsheetMerge &merge : sheet.mergedRanges) {
            // Merge a blank range first, then write the anchor with the same
            // explicit string API as every other cell below.
            result = worksheet_merge_range(worksheet,
                static_cast<lxw_row_t>(merge.firstRow), static_cast<lxw_col_t>(merge.firstColumn),
                static_cast<lxw_row_t>(merge.lastRow), static_cast<lxw_col_t>(merge.lastColumn),
                "", m_cellFormat);
            if (result != LXW_NO_ERROR) {
                return fail(libraryError(QStringLiteral("Could not merge worksheet cells"), result), error);
            }
        }

        for (qsizetype rowIndex = 0; rowIndex < sheet.rows.size(); ++rowIndex) {
            const SpreadsheetRow &row = sheet.rows.at(rowIndex);
            for (const SpreadsheetCell &cell : row.cells) {
                const auto merged = mergedCells.constFind(cellKey(int(rowIndex), cell.column));
                if (merged != mergedCells.cend()) {
                    const SpreadsheetMerge &merge = sheet.mergedRanges.at(merged.value());
                    if (rowIndex != merge.firstRow || cell.column != merge.firstColumn)
                        continue;
                }
                const QByteArray encodedText = cell.text.toUtf8();
                // This explicit string call preserves leading zeros and text
                // beginning with '=', '+', '-', or '@' without formula execution.
                result = worksheet_write_string(worksheet,
                    static_cast<lxw_row_t>(rowIndex),
                    static_cast<lxw_col_t>(cell.column),
                    encodedText.constData(), m_cellFormat);
                if (result != LXW_NO_ERROR) {
                    return fail(libraryError(QStringLiteral("Could not write worksheet cell"), result), error);
                }
            }
        }
        m_sheetNames.insert(sheet.name.toCaseFolded());
        m_cellCount += sheetCellCount;
        m_textBytes += sheetTextBytes;
        return true;
    }

    bool finish(QString &error) override
    {
        error.clear();
        if (!m_workbook || m_sheetNames.isEmpty()) {
            return fail(QStringLiteral("The workbook contains no completed worksheets."), error);
        }
        // workbook_close consumes the handle on both success and failure.
        lxw_workbook *workbook = std::exchange(m_workbook, nullptr);
        m_cellFormat = nullptr;
        const lxw_error result = workbook_close(workbook);
        if (result != LXW_NO_ERROR) {
            return fail(libraryError(QStringLiteral("Could not finish the Excel workbook"), result), error);
        }
        m_stagingPath.clear();
        return true;
    }

private:
    bool fail(const QString &message, QString &error)
    {
        error = message;
        discard();
        return false;
    }

    void discard()
    {
        // The pinned backend exposes a free-only function. Unlike close(), it
        // releases cells without packaging a cancelled/failed workbook.
        if (m_workbook) {
            lxw_workbook_free(std::exchange(m_workbook, nullptr));
        }
        m_cellFormat = nullptr;
        if (!m_stagingPath.isEmpty()) {
            QFile::remove(m_stagingPath);
            m_stagingPath.clear();
        }
    }

    lxw_workbook *m_workbook = nullptr;
    lxw_format *m_cellFormat = nullptr;
    QString m_stagingPath;
    QSet<QString> m_sheetNames;
    qint64 m_cellCount = 0;
    qint64 m_textBytes = 0;
    bool m_started = false;
};

} // namespace

std::unique_ptr<SpreadsheetWorkbook> createSpreadsheetWorkbook()
{
    return std::make_unique<LibxlsxwriterWorkbook>();
}
