#include "PDFTableExtractor.h"

#include "PDFTableLayout.h"
#include "PDFTableSymbolMapping.h"
#include "PDFiumSynchronization.h"

#include <QCoreApplication>
#include <QMap>
#include <QTransform>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
constexpr double coordinateTolerance = 0.6;
// Office-generated tables can shift the same divider slightly between row
// groups. Cluster axes within one point without relaxing endpoint/gap checks.
constexpr double axisTolerance = 1.0;
constexpr int maximumObjects = 20000;
constexpr int maximumPathSegments = 40000;
constexpr int maximumIntersectionWork = 2000000;
constexpr int maximumGlyphWork = 13000000;

bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

bool failLimit(QString &error)
{
    //% "This page exceeds the safe table extraction limits. No Excel file was written."
    error = qtTrId("ExcelTableExtractionLimit");
    return false;
}

bool failPage(QString &error)
{
    //% "The PDF page could not be read for table extraction."
    error = qtTrId("ExcelTablePageReadFailed");
    return false;
}

bool finitePoint(const QPointF &point)
{
    return qIsFinite(point.x()) && qIsFinite(point.y());
}

struct AxisRule {
    double axis = 0.0;
    double first = 0.0;
    double last = 0.0;
    double halfWidth = 0.0;
};

QVector<AxisRule> mergedRules(QVector<AxisRule> lines)
{
    std::sort(lines.begin(), lines.end(), [](const AxisRule &a, const AxisRule &b) {
        return a.axis < b.axis;
    });
    QVector<AxisRule> result;
    for (qsizetype begin = 0; begin < lines.size();) {
        qsizetype end = begin + 1;
        while (end < lines.size()
               && lines.at(end).axis - lines.at(begin).axis <= axisTolerance) {
            ++end;
        }
        const double axis = (lines.at(begin).axis + lines.at(end - 1).axis) / 2.0;
        std::sort(lines.begin() + begin, lines.begin() + end,
                  [](const AxisRule &a, const AxisRule &b) { return a.first < b.first; });
        AxisRule merged{axis, lines.at(begin).first, lines.at(begin).last,
                        lines.at(begin).halfWidth};
        for (qsizetype i = begin + 1; i < end; ++i) {
            if (lines.at(i).first <= merged.last + coordinateTolerance) {
                merged.last = std::max(merged.last, lines.at(i).last);
                merged.halfWidth = std::max(merged.halfWidth, lines.at(i).halfWidth);
            } else {
                result.append(merged);
                merged = {axis, lines.at(i).first, lines.at(i).last, lines.at(i).halfWidth};
            }
        }
        result.append(merged);
        begin = end;
    }
    return result;
}

struct Grid {
    struct Cell {
        int firstRow;
        int firstColumn;
        int lastRow;
        int lastColumn;
        QRectF bounds;
    };
    QVector<double> x;
    QVector<double> y;
    QRectF bounds;
    QVector<Cell> cells;
    QVector<int> cellIndices;
};

bool failGrid(QString &error)
{
    //% "A table has incomplete borders or a non-rectangular merged cell. This layout is not supported yet."
    error = qtTrId("ExcelTableUnsupportedGrid");
    return false;
}

