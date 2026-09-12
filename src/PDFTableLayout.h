#pragma once

#include "SpreadsheetWorkbook.h"

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

// Geometry is in displayed PDF points, with a top-left origin. This small,
// backend-independent boundary also permits deterministic extraction tests.
namespace PDFTableLayout {
struct Rule {
    QPointF start;
    QPointF end;
    // Painted width in page points, used only to join endpoints through an
    // intersecting border. Zero denotes a geometric centerline.
    double thickness = 0.0;
};

struct Glyph {
    QString text;
    QRectF bounds;
    QPointF origin;
    bool upright = true;
    bool validUnicode = true;
    int sourceIndex = 0;
};

inline constexpr int maximumRules = 4096;
inline constexpr int maximumCharacters = 100000;
inline constexpr int maximumCells = 100000;
inline constexpr int maximumTables = 128;

// Ruled grids need a closed perimeter, at least two rows and two columns, and
// rectangular cells (including spans across absent interior dividers).
// On failure, tables is left unchanged. Names are supplied by the caller.
bool extract(const QVector<Rule> &rules,
             const QVector<Glyph> &glyphs,
             QVector<SpreadsheetSheet> &tables,
             const std::shared_ptr<std::atomic_bool> &cancellation,
             QString &error);
}
