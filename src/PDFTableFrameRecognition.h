#pragma once

#include "PDFShapeUtils.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <atomic>
#include <memory>
#include <numeric>

// Read-only recognition of complete, opaque ruled frames. All source objects
// remain independently owned by PDFium; a Group is only a bounded snapshot.
namespace PDFTableFrameRecognition {

struct Group {
    PDFShapeUtils::Metadata metadata;
    QVector<QVector<int>> objectPaths;
    QByteArray fingerprint;
};

namespace Detail {

inline constexpr int MaximumObjects = 50000;
inline constexpr int MaximumSegments = 16384;
inline constexpr int MaximumPrimitives = 2048;
inline constexpr int MaximumGroups = 64;
inline constexpr qreal Tolerance = .04;

inline bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

struct Primitive {
    // A small square fills a junction, but cannot introduce an axis on its own.
    enum Axis { Horizontal, Vertical, Junction } axis = Junction;
    QRectF ink;
    int owner = -1;
};

struct Object {
    int index = -1;
    QColor color;
    qreal width = 0;
    QJsonObject snapshot;
    QVector<Primitive> primitives;
};

inline bool near(qreal a, qreal b)
{
    return std::abs(a - b) <= Tolerance;
}

inline bool readObject(FPDF_PAGEOBJECT path, int index,
                       const PDFShapeUtils::CoordinateMapper &mapper,
                       const QSizeF &pageSize, Object &result)
{
    if (!path || FPDFPageObj_GetType(path) != FPDF_PAGEOBJ_PATH
        || FPDFPageObj_CountMarks(path) != 0 || FPDFPageObj_HasTransparency(path)
        || FPDFPageObj_GetDashCount(path) != 0)
        return false;
    FPDF_BOOL active = false;
    const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(path);
    if (!FPDFPageObj_GetIsActive(path, &active) || !active
        || (clip && FPDFClipPath_CountPaths(clip) > 0))
        return false;
    FS_MATRIX matrix{};
    float strokeWidth = 0, dashPhase = 0;
    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL stroke = false;
    const int cap = FPDFPageObj_GetLineCap(path);
    const int join = FPDFPageObj_GetLineJoin(path);
    if (!FPDFPageObj_GetMatrix(path, &matrix)
        || !FPDFPageObj_GetStrokeWidth(path, &strokeWidth)
        || !std::isfinite(strokeWidth)
        || !FPDFPageObj_GetDashPhase(path, &dashPhase) || dashPhase != 0
        || !FPDFPath_GetDrawMode(path, &fillMode, &stroke)
        || cap < FPDF_LINECAP_BUTT || cap > FPDF_LINECAP_PROJECTING_SQUARE
        || join < FPDF_LINEJOIN_MITER || join > FPDF_LINEJOIN_BEVEL)
        return false;
    QJsonArray transform;
    for (double value : {matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f}) {
        if (!std::isfinite(value))
            return false;
        transform.append(value);
    }
    const qreal scaleX = std::hypot(matrix.a, matrix.b);
    const qreal scaleY = std::hypot(matrix.c, matrix.d);
    const qreal dot = qreal(matrix.a) * matrix.c + qreal(matrix.b) * matrix.d;
    if (scaleX < .000001 || std::abs(scaleX - scaleY) > .00001 * qMax(scaleX, scaleY)
        || std::abs(dot) > .00001 * scaleX * scaleY)
        return false;
    const bool filled = fillMode == FPDF_FILLMODE_WINDING || fillMode == FPDF_FILLMODE_ALTERNATE;
    if ((filled && stroke) || (!filled && (!stroke || fillMode != FPDF_FILLMODE_NONE)))
        return false;
    unsigned int red = 0, green = 0, blue = 0, alpha = 0;
    const bool colorRead = filled
        ? FPDFPageObj_GetFillColor(path, &red, &green, &blue, &alpha)
        : FPDFPageObj_GetStrokeColor(path, &red, &green, &blue, &alpha);
    if (!colorRead || alpha != 255)
        return false;
    result.index = index;
    result.color = QColor(int(red), int(green), int(blue));
    QVector<QPointF> vertices;
    QVector<QPair<QPointF, QPointF>> edges;
    QJsonArray segments;
    QPointF previous, start;
    bool havePrevious = false;
    int moves = 0;
    const int count = FPDFPath_CountSegments(path);
    if (count < 2 || count > 256)
        return false;
    const QRectF visible(QPointF(0, 0), pageSize);
    for (int segmentIndex = 0; segmentIndex < count; ++segmentIndex) {
        const auto segment = FPDFPath_GetPathSegment(path, segmentIndex);
        float x = 0, y = 0;
        if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !std::isfinite(x) || !std::isfinite(y))
            return false;
        const int type = FPDFPathSegment_GetType(segment);
        const bool close = FPDFPathSegment_GetClose(segment);
        if (type != FPDF_SEGMENT_MOVETO && type != FPDF_SEGMENT_LINETO)
            return false;
        segments.append(QJsonArray{x, y, type, close});
        QPointF point;
        if (!mapper.devicePoint(QPointF(matrix.a * x + matrix.c * y + matrix.e,
                                       matrix.b * x + matrix.d * y + matrix.f), point)
            || !visible.adjusted(-Tolerance, -Tolerance, Tolerance, Tolerance).contains(point))
            return false;
        if (type == FPDF_SEGMENT_MOVETO) {
            if (filled && ++moves > 1)
                return false;
            start = point;
            havePrevious = true;
        } else {
            if (!havePrevious)
                return false;
            if (point != previous)
                edges.append({previous, point});
        }
        vertices.append(point);
        previous = point;
        if (close) {
            if (point != start)
                edges.append({point, start});
            previous = start;
        }
    }
    if (filled && previous != start)
        edges.append({previous, start}); // Filling implicitly closes the subpath.
    if (edges.isEmpty())
        return false;
    result.snapshot = {
        {QStringLiteral("index"), index}, {QStringLiteral("matrix"), transform},
        {QStringLiteral("segments"), segments}, {QStringLiteral("strokeWidth"), strokeWidth},
        {QStringLiteral("fillMode"), fillMode}, {QStringLiteral("stroke"), bool(stroke)},
        {QStringLiteral("cap"), cap}, {QStringLiteral("join"), join},
        {QStringLiteral("color"), QJsonArray{int(red), int(green), int(blue), int(alpha)}}};
    if (filled) {
        if (edges.size() != 4)
            return false;
        qreal left = vertices.first().x(), right = left;
        qreal top = vertices.first().y(), bottom = top;
        for (int edge = 0; edge < edges.size(); ++edge) {
            const auto delta = edges[edge].second - edges[edge].first;
            const auto next = edges[(edge + 1) % edges.size()].second
                - edges[(edge + 1) % edges.size()].first;
            const bool horizontal = near(delta.y(), 0) && std::abs(delta.x()) > Tolerance;
            const bool vertical = near(delta.x(), 0) && std::abs(delta.y()) > Tolerance;
            if ((!horizontal && !vertical) || edges[edge].second != edges[(edge + 1) % edges.size()].first
                || (horizontal && !near(next.x(), 0)) || (vertical && !near(next.y(), 0)))
                return false;
            left = qMin(left, edges[edge].first.x());
            right = qMax(right, edges[edge].first.x());
            top = qMin(top, edges[edge].first.y());
            bottom = qMax(bottom, edges[edge].first.y());
        }
        const QRectF ink(QPointF(left, top), QPointF(right, bottom));
        result.width = qMin(ink.width(), ink.height());
        if (result.width < .75 - .001 || result.width > 2.5)
            return false;
        const auto axis = near(ink.width(), ink.height()) ? Primitive::Junction
            : ink.width() > ink.height() ? Primitive::Horizontal : Primitive::Vertical;
        result.primitives.append({axis, ink, -1});
    } else {
        result.width = strokeWidth * scaleX / mapper.strokeWidth(1);
        if (!std::isfinite(result.width) || result.width < .75 - .001 || result.width > 2.5)
            return false;
        for (const auto &edge : edges) {
            const auto delta = edge.second - edge.first;
            const bool horizontal = near(delta.y(), 0) && std::abs(delta.x()) > Tolerance;
            const bool vertical = near(delta.x(), 0) && std::abs(delta.y()) > Tolerance;
            if (!horizontal && !vertical)
                return false;
            const qreal half = result.width / 2;
            const qreal extension = cap == FPDF_LINECAP_BUTT ? 0 : half;
            QRectF ink;
            if (horizontal) {
                ink = QRectF(QPointF(qMin(edge.first.x(), edge.second.x()) - extension,
                                     (edge.first.y() + edge.second.y()) / 2 - half),
                             QPointF(qMax(edge.first.x(), edge.second.x()) + extension,
                                     (edge.first.y() + edge.second.y()) / 2 + half));
            } else {
                ink = QRectF(QPointF((edge.first.x() + edge.second.x()) / 2 - half,
                                     qMin(edge.first.y(), edge.second.y()) - extension),
                             QPointF((edge.first.x() + edge.second.x()) / 2 + half,
                                     qMax(edge.first.y(), edge.second.y()) + extension));
            }
            if (!visible.adjusted(-Tolerance, -Tolerance, Tolerance, Tolerance).contains(ink))
                return false;
            result.primitives.append({horizontal ? Primitive::Horizontal : Primitive::Vertical, ink, -1});
        }
    }
    return true;
}

inline QVector<qreal> axes(QVector<qreal> coordinates)
{
    std::sort(coordinates.begin(), coordinates.end());
    QVector<qreal> result;
    for (qsizetype begin = 0; begin < coordinates.size();) {
        qsizetype end = begin + 1;
        while (end < coordinates.size() && near(coordinates[begin], coordinates[end]))
            ++end;
        result.append((coordinates[begin] + coordinates[end - 1]) / 2);
        begin = end;
    }
    return result;
}

inline bool covered(QVector<QPair<qreal, qreal>> intervals, qreal first, qreal last)
{
    std::sort(intervals.begin(), intervals.end());
    qreal end = first;
    for (const auto &interval : intervals) {
        if (interval.first > end + Tolerance)
            return false;
        end = qMax(end, interval.second);
    }
    return end >= last - Tolerance;
}

inline bool buildGroup(const QVector<Object> &objects, const QVector<Primitive> &primitives,
                       const QVector<int> &component, const QSizeF &pageSize, Group &group)
{
    QVector<qreal> horizontal, vertical;
    QSet<int> owners;
    const auto &firstObject = objects[primitives[component.first()].owner];
    const qreal width = firstObject.width;
    for (int index : component) {
        const auto &part = primitives[index];
        const auto &owner = objects[part.owner];
        if (owner.color != firstObject.color || !near(owner.width, width))
            return false;
        owners.insert(part.owner);
        if (part.axis == Primitive::Horizontal)
            horizontal.append(part.ink.center().y());
        else if (part.axis == Primitive::Vertical)
            vertical.append(part.ink.center().x());
    }
    horizontal = axes(std::move(horizontal));
    vertical = axes(std::move(vertical));
    if (horizontal.size() < 2 || vertical.size() < 2
        || (horizontal.size() == 2 && vertical.size() == 2)
        || horizontal.size() > PDFEditController::MaximumTableDimension + 1
        || vertical.size() > PDFEditController::MaximumTableDimension + 1)
        return false;
    const QRectF centers(QPointF(vertical.first(), horizontal.first()),
                         QPointF(vertical.last(), horizontal.last()));
    const QRectF frame = centers.adjusted(-width / 2, -width / 2, width / 2, width / 2);
    if (centers.width() < 4 || centers.height() < 4
        || !QRectF(QPointF(0, 0), pageSize).contains(frame))
        return false;
    const auto atAxis = [](const QVector<qreal> &values, qreal value) {
        return std::any_of(values.cbegin(), values.cend(), [value](qreal axis) { return near(value, axis); });
    };
    for (int index : component) {
        const auto &part = primitives[index];
        if (!frame.adjusted(-Tolerance, -Tolerance, Tolerance, Tolerance).contains(part.ink)
            || (part.axis == Primitive::Junction
                && (!atAxis(horizontal, part.ink.center().y())
                    || !atAxis(vertical, part.ink.center().x()))))
            return false;
    }
    for (bool isHorizontal : {true, false}) {
        const auto &coordinates = isHorizontal ? horizontal : vertical;
        for (qreal axis : coordinates) {
            QVector<QPair<qreal, qreal>> intervals;
            for (int index : component) {
                const auto &part = primitives[index];
                if (isHorizontal && part.axis != Primitive::Vertical && near(part.ink.center().y(), axis))
                    intervals.append({part.ink.left(), part.ink.right()});
                else if (!isHorizontal && part.axis != Primitive::Horizontal && near(part.ink.center().x(), axis))
                    intervals.append({part.ink.top(), part.ink.bottom()});
            }
            if (!covered(std::move(intervals), isHorizontal ? centers.left() : centers.top(),
                         isHorizontal ? centers.right() : centers.bottom()))
                return false;
        }
    }
    QVector<int> orderedOwners(owners.cbegin(), owners.cend());
    std::sort(orderedOwners.begin(), orderedOwners.end(), [&objects](int a, int b) {
        return objects[a].index < objects[b].index;
    });
    // Replacing one consecutive paint block preserves unrelated layer order.
    if (objects[orderedOwners.last()].index - objects[orderedOwners.first()].index + 1
        != orderedOwners.size())
        return false;
    QJsonArray snapshot;
    for (int owner : orderedOwners) {
        group.objectPaths.append(QVector<int>{objects[owner].index});
        snapshot.append(objects[owner].snapshot);
    }
    QJsonArray horizontalAxes, verticalAxes;
    for (qreal axis : horizontal)
        horizontalAxes.append(axis);
    for (qreal axis : vertical)
        verticalAxes.append(axis);
    const QJsonObject sourceSnapshot{
        {QStringLiteral("objects"), snapshot},
        {QStringLiteral("pageSize"), QJsonArray{pageSize.width(), pageSize.height()}},
        {QStringLiteral("horizontal"), horizontalAxes},
        {QStringLiteral("vertical"), verticalAxes}};
    group.fingerprint = QCryptographicHash::hash(
        QJsonDocument(sourceSnapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    auto &metadata = group.metadata;
    metadata.type = PDFEditController::TableShape;
    metadata.tableRows = int(horizontal.size()) - 1;
    metadata.tableColumns = int(vertical.size()) - 1;
    metadata.tableFrameOnly = true;
    metadata.frame = QRectF(frame.x() / pageSize.width(), frame.y() / pageSize.height(),
                            frame.width() / pageSize.width(), frame.height() / pageSize.height());
    metadata.strokeWidthRatio = width / pageSize.height();
    metadata.stroke = firstObject.color;
    for (int row = 1; row < horizontal.size() - 1; ++row)
        metadata.tableRowStops.append((horizontal[row] - centers.top()) / centers.height());
    for (int column = 1; column < vertical.size() - 1; ++column)
        metadata.tableColumnStops.append((vertical[column] - centers.left()) / centers.width());
    return PDFShapeUtils::validMetadata(metadata);
}

} // namespace Detail

inline QVector<Group> scan(FPDF_PAGE page, const QSizeF &pageSize,
                           const std::shared_ptr<std::atomic_bool> &cancellation = {})
{
    assertPDFiumProcessLockHeld();
    using namespace Detail;
    if (!page || !std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height())
        || pageSize.isEmpty() || pageSize.width() > 1000000 || pageSize.height() > 1000000)
        return {};
    const int count = FPDFPage_CountObjects(page);
    if (count < 0 || count > MaximumObjects)
        return {};
    const PDFShapeUtils::CoordinateMapper mapper(page, pageSize);
    QVector<Object> objects;
    QVector<Primitive> primitives;
    int segmentCount = 0;
    for (int index = 0; index < count; ++index) {
        if (cancelled(cancellation))
            return {};
        const auto object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH)
            continue;
        const int segments = FPDFPath_CountSegments(object);
        if (segments < 0 || segments > MaximumSegments - segmentCount)
            return {};
        segmentCount += segments;
        Object parsed;
        if (!readObject(object, index, mapper, pageSize, parsed))
            continue;
        if (parsed.primitives.size() > MaximumPrimitives - primitives.size())
            return {};
        const int owner = int(objects.size());
        for (auto part : parsed.primitives) {
            part.owner = owner;
            primitives.append(part);
        }
        objects.append(std::move(parsed));
    }
    QVector<int> parents(primitives.size());
    std::iota(parents.begin(), parents.end(), 0);
    const auto root = [&parents](int index) {
        while (parents[index] != index) {
            parents[index] = parents[parents[index]];
            index = parents[index];
        }
        return index;
    };
    for (int i = 0; i < primitives.size(); ++i) {
        if (cancelled(cancellation))
            return {};
        for (int j = i + 1; j < primitives.size(); ++j) {
            if (primitives[i].owner == primitives[j].owner
                || primitives[i].ink.adjusted(-Tolerance, -Tolerance, Tolerance, Tolerance)
                       .intersects(primitives[j].ink))
                parents[root(i)] = root(j);
        }
    }
    QMap<int, QVector<int>> components;
    for (int index = 0; index < primitives.size(); ++index)
        components[root(index)].append(index);
    QVector<Group> groups;
    for (const auto &component : std::as_const(components)) {
        if (cancelled(cancellation))
            return {};
        Group group;
        if (buildGroup(objects, primitives, component, pageSize, group)) {
            if (groups.size() == MaximumGroups)
                return {};
            groups.append(std::move(group));
        }
    }
    return groups;
}

} // namespace PDFTableFrameRecognition