bool buildGridCells(Grid &grid, const QVector<AxisRule> &horizontal,
                    const QVector<AxisRule> &vertical,
                    const std::shared_ptr<std::atomic_bool> &cancellation, QString &error)
{
    const int columns = int(grid.x.size()) - 1;
    const int rows = int(grid.y.size()) - 1;
    const int count = rows * columns;
    QVector<quint8> horizontalEdges((rows + 1) * columns, 0);
    QVector<quint8> verticalEdges(rows * (columns + 1), 0);
    const auto axisIndex = [](const QVector<double> &axes, double value) {
        const auto it = std::lower_bound(axes.cbegin(), axes.cend(), value - coordinateTolerance);
        return it != axes.cend() && std::abs(*it - value) <= coordinateTolerance
            ? int(it - axes.cbegin()) : -1;
    };
    // Endpoints must meet a grid axis. A line ending in the middle of an
    // elementary edge is damage/decoration, not evidence of a merged cell.
    for (const auto &line : horizontal) {
        const int row = axisIndex(grid.y, line.axis);
        const int first = axisIndex(grid.x, line.first);
        const int last = axisIndex(grid.x, line.last);
        if (row < 0 || first < 0 || last <= first)
            return failGrid(error);
        for (int column = first; column < last; ++column)
            horizontalEdges[row * columns + column] = 1;
    }
    for (const auto &line : vertical) {
        const int column = axisIndex(grid.x, line.axis);
        const int first = axisIndex(grid.y, line.first);
        const int last = axisIndex(grid.y, line.last);
        if (column < 0 || first < 0 || last <= first)
            return failGrid(error);
        for (int row = first; row < last; ++row)
            verticalEdges[row * (columns + 1) + column] = 1;
    }
    for (int column = 0; column < columns; ++column) {
        if (!horizontalEdges[column] || !horizontalEdges[rows * columns + column])
            return failGrid(error);
    }
    for (int row = 0; row < rows; ++row) {
        if (!verticalEdges[row * (columns + 1)]
            || !verticalEdges[row * (columns + 1) + columns])
            return failGrid(error);
    }
    QVector<int> parents(count);
    std::iota(parents.begin(), parents.end(), 0);
    const auto root = [&parents](int index) {
        while (parents[index] != index) {
            parents[index] = parents[parents[index]];
            index = parents[index];
        }
        return index;
    };
    for (int row = 0; row < rows; ++row) {
        if (cancelled(cancellation))
            return false;
        for (int column = 0; column < columns; ++column) {
            const int index = row * columns + column;
            if (column && !verticalEdges[row * (columns + 1) + column])
                parents[root(index)] = root(index - 1);
            if (row && !horizontalEdges[row * columns + column])
                parents[root(index)] = root(index - columns);
        }
    }
    QVector<int> regionIndices(count, -1);
    QVector<int> regionSizes;
    grid.cellIndices.resize(count);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int index = row * columns + column;
            const int owner = root(index);
            int &region = regionIndices[owner];
            if (region < 0) {
                region = int(grid.cells.size());
                grid.cells.append({row, column, row, column, {}});
                regionSizes.append(0);
            }
            auto &cell = grid.cells[region];
            cell.firstColumn = std::min(cell.firstColumn, column);
            cell.lastColumn = std::max(cell.lastColumn, column);
            cell.lastRow = row;
            ++regionSizes[region];
            grid.cellIndices[index] = region;
            // A border inside a connected region would be a dangling divider.
            if ((column && owner == root(index - 1)
                 && verticalEdges[row * (columns + 1) + column])
                || (row && owner == root(index - columns)
                    && horizontalEdges[row * columns + column]))
                return failGrid(error);
        }
    }
    for (int i = 0; i < grid.cells.size(); ++i) {
        auto &cell = grid.cells[i];
        if (regionSizes[i] != (cell.lastRow - cell.firstRow + 1)
                                 * (cell.lastColumn - cell.firstColumn + 1))
            return failGrid(error);
        cell.bounds = QRectF(QPointF(grid.x[cell.firstColumn], grid.y[cell.firstRow]),
                             QPointF(grid.x[cell.lastColumn + 1], grid.y[cell.lastRow + 1]));
    }
    return true;
}

