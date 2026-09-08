#pragma once

#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace PDFRasterPlan {

inline constexpr qint64 fullPageLimitBytes = 32 * 1024 * 1024;
inline constexpr qint64 fullPageReturnBytes = 24 * 1024 * 1024;
inline constexpr qint64 previewLimitBytes = 4 * 1024 * 1024;
inline constexpr int tileEdge = 1024;
inline constexpr int gutter = 2;
inline constexpr int preloadPixels = 128;
inline constexpr int maxTiles = 64;
inline constexpr int maxFullPageEdge = 4096;
inline constexpr int maxPreviewEdge = 2048;
inline constexpr int maximumRasterDimension = 1000000;

struct Tile {
    QRect rect;
    QRect renderRect;
};

struct Plan {
    bool tiled = false;
    QSize rasterSize;
    QSize previewSize;
    QVector<Tile> tiles;
    int visibleTileCount = 0;
};

// Invalid dimensions have no pixels. Overflow saturates so budget comparisons
// reject the image instead of accidentally treating it as a small allocation.
inline qint64 imageBytes(const QSize &size)
{
    if (size.isEmpty()) {
        return 0;
    }
    const qint64 width = size.width();
    const qint64 height = size.height();
    constexpr qint64 maximum = std::numeric_limits<qint64>::max();
    if (width > maximum / 4 / height) {
        return maximum;
    }
    return width * height * 4;
}

inline QSize rasterSize(const QSizeF &logicalSize, qreal devicePixelRatio)
{
    if (!std::isfinite(logicalSize.width())
        || !std::isfinite(logicalSize.height())
        || !std::isfinite(devicePixelRatio)
        || logicalSize.isEmpty() || devicePixelRatio <= 0) {
        return {};
    }
    const qreal width = std::ceil(logicalSize.width() * devicePixelRatio);
    const qreal height = std::ceil(logicalSize.height() * devicePixelRatio);
    if (!std::isfinite(width) || !std::isfinite(height)
        || width < 1 || height < 1
        || width > maximumRasterDimension
        || height > maximumRasterDimension) {
        return {};
    }
    return QSize(static_cast<int>(width), static_cast<int>(height));
}

namespace Detail {

inline QSize previewSize(const QSize &raster)
{
    const qreal scale = std::min({
        qreal(1),
        qreal(maxPreviewEdge) / qMax(raster.width(), raster.height()),
        std::sqrt(qreal(previewLimitBytes) / qreal(imageBytes(raster))),
    });
    return QSize(
        qMax(1, static_cast<int>(std::floor(raster.width() * scale))),
        qMax(1, static_cast<int>(std::floor(raster.height() * scale))));
}

inline qreal tileCenter(int index, int dimension)
{
    const int start = index * tileEdge;
    return start + qMin(tileEdge, dimension - start) / qreal(2);
}

// At most maxTiles values per axis are needed: a more distant axis value has
// maxTiles closer alternatives with the other axis held fixed.
inline QVector<int> nearestIndices(int first, int last,
                                   qreal center, int dimension)
{
    QVector<int> indices;
    if (first > last) {
        return indices;
    }
    indices.reserve(qMin(maxTiles, last - first + 1));
    const auto distance = [center, dimension](int index) {
        return std::abs(tileCenter(index, dimension) - center);
    };
    int nearest = qBound(first, static_cast<int>(center / tileEdge), last);
    if (nearest > first && distance(nearest - 1) < distance(nearest)) {
        --nearest;
    } else if (nearest < last && distance(nearest + 1) < distance(nearest)) {
        ++nearest;
    }
    indices.append(nearest);
    int left = nearest - 1;
    int right = nearest + 1;
    while (indices.size() < maxTiles && (left >= first || right <= last)) {
        if (right > last
            || (left >= first && distance(left) <= distance(right))) {
            indices.append(left--);
        } else {
            indices.append(right++);
        }
    }
    return indices;
}

struct Candidate {
    Tile tile;
    qreal distanceSquared = 0;
    bool visible = false;
};

inline void appendRegion(QVector<Candidate> &candidates,
                         const QSize &raster, const QPointF &center,
                         int firstColumn, int lastColumn,
                         int firstRow, int lastRow, bool visible)
{
    const QVector<int> columns = nearestIndices(
        firstColumn, lastColumn, center.x(), raster.width());
    const QVector<int> rows = nearestIndices(
        firstRow, lastRow, center.y(), raster.height());
    for (int row : rows) {
        for (int column : columns) {
            const int x = column * tileEdge;
            const int y = row * tileEdge;
            const QRect core(x, y,
                             qMin(tileEdge, raster.width() - x),
                             qMin(tileEdge, raster.height() - y));
            const QRect padded = core.adjusted(-gutter, -gutter, gutter, gutter)
                                     .intersected(QRect(QPoint(), raster));
            const qreal dx = tileCenter(column, raster.width()) - center.x();
            const qreal dy = tileCenter(row, raster.height()) - center.y();
            candidates.append({{core, padded}, dx * dx + dy * dy, visible});
        }
    }
}

} // namespace Detail

