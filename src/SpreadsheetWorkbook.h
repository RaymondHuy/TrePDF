#pragma once

#include <QString>
#include <QVector>

#include <memory>

// Values are deliberately text: PDF extraction cannot reliably distinguish
// identifiers, dates, localized numbers, or formulas from their visible text.
struct SpreadsheetCell
{
    int column = 0;
    QString text;
};

struct SpreadsheetRow
{
    QVector<SpreadsheetCell> cells;
};

struct SpreadsheetMerge
{
    int firstRow = 0;
    int firstColumn = 0;
    int lastRow = 0;
    int lastColumn = 0;
};

struct SpreadsheetSheet
{
    QString name;
    // Widths use spreadsheet character units, independent of PDF geometry.
    QVector<double> columnWidths;
    QVector<SpreadsheetRow> rows;
    // Inclusive, zero-based rectangles. Ranges cannot overlap; their text lives
    // only in the top-left cell. Covered cells may be absent or blank.
    QVector<SpreadsheetMerge> mergedRanges;
};

// One instance belongs to one export worker and one output file. The caller
// supplies an exclusively owned staging path and atomically commits it only
// after finish() succeeds. Destruction before finish discards temporary data.
// This boundary contains no PDF or spreadsheet-library types; replacing the
// backend requires a new implementation and changing the factory below.
class SpreadsheetWorkbook
{
public:
    static constexpr int MaximumRows = 1048576;
    static constexpr int MaximumColumns = 16384;
    static constexpr int MaximumCellTextLength = 32767;
    static constexpr int MaximumSheets = 1024;
    // Backend memory budgets include blank cells materialized by merged ranges.
    static constexpr int MaximumWorkbookCells = 250000;
    static constexpr qint64 MaximumWorkbookTextBytes = 32 * 1024 * 1024;
    static constexpr int MaximumMergedRanges = 16384;

    virtual ~SpreadsheetWorkbook() = default;

    virtual bool open(const QString &stagingPath, QString &error) = 0;
    // Row order is preserved. Cell columns must be unique within each row.
    virtual bool writeSheet(const SpreadsheetSheet &sheet, QString &error) = 0;
    virtual bool finish(QString &error) = 0;
};

std::unique_ptr<SpreadsheetWorkbook> createSpreadsheetWorkbook();