bool detectGrids(const QVector<PDFTableLayout::Rule> &rules,
                 QVector<Grid> &grids,
                 const std::shared_ptr<std::atomic_bool> &cancellation,
                 QString &error)
{
    QVector<AxisRule> horizontal;
    QVector<AxisRule> vertical;
    for (const auto &rule : rules) {
        if (!finitePoint(rule.start) || !finitePoint(rule.end)
            || !qIsFinite(rule.thickness) || rule.thickness < 0.0 || rule.thickness > 2.5) {
            return failPage(error);
        }
        const double dx = std::abs(rule.end.x() - rule.start.x());
        const double dy = std::abs(rule.end.y() - rule.start.y());
        if (dy <= 0.1 && dx >= 3.0) {
            horizontal.append({(rule.start.y() + rule.end.y()) / 2.0,
                               std::min(rule.start.x(), rule.end.x()),
                               std::max(rule.start.x(), rule.end.x()), rule.thickness / 2.0});
        } else if (dx <= 0.1 && dy >= 3.0) {
            vertical.append({(rule.start.x() + rule.end.x()) / 2.0,
                             std::min(rule.start.y(), rule.end.y()),
                             std::max(rule.start.y(), rule.end.y()), rule.thickness / 2.0});
        }
    }
    horizontal = mergedRules(std::move(horizontal));
    vertical = mergedRules(std::move(vertical));
    if (horizontal.size() < 3 || vertical.size() < 3) {
        return true;
    }
    if (horizontal.size() * vertical.size() > maximumIntersectionWork) {
        return failLimit(error);
    }
    // Filled cell borders often stop at the painted edge of the perpendicular
    // border, not its centerline. Join through that actual painted width only;
    // increasing the global gap tolerance would also hide genuinely missing lines.
    for (auto &h : horizontal) {
        if (cancelled(cancellation))
            return false;
        for (auto &v : vertical) {
            const double xTolerance = coordinateTolerance + v.halfWidth;
            const double yTolerance = coordinateTolerance + h.halfWidth;
            if (v.axis < h.first - xTolerance || v.axis > h.last + xTolerance
                || h.axis < v.first - yTolerance || h.axis > v.last + yTolerance)
                continue;
            if (std::abs(h.first - v.axis) <= xTolerance) h.first = v.axis;
            if (std::abs(h.last - v.axis) <= xTolerance) h.last = v.axis;
            if (std::abs(v.first - h.axis) <= yTolerance) v.first = h.axis;
            if (std::abs(v.last - h.axis) <= yTolerance) v.last = h.axis;
        }
    }
    horizontal = mergedRules(std::move(horizontal));
    vertical = mergedRules(std::move(vertical));
    const int count = int(horizontal.size() + vertical.size());
    QVector<int> parents(count);
    std::iota(parents.begin(), parents.end(), 0);
    const auto root = [&parents](int index) {
        while (parents.at(index) != index) {
            parents[index] = parents.at(parents.at(index));
            index = parents.at(index);
        }
        return index;
    };
    for (qsizetype h = 0; h < horizontal.size(); ++h) {
        if (cancelled(cancellation)) {
            return false;
        }
        const auto &line = horizontal.at(h);
        for (qsizetype v = 0; v < vertical.size(); ++v) {
            const auto &other = vertical.at(v);
            if (other.axis >= line.first - coordinateTolerance
                && other.axis <= line.last + coordinateTolerance
                && line.axis >= other.first - coordinateTolerance
                && line.axis <= other.last + coordinateTolerance) {
                parents[root(int(h))] = root(int(horizontal.size() + v));
            }
        }
    }
    struct Component { QVector<AxisRule> horizontal; QVector<AxisRule> vertical; };
    QMap<int, Component> components;
    for (qsizetype h = 0; h < horizontal.size(); ++h) {
        components[root(int(h))].horizontal.append(horizontal.at(h));
    }
    for (qsizetype v = 0; v < vertical.size(); ++v) {
        components[root(int(horizontal.size() + v))].vertical.append(vertical.at(v));
    }
    qint64 cells = 0;
    for (const auto &component : std::as_const(components)) {
        if (cancelled(cancellation)) {
            return false;
        }
        if (component.horizontal.size() < 3 || component.vertical.size() < 3) {
            continue;
        }
        Grid grid;
        for (const auto &line : component.horizontal) {
            if (grid.y.isEmpty() || line.axis - grid.y.last() > coordinateTolerance) {
                grid.y.append(line.axis);
            }
        }
        for (const auto &line : component.vertical) {
            if (grid.x.isEmpty() || line.axis - grid.x.last() > coordinateTolerance) {
                grid.x.append(line.axis);
            }
        }
        if (grid.x.size() < 3 || grid.y.size() < 3) {
            continue;
        }
        grid.bounds = QRectF(QPointF(grid.x.first(), grid.y.first()),
                             QPointF(grid.x.last(), grid.y.last()));
        cells += (grid.x.size() - 1) * (grid.y.size() - 1);
        if (cells > PDFTableLayout::maximumCells
            || grids.size() >= PDFTableLayout::maximumTables) {
            return failLimit(error);
        }
        if (!buildGridCells(grid, component.horizontal, component.vertical, cancellation, error))
            return false;
        grids.append(std::move(grid));
    }
    std::sort(grids.begin(), grids.end(), [](const Grid &a, const Grid &b) {
        if (a.bounds.top() != b.bounds.top()) {
            return a.bounds.top() < b.bounds.top();
        }
        return a.bounds.left() < b.bounds.left();
    });
    return true;
}

