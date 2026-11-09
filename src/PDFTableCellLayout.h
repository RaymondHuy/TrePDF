#pragma once

#include "PDFTableCellFormat.h"

#include <QFont>
#include <QRectF>
#include <QString>
#include <QTextLayout>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace PDFTableCellLayout {
inline constexpr int MaximumCellCharacters = 1024;
inline constexpr int MaximumTableCharacters = 8192;
inline constexpr qreal CellPadding = 3.0;

inline bool validText(const QString &text)
{
    if (text.size() > MaximumCellCharacters || !text.isValidUtf16())
        return false;
    for (QChar character : text) {
        if (character.category() == QChar::Other_Control && character != QLatin1Char('\n'))
            return false;
    }
    return true;
}

// Positions and return values use displayed page coordinates, including CropBox
// and intrinsic rotation. Padding remains physical when the table is resized.
inline QVector<QRectF> cellRects(const QRectF &frame, const QSizeF &pageSize,
                                 qreal strokeWidthRatio, int rows, int columns,
                                 const QVector<qreal> &rowStops,
                                 const QVector<qreal> &columnStops)
{
    QVector<QRectF> result;
    if (rows < 1 || rows > 20 || columns < 1 || columns > 20
        || (!rowStops.isEmpty() && rowStops.size() != rows - 1)
        || (!columnStops.isEmpty() && columnStops.size() != columns - 1)
        || pageSize.isEmpty() || frame.isEmpty()
        || !std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height()))
        return result;
    const QRectF physical(frame.x() * pageSize.width(), frame.y() * pageSize.height(),
                          frame.width() * pageSize.width(), frame.height() * pageSize.height());
    const qreal inset = std::min({qMax<qreal>(0.75, strokeWidthRatio * pageSize.height()) / 2,
                                  physical.width() / 4, physical.height() / 4});
    const QRectF grid = physical.adjusted(inset, inset, -inset, -inset);
    const auto boundary = [](const QVector<qreal> &stops, int count, int index) {
        if (index == 0)
            return 0.0;
        if (index == count)
            return 1.0;
        return stops.isEmpty() ? qreal(index) / count : stops.at(index - 1);
    };
    result.reserve(rows * columns);
    for (int row = 0; row < rows; ++row) {
        const qreal top = grid.top() + grid.height() * boundary(rowStops, rows, row);
        const qreal bottom = grid.top() + grid.height() * boundary(rowStops, rows, row + 1);
        for (int column = 0; column < columns; ++column) {
            const qreal left = grid.left() + grid.width() * boundary(columnStops, columns, column);
            const qreal right = grid.left() + grid.width() * boundary(columnStops, columns, column + 1);
            result.append(QRectF((left + CellPadding) / pageSize.width(),
                (top + CellPadding) / pageSize.height(),
                qMax<qreal>(0, right - left - CellPadding * 2) / pageSize.width(),
                qMax<qreal>(0, bottom - top - CellPadding * 2) / pageSize.height()));
        }
    }
    return result;
}

inline bool fits(const QString &text, const QFont &font, const QSizeF &size,
                 const PDFTableCellFormat::Cell &format = {})
{
    if (text.isEmpty())
        return true;
    if (!validText(text) || size.isEmpty())
        return false;
    qreal height = 1.0; // The added-text writer starts one point below its frame.
    int paragraphStart = 0;
    for (const QString &paragraph : text.split(QLatin1Char('\n'))) {
        const bool formatted = !format.defaultStyle.family.isEmpty();
        const auto paragraphFont = formatted
            ? PDFTableCellFormat::displayFont(PDFTableCellFormat::styleAt(format, paragraphStart),
                                              font.pixelSize()) : font;
        QTextLayout layout(paragraph.isEmpty() ? QStringLiteral(" ") : paragraph, paragraphFont);
        if (formatted && !paragraph.isEmpty()) {
            QVector<QTextLayout::FormatRange> ranges;
            for (const auto &span : format.spans) {
                const int start = qMax(paragraphStart, span.start);
                const int end = qMin(paragraphStart + int(paragraph.size()), span.start + span.length);
                if (end > start) {
                    QTextCharFormat characterFormat;
                    characterFormat.setFont(PDFTableCellFormat::displayFont(span.style, font.pixelSize()));
                    ranges.append({start - paragraphStart, end - start, characterFormat});
                }
            }
            layout.setFormats(ranges);
        }
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        bool fits = true;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(size.width());
            height += line.height();
            if (height > size.height() + 0.001 || line.naturalTextWidth() > size.width() + 0.001) {
                fits = false;
                break;
            }
        }
        layout.endLayout();
        if (!fits)
            return false;
        paragraphStart += int(paragraph.size()) + 1;
    }
    return true;
}
} // namespace PDFTableCellLayout