inline Plan makePlan(const QSize &raster, const QRectF &normalizedVisibleRect,
                     bool previouslyTiled = false, bool forceTiled = false)
{
    Plan plan;
    if (raster.isEmpty()
        || raster.width() > maximumRasterDimension
        || raster.height() > maximumRasterDimension) {
        return plan;
    }
    plan.rasterSize = raster;
    const qint64 limit = previouslyTiled
        ? fullPageReturnBytes : fullPageLimitBytes;
    plan.tiled = forceTiled || imageBytes(raster) > limit
        || qMax(raster.width(), raster.height()) > maxFullPageEdge;
    if (!plan.tiled) {
        return plan;
    }
    plan.previewSize = Detail::previewSize(raster);
    if (normalizedVisibleRect.isEmpty()
        || !std::isfinite(normalizedVisibleRect.x())
        || !std::isfinite(normalizedVisibleRect.y())
        || !std::isfinite(normalizedVisibleRect.width())
        || !std::isfinite(normalizedVisibleRect.height())) {
        return plan;
    }

    const qreal left = qBound(qreal(0), normalizedVisibleRect.left(), qreal(1));
    const qreal top = qBound(qreal(0), normalizedVisibleRect.top(), qreal(1));
    const qreal right = qBound(qreal(0), normalizedVisibleRect.right(), qreal(1));
    const qreal bottom = qBound(qreal(0), normalizedVisibleRect.bottom(), qreal(1));
    if (right <= left || bottom <= top) {
        return plan;
    }
    const QPointF center((left + right) * raster.width() / 2,
                         (top + bottom) * raster.height() / 2);
    // The viewport is half-open [left, right) x [top, bottom). Round its
    // outer bounds outwards, then use end - 1 for the last tile. Adjacent core
    // rectangles never overlap or leave a gap; only renderRect has a gutter.
    const int x = static_cast<int>(std::floor(left * raster.width()));
    const int y = static_cast<int>(std::floor(top * raster.height()));
    const int endX = static_cast<int>(std::ceil(right * raster.width()));
    const int endY = static_cast<int>(std::ceil(bottom * raster.height()));
    const int firstColumn = x / tileEdge;
    const int firstRow = y / tileEdge;
    const int lastColumn = (endX - 1) / tileEdge;
    const int lastRow = (endY - 1) / tileEdge;
    QVector<Detail::Candidate> candidates;
    Detail::appendRegion(candidates, raster, center,
                         firstColumn, lastColumn, firstRow, lastRow, true);

    // A huge viewport is bounded before enumerating: nearestIndices yields at
    // most 64 x 64 candidates, of which only the closest 64 survive. Preload is
    // useful only when all visible tiles fit within that per-page metadata cap.
    const qint64 visibleCount = qint64(lastColumn - firstColumn + 1)
        * (lastRow - firstRow + 1);
    if (visibleCount < maxTiles) {
        const int preloadFirstColumn = qMax(0, x - preloadPixels) / tileEdge;
        const int preloadFirstRow = qMax(0, y - preloadPixels) / tileEdge;
        const int preloadLastColumn
            = (qMin(raster.width(), endX + preloadPixels) - 1) / tileEdge;
        const int preloadLastRow
            = (qMin(raster.height(), endY + preloadPixels) - 1) / tileEdge;
        // Four disjoint strips avoid duplicating visible tiles or corners.
        Detail::appendRegion(candidates, raster, center,
                             preloadFirstColumn, preloadLastColumn,
                             preloadFirstRow, firstRow - 1, false);
        Detail::appendRegion(candidates, raster, center,
                             preloadFirstColumn, preloadLastColumn,
                             lastRow + 1, preloadLastRow, false);
        Detail::appendRegion(candidates, raster, center,
                             preloadFirstColumn, firstColumn - 1,
                             firstRow, lastRow, false);
        Detail::appendRegion(candidates, raster, center,
                             lastColumn + 1, preloadLastColumn,
                             firstRow, lastRow, false);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Detail::Candidate &a, const Detail::Candidate &b) {
                  if (a.visible != b.visible) {
                      return a.visible;
                  }
                  if (a.distanceSquared != b.distanceSquared) {
                      return a.distanceSquared < b.distanceSquared;
                  }
                  if (a.tile.rect.y() != b.tile.rect.y()) {
                      return a.tile.rect.y() < b.tile.rect.y();
                  }
                  return a.tile.rect.x() < b.tile.rect.x();
              });
    const int count = qMin(maxTiles, static_cast<int>(candidates.size()));
    plan.tiles.reserve(count);
    for (int i = 0; i < count; ++i) {
        plan.tiles.append(candidates.at(i).tile);
        if (candidates.at(i).visible) {
            ++plan.visibleTileCount;
        }
    }
    return plan;
}

} // namespace PDFRasterPlan