QString cellText(QVector<const PDFTableLayout::Glyph *> glyphs)
{
    // PDF text-object order is not necessarily reading order. Group by baseline,
    // then sort each line horizontally; source order disambiguates combining marks.
    const auto horizontalOrder = [](const auto *a, const auto *b) {
        if (a->origin.x() != b->origin.x()) {
            return a->origin.x() < b->origin.x();
        }
        return a->sourceIndex < b->sourceIndex;
    };
    std::sort(glyphs.begin(), glyphs.end(), [](const auto *a, const auto *b) {
        if (a->origin.y() != b->origin.y()) {
            return a->origin.y() < b->origin.y();
        }
        return a->sourceIndex < b->sourceIndex;
    });
    struct TextLine {
        QVector<const PDFTableLayout::Glyph *> glyphs;
        QVector<const PDFTableLayout::Glyph *> visibleGlyphs;
        double baseline = 0.0;
        double maximumHeight = 0.0;
    };
    QVector<TextLine> lines;
    for (qsizetype begin = 0; begin < glyphs.size();) {
        qsizetype end = begin + 1;
        const double baseline = glyphs.at(begin)->origin.y();
        const double tolerance = std::max(1.0, glyphs.at(begin)->bounds.height() * 0.25);
        while (end < glyphs.size()
               && glyphs.at(end)->origin.y() - baseline <= tolerance) {
            ++end;
        }
        TextLine line;
        line.baseline = baseline;
        for (qsizetype i = begin; i < end; ++i) {
            const auto *glyph = glyphs.at(i);
            line.glyphs.append(glyph);
            if (!glyph->text.trimmed().isEmpty()) {
                line.visibleGlyphs.append(glyph);
                line.maximumHeight = std::max(line.maximumHeight, glyph->bounds.height());
            }
        }
        std::stable_sort(line.glyphs.begin(), line.glyphs.end(), horizontalOrder);
        std::stable_sort(line.visibleGlyphs.begin(), line.visibleGlyphs.end(), horizontalOrder);
        lines.append(std::move(line));
        begin = end;
    }
    // Footnote markers have a higher baseline than the text they follow. Merge
    // only a short, smaller group whose glyphs overlap and closely follow glyphs
    // of the immediately lower line. The fixed marker cap and binary searches
    // keep this bounded without broadening the normal multiline tolerance.
    for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
        auto &marker = lines[i];
        auto &body = lines[i + 1];
        if (marker.visibleGlyphs.isEmpty() || marker.visibleGlyphs.size() > 16
            || body.visibleGlyphs.isEmpty()
            || marker.maximumHeight > body.maximumHeight * 0.8
            || body.baseline - marker.baseline > body.maximumHeight * 0.8) {
            continue;
        }
        const bool followsBody = std::all_of(
            marker.visibleGlyphs.cbegin(), marker.visibleGlyphs.cend(),
            [&body](const auto *glyph) {
                const auto next = std::lower_bound(
                    body.visibleGlyphs.cbegin(), body.visibleGlyphs.cend(), glyph->origin.x(),
                    [](const auto *candidate, double x) { return candidate->origin.x() < x; });
                if (next == body.visibleGlyphs.cbegin()) {
                    return false;
                }
                const auto *previous = *(next - 1);
                const double gap = glyph->bounds.left() - previous->bounds.right();
                const double overlap = std::min(glyph->bounds.bottom(), previous->bounds.bottom())
                    - std::max(glyph->bounds.top(), previous->bounds.top());
                return gap >= -coordinateTolerance
                    && glyph->bounds.right() - previous->bounds.right() <= body.maximumHeight * 2.0
                    && overlap > 0.2 && glyph->bounds.top() < previous->bounds.top();
            });
        if (followsBody) {
            body.glyphs += marker.glyphs;
            std::stable_sort(body.glyphs.begin(), body.glyphs.end(), horizontalOrder);
            marker.glyphs.clear();
        }
    }
    QString result;
    for (const auto &textLine : std::as_const(lines)) {
        if (textLine.glyphs.isEmpty()) {
            continue;
        }
        QString line;
        const PDFTableLayout::Glyph *previous = nullptr;
        for (const auto *glyph : textLine.glyphs) {
            if (previous && glyph->sourceIndex != previous->sourceIndex + 1
                && !line.isEmpty() && !line.back().isSpace()
                && !glyph->text.front().isSpace()
                && glyph->text.front().combiningClass() == 0) {
                const double gap = glyph->bounds.left() - previous->bounds.right();
                const double em = std::max(previous->bounds.height(), glyph->bounds.height());
                if (gap > std::max(1.0, em * 0.20)) {
                    line.append(QLatin1Char(' '));
                }
            }
            line.append(glyph->text);
            previous = glyph;
        }
        if (!result.isEmpty()) {
            result.append(QLatin1Char('\n'));
        }
        result.append(line.trimmed());
    }
    return result.normalized(QString::NormalizationForm_C);
}

class PageHandle final
{
public:
    explicit PageHandle(FPDF_PAGE page) : value(page) {}
    ~PageHandle() { if (value) FPDF_ClosePage(value); }
    Q_DISABLE_COPY_MOVE(PageHandle)
    FPDF_PAGE value = nullptr;
};

class TextHandle final
{
public:
    explicit TextHandle(FPDF_TEXTPAGE page) : value(page) {}
    ~TextHandle() { if (value) FPDFText_ClosePage(value); }
    Q_DISABLE_COPY_MOVE(TextHandle)
    FPDF_TEXTPAGE value = nullptr;
};

bool pageTransform(FPDF_PAGE page, QTransform &transform, QRectF &bounds)
{
    const double width = FPDF_GetPageWidthF(page);
    const double height = FPDF_GetPageHeightF(page);
    if (!qIsFinite(width) || !qIsFinite(height) || width <= 0.0 || height <= 0.0
        || width > 1000000.0 || height > 1000000.0) {
        return false;
    }
    // Derive an affine mapping from PDFium itself so CropBox offsets and the
    // page's intrinsic /Rotate are identical for rules and text geometry.
    QPointF points[3];
    constexpr int extent = 1000000;
    const QPoint device[3] = {{0, 0}, {extent, 0}, {0, extent}};
    for (int i = 0; i < 3; ++i) {
        double x = 0.0;
        double y = 0.0;
        if (!FPDF_DeviceToPage(page, 0, 0, extent, extent, 0,
                               device[i].x(), device[i].y(), &x, &y)) {
            return false;
        }
        points[i] = QPointF(x, y);
    }
    const QPointF dx = points[1] - points[0];
    const QPointF dy = points[2] - points[0];
    const QTransform unitToPage(dx.x(), dx.y(), dy.x(), dy.y(),
                                points[0].x(), points[0].y());
    bool invertible = false;
    transform = unitToPage.inverted(&invertible) * QTransform::fromScale(width, height);
    bounds = QRectF(0.0, 0.0, width, height);
    return invertible;
}

struct RuleScanner {
    QVector<PDFTableLayout::Rule> rules;
    const std::shared_ptr<std::atomic_bool> &cancellation;
    QString &error;
    QRectF pageBounds;
    int objectCount = 0;
    int segmentCount = 0;

    bool append(const QPointF &start, const QPointF &end, double thickness = 0.0)
    {
        if (!finitePoint(start) || !finitePoint(end)) {
            return failPage(error);
        }
        // Do not turn content outside the CropBox into visible grid borders.
        const QRectF visible = pageBounds.adjusted(-coordinateTolerance, -coordinateTolerance,
                                                   coordinateTolerance, coordinateTolerance);
        if (!visible.contains(start) || !visible.contains(end)) {
            return true;
        }
        if (rules.size() >= PDFTableLayout::maximumRules) {
            return failLimit(error);
        }
        rules.append({start, end, thickness});
        return true;
    }

    bool appendFilledRectangle(const QVector<PDFTableLayout::Rule> &edges)
    {
        if (edges.size() != 4 || edges.last().end != edges.first().start)
            return true;
        double left = edges.first().start.x();
        double right = left;
        double top = edges.first().start.y();
        double bottom = top;
        for (int i = 0; i < edges.size(); ++i) {
            const auto &edge = edges.at(i);
            const auto &next = edges.at((i + 1) % edges.size());
            const QPointF delta = edge.end - edge.start;
            const QPointF nextDelta = next.end - next.start;
            const bool horizontal = std::abs(delta.y()) <= 0.1 && std::abs(delta.x()) > 0.1;
            const bool vertical = std::abs(delta.x()) <= 0.1 && std::abs(delta.y()) > 0.1;
            if ((!horizontal && !vertical) || edge.end != next.start
                || (horizontal && std::abs(nextDelta.x()) > 0.1)
                || (vertical && std::abs(nextDelta.y()) > 0.1))
                return true;
            left = std::min(left, edge.start.x());
            right = std::max(right, edge.start.x());
            top = std::min(top, edge.start.y());
            bottom = std::max(bottom, edge.start.y());
        }
        const QRectF rect(QPointF(left, top), QPointF(right, bottom));
        if (rect.height() <= 2.5 && rect.width() >= 5.0)
            return append(QPointF(rect.left(), rect.center().y()),
                          QPointF(rect.right(), rect.center().y()), rect.height());
        if (rect.width() <= 2.5 && rect.height() >= 5.0)
            return append(QPointF(rect.center().x(), rect.top()),
                          QPointF(rect.center().x(), rect.bottom()), rect.width());
        return true;
    }

    bool scan(FPDF_PAGEOBJECT object, const QTransform &parentTransform, int depth)
    {
        if (cancelled(cancellation)) {
            return false;
        }
        if (!object || ++objectCount > maximumObjects || depth > 8) {
            return failLimit(error);
        }
        const int type = FPDFPageObj_GetType(object);
        if (type != FPDF_PAGEOBJ_PATH && type != FPDF_PAGEOBJ_FORM) {
            return true;
        }
        FS_MATRIX matrix{};
        if (!FPDFPageObj_GetMatrix(object, &matrix)) {
            return failPage(error);
        }
        const QTransform transform = QTransform(matrix.a, matrix.b, matrix.c, matrix.d,
                                                 matrix.e, matrix.f) * parentTransform;
        const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
        if (clip && FPDFClipPath_CountPaths(clip) > 0) {
            // Arbitrary clipping cannot safely be inferred from the path alone.
            return true;
        }
        if (type == FPDF_PAGEOBJ_FORM) {
            const int count = FPDFFormObj_CountObjects(object);
            if (count < 0 || count > maximumObjects - objectCount) {
                return failLimit(error);
            }
            for (int i = 0; i < count; ++i) {
                if (!scan(FPDFFormObj_GetObject(object, static_cast<unsigned long>(i)),
                          transform, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        int fill = 0;
        FPDF_BOOL stroke = false;
        if (!FPDFPath_GetDrawMode(object, &fill, &stroke)) {
            return failPage(error);
        }
        unsigned int red = 0, green = 0, blue = 0, alpha = 0;
        if (stroke && (!FPDFPageObj_GetStrokeColor(object, &red, &green, &blue, &alpha)
                       || alpha == 0)) {
            stroke = false;
        }
        if (!stroke && (fill == FPDF_FILLMODE_NONE
                        || !FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)
                        || alpha == 0)) {
            return true;
        }
        const int count = FPDFPath_CountSegments(object);
        if (count < 0 || count > maximumPathSegments - segmentCount) {
            return failLimit(error);
        }
        segmentCount += count;
        QVector<PDFTableLayout::Rule> pathRules;
        QPointF first;
        QPointF previous;
        bool hasPrevious = false;
        bool curves = false;
        const auto finishSubpath = [&]() {
            // A single PDF path can contain dozens of separately filled cell
            // borders. Interpret each closed rectangle, not their combined bbox.
            const bool ok = curves || appendFilledRectangle(pathRules);
            pathRules.clear();
            curves = false;
            return ok;
        };
        for (int i = 0; i < count; ++i) {
            if ((i & 255) == 0 && cancelled(cancellation)) {
                return false;
            }
            FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(object, i);
            float x = 0.0f, y = 0.0f;
            if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y)) {
                return failPage(error);
            }
            const QPointF point = transform.map(QPointF(x, y));
            const int segmentType = FPDFPathSegment_GetType(segment);
            if (segmentType == FPDF_SEGMENT_MOVETO) {
                if (!stroke && !finishSubpath())
                    return false;
                first = point;
                hasPrevious = true;
            } else if (segmentType == FPDF_SEGMENT_LINETO && hasPrevious) {
                pathRules.append({previous, point});
            } else {
                curves = true;
            }
            if (FPDFPathSegment_GetClose(segment) && hasPrevious && point != first) {
                pathRules.append({point, first});
            }
            previous = point;
        }
        if (!stroke)
            return finishSubpath();
        if (curves) {
            return true;
        }
        for (const auto &rule : std::as_const(pathRules)) {
            if (!append(rule.start, rule.end)) {
                return false;
            }
        }
        return true;
    }
};
}

bool PDFTableLayout::extract(const QVector<Rule> &rules,
                             const QVector<Glyph> &glyphs,
                             QVector<SpreadsheetSheet> &tables,
                             const std::shared_ptr<std::atomic_bool> &cancellation,
                             QString &error)
{
    error.clear();
    if (cancelled(cancellation)) {
        return false;
    }
    if (rules.size() > maximumRules || glyphs.size() > maximumCharacters) {
        return failLimit(error);
    }
    QVector<Grid> grids;
    if (!detectGrids(rules, grids, cancellation, error)) {
        return false;
    }
    if (qint64(grids.size()) * glyphs.size() > maximumGlyphWork) {
        return failLimit(error);
    }
    QVector<SpreadsheetSheet> extracted;
    for (const auto &grid : std::as_const(grids)) {
        const int columns = int(grid.x.size()) - 1;
        const int rows = int(grid.y.size()) - 1;
        QVector<QVector<const Glyph *>> cells(grid.cells.size());
        for (qsizetype i = 0; i < glyphs.size(); ++i) {
            if ((i & 255) == 0 && cancelled(cancellation)) {
                return false;
            }
            const auto &glyph = glyphs.at(i);
            if (glyph.text.isEmpty() || glyph.text == QLatin1String("\r")
                || glyph.text == QLatin1String("\n")
                || !grid.bounds.contains(glyph.bounds.center())) {
                continue;
            }
            if (!glyph.upright || !finitePoint(glyph.bounds.topLeft())
                || !finitePoint(glyph.bounds.bottomRight()) || !finitePoint(glyph.origin)) {
                //% "A detected table contains rotated or skewed text that cannot be exported safely."
                error = qtTrId("ExcelTableUnsupportedText");
                return false;
            }
            if (!glyph.validUnicode) {
                //% "A detected table contains text without a reliable Unicode mapping."
                error = qtTrId("ExcelTableInvalidUnicode");
                return false;
            }
            const QPointF center = glyph.bounds.center();
            const int column = int(std::upper_bound(grid.x.cbegin(), grid.x.cend(), center.x())
                                   - grid.x.cbegin()) - 1;
            const int row = int(std::upper_bound(grid.y.cbegin(), grid.y.cend(), center.y())
                                - grid.y.cbegin()) - 1;
            if (column < 0 || column >= columns || row < 0 || row >= rows) {
                continue;
            }
            const int cellIndex = grid.cellIndices[row * columns + column];
            if (!grid.cells[cellIndex].bounds.adjusted(-coordinateTolerance, -coordinateTolerance,
                               coordinateTolerance, coordinateTolerance).contains(glyph.bounds)) {
                if (glyph.text.trimmed().isEmpty()) {
                    continue;
                }
                //% "Text crosses a detected table border. The cells cannot be assigned safely."
                error = qtTrId("ExcelTableAmbiguousText");
                return false;
            }
            cells[cellIndex].append(&glyph);
        }
        SpreadsheetSheet sheet;
        int populatedCells = 0;
        QVector<QString> values(rows * columns);
        for (int i = 0; i < grid.cells.size(); ++i) {
            const auto &cell = grid.cells[i];
            QString value = cellText(std::move(cells[i]));
            if (!value.isEmpty())
                ++populatedCells;
            values[cell.firstRow * columns + cell.firstColumn] = std::move(value);
            if (cell.firstRow != cell.lastRow || cell.firstColumn != cell.lastColumn)
                sheet.mergedRanges.append({cell.firstRow, cell.firstColumn,
                                           cell.lastRow, cell.lastColumn});
        }
        for (int row = 0; row < rows; ++row) {
            if (cancelled(cancellation)) {
                return false;
            }
            SpreadsheetRow outputRow;
            for (int column = 0; column < columns; ++column) {
                outputRow.cells.append({column, std::move(values[row * columns + column])});
            }
            sheet.rows.append(std::move(outputRow));
        }
        // Blank grids and decorative frames are not data tables.
        if (populatedCells < 2) {
            continue;
        }
        for (int column = 0; column < columns; ++column) {
            sheet.columnWidths.append(qBound(8.0,
                (grid.x.at(column + 1) - grid.x.at(column)) / 5.25, 80.0));
        }
        extracted.append(std::move(sheet));
    }
    tables += extracted;
    return true;
}

bool PDFTableExtractor::extractPage(
    FPDF_DOCUMENT document,
    int sourcePage,
    QVector<SpreadsheetSheet> &tables,
    const std::shared_ptr<std::atomic_bool> &cancellation,
    QString &error)
{
    assertPDFiumProcessLockHeld();
    error.clear();
    if (cancelled(cancellation)) {
        return false;
    }
    if (!document || sourcePage < 0 || sourcePage >= FPDF_GetPageCount(document)) {
        return failPage(error);
    }
    PageHandle page(FPDF_LoadPage(document, sourcePage));
    if (!page.value) {
        return failPage(error);
    }
    QTransform transform;
    QRectF bounds;
    if (!pageTransform(page.value, transform, bounds)) {
        return failPage(error);
    }
    RuleScanner scanner{{}, cancellation, error, bounds};
    const int objectCount = FPDFPage_CountObjects(page.value);
    if (objectCount < 0 || objectCount > maximumObjects) {
        return failLimit(error);
    }
    for (int i = 0; i < objectCount; ++i) {
        if (!scanner.scan(FPDFPage_GetObject(page.value, i), transform, 0)) {
            return false;
        }
    }
    if (scanner.rules.size() < 6) {
        return true;
    }
    TextHandle text(FPDFText_LoadPage(page.value));
    if (!text.value) {
        return failPage(error);
    }
    const int count = FPDFText_CountChars(text.value);
    if (count < 0 || count > PDFTableLayout::maximumCharacters) {
        return failLimit(error);
    }
    QVector<PDFTableLayout::Glyph> glyphs;
    glyphs.reserve(count);
    for (int i = 0; i < count; ++i) {
        if ((i & 255) == 0 && cancelled(cancellation)) {
            return false;
        }
        char32_t codepoint = FPDFText_GetUnicode(text.value, i);
        if (codepoint == U'\r' || codepoint == U'\n'
            || FPDFText_IsGenerated(text.value, i) == 1) {
            continue;
        }
        if (codepoint == 0xf0fc || codepoint == 0xf0fd) {
            char fontName[128]{};
            const unsigned long nameLength = FPDFText_GetFontInfo(
                text.value, i, fontName, sizeof(fontName), nullptr);
            if (nameLength > 1 && nameLength <= sizeof(fontName)
                && fontName[nameLength - 1] == '\0') {
                codepoint = PDFTableSymbolMapping::unicodeSymbol(
                    codepoint, std::string_view(fontName, nameLength - 1));
            }
        }
        double left = 0.0, right = 0.0, bottom = 0.0, top = 0.0;
        double x = 0.0, y = 0.0;
        if (!FPDFText_GetCharBox(text.value, i, &left, &right, &bottom, &top)
            || !FPDFText_GetCharOrigin(text.value, i, &x, &y)) {
            return failPage(error);
        }
        PDFTableLayout::Glyph glyph;
        glyph.validUnicode = codepoint > 0 && codepoint <= 0x10ffff
            && !(codepoint >= 0xd800 && codepoint <= 0xdfff)
            && FPDFText_HasUnicodeMapError(text.value, i) == 0;
        glyph.text = glyph.validUnicode ? QString::fromUcs4(&codepoint, 1)
                                        : QString(QChar::ReplacementCharacter);
        glyph.bounds = transform.mapRect(QRectF(QPointF(left, bottom),
                                                 QPointF(right, top)).normalized());
        glyph.origin = transform.map(QPointF(x, y));
        glyph.sourceIndex = i;
        FS_MATRIX matrix{};
        glyph.upright = FPDFText_GetMatrix(text.value, i, &matrix);
        if (glyph.upright) {
            const QTransform textTransform = QTransform(matrix.a, matrix.b, matrix.c, matrix.d,
                                                         matrix.e, matrix.f) * transform;
            const QPointF origin = textTransform.map(QPointF(0, 0));
            const QPointF dx = textTransform.map(QPointF(1, 0)) - origin;
            const QPointF dy = textTransform.map(QPointF(0, 1)) - origin;
            glyph.upright = dx.x() > 0.0 && dy.y() < 0.0
                && std::abs(dx.y()) <= std::abs(dx.x()) * 0.01
                && std::abs(dy.x()) <= std::abs(dy.y()) * 0.25;
        }
        glyphs.append(std::move(glyph));
    }
    return PDFTableLayout::extract(scanner.rules, glyphs, tables, cancellation, error);
}
