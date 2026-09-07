#pragma once

#include "PDFEditController.h"
#include "PDFiumSynchronization.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSizeF>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <fpdf_edit.h>
#include <fpdf_transformpage.h>

// A shape is adopted only when bounded semantic data reproduces its actual PDF
// path. Unmarked paths have a narrower, closed-polygon recognition contract;
// callers must exclude nested objects. These helpers run only inside a worker
// PDFium transaction.
namespace PDFShapeUtils {

inline constexpr char kMarkName[] = "TrePDFShape";
inline constexpr char kDataKey[] = "Data";
inline constexpr int kMaximumMetadataBytes = 16 * 1024;
inline constexpr int kMaximumSegments = 32;

struct Metadata {
    PDFEditController::ShapeType type = PDFEditController::RectangleShape;
    QRectF frame;
    QPointF start;
    QPointF end = QPointF(1.0, 1.0);
    qreal strokeWidthRatio = 0.0;
    QColor stroke = Qt::black;
    QColor fill;
    qreal opacity = 1.0;
};

inline bool validMetadata(const Metadata &metadata)
{
    const auto unit = [](qreal value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    };
    return metadata.type >= PDFEditController::RectangleShape
        && metadata.type <= PDFEditController::DiamondShape
        && unit(metadata.frame.left()) && unit(metadata.frame.top())
        && unit(metadata.frame.width()) && unit(metadata.frame.height())
        && metadata.frame.width() > 0.0 && metadata.frame.height() > 0.0
        && metadata.frame.right() <= 1.000001
        && metadata.frame.bottom() <= 1.000001
        && unit(metadata.start.x()) && unit(metadata.start.y())
        && unit(metadata.end.x()) && unit(metadata.end.y())
        && unit(metadata.opacity)
        && std::isfinite(metadata.strokeWidthRatio)
        && metadata.strokeWidthRatio > 0.0 && metadata.strokeWidthRatio <= 0.1
        && metadata.stroke.isValid() && metadata.stroke.alpha() == 255
        && (!metadata.fill.isValid() || metadata.fill.alpha() == 255)
        && ((metadata.type != PDFEditController::LineShape
             && metadata.type != PDFEditController::ArrowShape)
            || (!metadata.fill.isValid() && metadata.start != metadata.end));
}

class CoordinateMapper {
public:
    CoordinateMapper(FPDF_PAGE page, const QSizeF &size)
        : m_page(page), m_width(qMax(1, qRound(size.width() * 64))),
          m_height(qMax(1, qRound(size.height() * 64))) {}

    bool pagePoint(qreal x, qreal y, QPointF &point) const
    {
        double pageX = 0.0;
        double pageY = 0.0;
        if (!FPDF_DeviceToPage(m_page, 0, 0, m_width, m_height, 0,
                              qRound(x * 64), qRound(y * 64), &pageX, &pageY)) {
            return false;
        }
        point = QPointF(pageX, pageY);
        return true;
    }

    bool devicePoint(const QPointF &point, QPointF &result) const
    {
        // Invert the same display mapping used by createPath, retaining the
        // subpixel coordinates that FPDF_PageToDevice's integer output loses.
        QPointF origin, right, down;
        if (!pagePoint(0, 0, origin) || !pagePoint(1, 0, right)
            || !pagePoint(0, 1, down)) {
            return false;
        }
        const QPointF horizontal = right - origin;
        const QPointF vertical = down - origin;
        const qreal determinant = horizontal.x() * vertical.y()
            - horizontal.y() * vertical.x();
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
            return false;
        }
        const QPointF relative = point - origin;
        result = QPointF(
            (relative.x() * vertical.y() - relative.y() * vertical.x())
                / determinant,
            (horizontal.x() * relative.y() - horizontal.y() * relative.x())
                / determinant);
        return std::isfinite(result.x()) && std::isfinite(result.y());
    }

    qreal strokeWidth(qreal width) const
    {
        QPointF origin, right, down;
        if (!pagePoint(0, 0, origin) || !pagePoint(1, 0, right)
            || !pagePoint(0, 1, down)) {
            return width;
        }
        const QPointF horizontal = right - origin;
        const QPointF vertical = down - origin;
        return width * qMax<qreal>(0.01,
            (std::hypot(horizontal.x(), horizontal.y())
             + std::hypot(vertical.x(), vertical.y())) / 2.0);
    }

private:
    FPDF_PAGE m_page;
    int m_width;
    int m_height;
};

inline FPDF_PAGEOBJECT createPath(FPDF_PAGE page, const QSizeF &pageSize,
                                  const Metadata &metadata)
{
    assertPDFiumProcessLockHeld();
    if (!page || !validMetadata(metadata) || pageSize.isEmpty()
        || !std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height())
        || pageSize.width() > 1000000.0 || pageSize.height() > 1000000.0) {
        return nullptr;
    }
    const qreal pageWidth = pageSize.width();
    const qreal pageHeight = pageSize.height();
    const CoordinateMapper coordinateMapper(page, pageSize);
    const QRectF frame(
        metadata.frame.x() * pageWidth,
        metadata.frame.y() * pageHeight,
        metadata.frame.width() * pageWidth,
        metadata.frame.height() * pageHeight);
    const qreal strokeDeviceWidth = qMax<qreal>(
        0.75,
        metadata.strokeWidthRatio * pageHeight);
    const qreal inset = std::min({
        strokeDeviceWidth / 2.0,
        frame.width() / 4.0,
        frame.height() / 4.0});
    const QRectF drawingRect = frame.adjusted(
        inset,
        inset,
        -inset,
        -inset);
    if (drawingRect.isEmpty()) {
        return nullptr;
    }

    const auto mapPoint = [&coordinateMapper](
                              const QPointF &devicePoint,
                              QPointF &pagePoint) {
        return coordinateMapper.pagePoint(
            devicePoint.x(),
            devicePoint.y(),
            pagePoint);
    };
    FPDF_PAGEOBJECT path = nullptr;
    const auto createPath = [&path, &mapPoint](const QPointF &start) {
        QPointF pagePoint;
        if (!mapPoint(start, pagePoint)) {
            return false;
        }
        path = FPDFPageObj_CreateNewPath(
            static_cast<float>(pagePoint.x()),
            static_cast<float>(pagePoint.y()));
        return path != nullptr;
    };
    const auto lineTo = [&path, &mapPoint](const QPointF &end) {
        QPointF pagePoint;
        return path
            && mapPoint(end, pagePoint)
            && FPDFPath_LineTo(
                path,
                static_cast<float>(pagePoint.x()),
                static_cast<float>(pagePoint.y()));
    };
    const auto moveTo = [&path, &mapPoint](const QPointF &point) {
        QPointF pagePoint;
        return path
            && mapPoint(point, pagePoint)
            && FPDFPath_MoveTo(
                path,
                static_cast<float>(pagePoint.x()),
                static_cast<float>(pagePoint.y()));
    };
    const auto bezierTo = [&path, &mapPoint](
                              const QPointF &control1,
                              const QPointF &control2,
                              const QPointF &end) {
        QPointF pageControl1;
        QPointF pageControl2;
        QPointF pageEnd;
        return path
            && mapPoint(control1, pageControl1)
            && mapPoint(control2, pageControl2)
            && mapPoint(end, pageEnd)
            && FPDFPath_BezierTo(
                path,
                static_cast<float>(pageControl1.x()),
                static_cast<float>(pageControl1.y()),
                static_cast<float>(pageControl2.x()),
                static_cast<float>(pageControl2.y()),
                static_cast<float>(pageEnd.x()),
                static_cast<float>(pageEnd.y()));
    };
    const auto ratioPoint = [&frame](const QPointF &ratio) {
        return QPointF(
            frame.x()
                + qBound<qreal>(0.0, ratio.x(), 1.0)
                    * frame.width(),
            frame.y()
                + qBound<qreal>(0.0, ratio.y(), 1.0)
                    * frame.height());
    };

    bool closedShape = false;
    bool shapeCreated = false;
    switch (metadata.type) {
    case PDFEditController::RectangleShape: {
        const QPointF topLeft = drawingRect.topLeft();
        shapeCreated = createPath(topLeft)
            && lineTo(drawingRect.topRight())
            && lineTo(drawingRect.bottomRight())
            && lineTo(drawingRect.bottomLeft())
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    case PDFEditController::EllipseShape: {
        constexpr qreal kappa = 0.5522847498307936;
        const qreal centerX = drawingRect.center().x();
        const qreal centerY = drawingRect.center().y();
        const qreal radiusX = drawingRect.width() / 2.0;
        const qreal radiusY = drawingRect.height() / 2.0;
        shapeCreated = createPath(QPointF(centerX, drawingRect.top()))
            && bezierTo(
                QPointF(centerX + kappa * radiusX, drawingRect.top()),
                QPointF(drawingRect.right(), centerY - kappa * radiusY),
                QPointF(drawingRect.right(), centerY))
            && bezierTo(
                QPointF(drawingRect.right(), centerY + kappa * radiusY),
                QPointF(centerX + kappa * radiusX, drawingRect.bottom()),
                QPointF(centerX, drawingRect.bottom()))
            && bezierTo(
                QPointF(centerX - kappa * radiusX, drawingRect.bottom()),
                QPointF(drawingRect.left(), centerY + kappa * radiusY),
                QPointF(drawingRect.left(), centerY))
            && bezierTo(
                QPointF(drawingRect.left(), centerY - kappa * radiusY),
                QPointF(centerX - kappa * radiusX, drawingRect.top()),
                QPointF(centerX, drawingRect.top()))
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    case PDFEditController::LineShape:
    case PDFEditController::ArrowShape: {
        const QPointF start = ratioPoint(metadata.start);
        const QPointF end = ratioPoint(metadata.end);
        shapeCreated = createPath(start) && lineTo(end);
        if (shapeCreated
            && metadata.type == PDFEditController::ArrowShape) {
            const QPointF direction = end - start;
            const qreal length = std::hypot(
                direction.x(),
                direction.y());
            if (length <= 0.0) {
                shapeCreated = false;
                break;
            }
            const QPointF unit = direction / length;
            const QPointF normal(-unit.y(), unit.x());
            const qreal headLength = qMin(
                qMax<qreal>(6.0, strokeDeviceWidth * 4.0),
                length * 0.35);
            const qreal halfWidth = headLength * 0.55;
            const QPointF headBase = end - unit * headLength;
            const QPointF headLeft = headBase + normal * halfWidth;
            const QPointF headRight = headBase - normal * halfWidth;
            shapeCreated = moveTo(headLeft)
                && lineTo(end)
                && lineTo(headRight);
        }
        break;
    }
    case PDFEditController::TriangleShape: {
        shapeCreated = createPath(
                           QPointF(
                               drawingRect.center().x(),
                               drawingRect.top()))
            && lineTo(drawingRect.bottomRight())
            && lineTo(drawingRect.bottomLeft())
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    case PDFEditController::DiamondShape: {
        shapeCreated = createPath(
                           QPointF(
                               drawingRect.center().x(),
                               drawingRect.top()))
            && lineTo(
                QPointF(
                    drawingRect.right(),
                    drawingRect.center().y()))
            && lineTo(
                QPointF(
                    drawingRect.center().x(),
                    drawingRect.bottom()))
            && lineTo(
                QPointF(
                    drawingRect.left(),
                    drawingRect.center().y()))
            && FPDFPath_Close(path);
        closedShape = true;
        break;
    }
    }

    QColor strokeColor = metadata.stroke.toRgb();
    strokeColor.setAlphaF(metadata.opacity);
    QColor fillColor = metadata.fill.isValid()
        ? metadata.fill.toRgb() : QColor(Qt::transparent);
    if (metadata.fill.isValid()) {
        fillColor.setAlphaF(metadata.opacity);
    }
    const bool configured = shapeCreated
        && FPDFPageObj_SetStrokeColor(
            path,
            static_cast<unsigned int>(strokeColor.red()),
            static_cast<unsigned int>(strokeColor.green()),
            static_cast<unsigned int>(strokeColor.blue()),
            static_cast<unsigned int>(strokeColor.alpha()))
        && FPDFPageObj_SetStrokeWidth(
            path,
            static_cast<float>(
                coordinateMapper.strokeWidth(strokeDeviceWidth)))
        && FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND)
        && FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND)
        && (!closedShape
            || FPDFPageObj_SetFillColor(
                path,
                static_cast<unsigned int>(fillColor.red()),
                static_cast<unsigned int>(fillColor.green()),
                static_cast<unsigned int>(fillColor.blue()),
                static_cast<unsigned int>(fillColor.alpha())))
        && FPDFPath_SetDrawMode(
            path,
            closedShape ? FPDF_FILLMODE_WINDING : FPDF_FILLMODE_NONE,
            true);
    if (!configured) {
        if (path) {
            FPDFPageObj_Destroy(path);
        }
        return nullptr;
    }

    return path;
}

inline QJsonArray physicalSignature(FPDF_PAGEOBJECT path)
{
    assertPDFiumProcessLockHeld();
    if (!path || FPDFPageObj_GetType(path) != FPDF_PAGEOBJ_PATH
        || FPDFPageObj_GetDashCount(path) != 0
        || FPDFPageObj_GetLineCap(path) != FPDF_LINECAP_ROUND
        || FPDFPageObj_GetLineJoin(path) != FPDF_LINEJOIN_ROUND) {
        return {};
    }
    const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(path);
    // A default, empty PDFium clip handle reports -1 instead of zero.
    if (clip && FPDFClipPath_CountPaths(clip) > 0) {
        return {};
    }
    FS_MATRIX matrix{};
    float width = 0.0F;
    float phase = 0.0F;
    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL stroke = false;
    if (!FPDFPageObj_GetMatrix(path, &matrix)
        || matrix.a != 1.0F || matrix.b != 0.0F
        || matrix.c != 0.0F || matrix.d != 1.0F
        || matrix.e != 0.0F || matrix.f != 0.0F
        || !FPDFPageObj_GetStrokeWidth(path, &width)
        || !FPDFPageObj_GetDashPhase(path, &phase) || phase != 0.0F
        || !FPDFPath_GetDrawMode(path, &fillMode, &stroke) || !stroke) {
        return {};
    }
    QJsonArray signature;
    for (double value : {matrix.a, matrix.b, matrix.c, matrix.d,
                         matrix.e, matrix.f, width}) {
        if (!std::isfinite(value)) {
            return {};
        }
        signature.append(value);
    }
    signature.append(fillMode);
    // Normalize color channels to a unit interval. PDFium's decimal color
    // serialization can round a byte by one without changing its appearance.
    for (bool strokeColor : {true, false}) {
        if (!strokeColor && fillMode == FPDF_FILLMODE_NONE) {
            continue;
        }
        unsigned int red = 0, green = 0, blue = 0, alpha = 0;
        const bool ok = strokeColor
            ? FPDFPageObj_GetStrokeColor(path, &red, &green, &blue, &alpha)
            : FPDFPageObj_GetFillColor(path, &red, &green, &blue, &alpha);
        if (!ok) {
            return {};
        }
        signature.append(QJsonArray{red / 255.0, green / 255.0,
                                    blue / 255.0, alpha / 255.0});
    }
    const int count = FPDFPath_CountSegments(path);
    if (count < 2 || count > kMaximumSegments) {
        return {};
    }
    QJsonArray segments;
    QPointF subpathStart;
    for (int index = 0; index < count; ++index) {
        FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(path, index);
        float x = 0.0F, y = 0.0F;
        if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !std::isfinite(x) || !std::isfinite(y)) {
            return {};
        }
        const int type = FPDFPathSegment_GetType(segment);
        if (type == FPDF_SEGMENT_UNKNOWN) {
            return {};
        }
        if (type == FPDF_SEGMENT_MOVETO) {
            subpathStart = QPointF(x, y);
        }
        const bool closes = FPDFPathSegment_GetClose(segment);
        // PDFium expands the close flag to an explicit line to the subpath
        // origin when it reparses generated content. Normalize that equivalent
        // spelling so a save/reopen does not invalidate the managed identity.
        if (closes && type == FPDF_SEGMENT_LINETO
            && QPointF(x, y) == subpathStart && !segments.isEmpty()) {
            QJsonArray previous = segments.last().toArray();
            previous[3] = true;
            segments[segments.size() - 1] = previous;
        } else {
            segments.append(QJsonArray{x, y, type, closes});
        }
    }
    signature.append(segments);
    return signature;
}

inline bool signaturesMatch(const QJsonArray &left, const QJsonArray &right)
{
    if (left.size() < 10 || left.size() > 11 || left.size() != right.size()) {
        return false;
    }
    const auto numberMatches = [](const QJsonValue &a, const QJsonValue &b,
                                  double tolerance) {
        return a.isDouble() && b.isDouble()
            && std::isfinite(a.toDouble()) && std::isfinite(b.toDouble())
            && std::abs(a.toDouble() - b.toDouble()) <= tolerance;
    };
    for (int index = 0; index < 8; ++index) {
        const double tolerance = index < 4 ? 0.000001
            : index < 7 ? 0.0001 : 0.0;
        if (!numberMatches(left.at(index), right.at(index), tolerance)) {
            return false;
        }
    }
    for (qsizetype index = 8; index < left.size() - 1; ++index) {
        const QJsonArray a = left.at(index).toArray();
        const QJsonArray b = right.at(index).toArray();
        if (a.size() != 4 || b.size() != 4) {
            return false;
        }
        for (int channel = 0; channel < 4; ++channel) {
            if (!numberMatches(a.at(channel), b.at(channel),
                               1.0 / 255.0 + 0.000001)) {
                return false;
            }
        }
    }
    const QJsonArray a = left.last().toArray(), b = right.last().toArray();
    if (a.size() < 2 || a.size() > kMaximumSegments || a.size() != b.size()) {
        return false;
    }
    for (qsizetype index = 0; index < a.size(); ++index) {
        const QJsonArray sa = a.at(index).toArray(), sb = b.at(index).toArray();
        if (sa.size() != 4 || sb.size() != 4
            || !numberMatches(sa.at(2), sb.at(2), 0.0)
            || !sa.at(3).isBool() || !sb.at(3).isBool()
            || sa.at(3).toBool() != sb.at(3).toBool()) {
            return false;
        }
        for (int coordinate = 0; coordinate < 2; ++coordinate) {
            // PDFium writes float coordinates using decimal serialization.
            const double tolerance = qMin(0.015625,
                qMax(0.0001, 0.000002 * qMax(
                    std::abs(sa.at(coordinate).toDouble()),
                    std::abs(sb.at(coordinate).toDouble()))));
            if (!numberMatches(sa.at(coordinate), sb.at(coordinate), tolerance)) {
                return false;
            }
        }
    }
    return true;
}

inline QByteArray encodeMetadata(const Metadata &metadata,
                                  FPDF_PAGEOBJECT path)
{
    if (!validMetadata(metadata)) {
        return {};
    }
    const QJsonArray signature = physicalSignature(path);
    if (signature.isEmpty()) {
        return {};
    }
    const QJsonObject object{
        {QStringLiteral("version"), 1},
        {QStringLiteral("type"), metadata.type},
        {QStringLiteral("frame"), QJsonArray{metadata.frame.x(), metadata.frame.y(),
             metadata.frame.width(), metadata.frame.height()}},
        {QStringLiteral("start"), QJsonArray{metadata.start.x(), metadata.start.y()}},
        {QStringLiteral("end"), QJsonArray{metadata.end.x(), metadata.end.y()}},
        {QStringLiteral("width"), metadata.strokeWidthRatio},
        {QStringLiteral("stroke"), metadata.stroke.name(QColor::HexRgb)},
        {QStringLiteral("fill"), metadata.fill.isValid()
            ? metadata.fill.name(QColor::HexRgb) : QString()},
        {QStringLiteral("opacity"), metadata.opacity},
        {QStringLiteral("path"), signature}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

inline bool markPath(FPDF_DOCUMENT document, FPDF_PAGEOBJECT path,
                      const Metadata &metadata)
{
    const QByteArray bytes = encodeMetadata(metadata, path);
    if (bytes.isEmpty() || bytes.size() > kMaximumMetadataBytes
        || FPDFPageObj_CountMarks(path) != 0) {
        return false;
    }
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(path, kMarkName);
    return mark && FPDFPageObjMark_SetBlobParam(document, path, mark, kDataKey,
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<unsigned long>(bytes.size()));
}

inline bool readMetadata(FPDF_PAGE page, const QSizeF &pageSize,
                          FPDF_PAGEOBJECT path, Metadata &metadata,
                          QByteArray &bytes)
{
    assertPDFiumProcessLockHeld();
    bytes.clear();
    if (!path || FPDFPageObj_GetType(path) != FPDF_PAGEOBJ_PATH
        || FPDFPageObj_CountMarks(path) != 1) {
        return false;
    }
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(path, 0);
    std::array<FPDF_WCHAR, 32> name{};
    unsigned long length = 0;
    if (!mark || FPDFPageObjMark_CountParams(mark) != 1
        || !FPDFPageObjMark_GetName(mark, name.data(), sizeof(name), &length)
        || length != sizeof(FPDF_WCHAR) * (sizeof(kMarkName))
        || QString::fromUtf16(reinterpret_cast<const char16_t *>(name.data()))
            != QLatin1String(kMarkName)
        || !FPDFPageObjMark_GetParamBlobValue(mark, kDataKey, nullptr, 0, &length)
        || length == 0 || length > kMaximumMetadataBytes) {
        return false;
    }
    QByteArray raw(static_cast<qsizetype>(length), Qt::Uninitialized);
    unsigned long actualLength = 0;
    if (!FPDFPageObjMark_GetParamBlobValue(mark, kDataKey,
            reinterpret_cast<unsigned char *>(raw.data()), length, &actualLength)
        || length != actualLength) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    if (object.size() != 10 || object.value(QStringLiteral("version")).toInt(-1) != 1
        || !object.value(QStringLiteral("stroke")).isString()
        || !object.value(QStringLiteral("fill")).isString()) {
        return false;
    }
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    const auto numbers = [invalid](const QJsonValue &value, int count) {
        QVector<double> result;
        const QJsonArray array = value.toArray();
        if (array.size() != count) {
            return result;
        }
        for (const QJsonValue &item : array) {
            result.append(item.toDouble(invalid));
        }
        return result;
    };
    const QVector<double> frame = numbers(object.value(QStringLiteral("frame")), 4);
    const QVector<double> start = numbers(object.value(QStringLiteral("start")), 2);
    const QVector<double> end = numbers(object.value(QStringLiteral("end")), 2);
    if (frame.size() != 4 || start.size() != 2 || end.size() != 2) {
        return false;
    }
    Metadata parsed;
    const QJsonValue type = object.value(QStringLiteral("type"));
    if (!type.isDouble() || type.toDouble() != type.toInt(-1)) {
        return false;
    }
    parsed.type = static_cast<PDFEditController::ShapeType>(type.toInt(-1));
    parsed.frame = QRectF(frame[0], frame[1], frame[2], frame[3]);
    parsed.start = QPointF(start[0], start[1]);
    parsed.end = QPointF(end[0], end[1]);
    parsed.strokeWidthRatio = object.value(QStringLiteral("width")).toDouble(invalid);
    parsed.opacity = object.value(QStringLiteral("opacity")).toDouble(invalid);
    const QString stroke = object.value(QStringLiteral("stroke")).toString();
    const QString fill = object.value(QStringLiteral("fill")).toString();
    const auto color = [](const QString &text) {
        const QColor result(text);
        return text.size() == 7 && text.startsWith(QLatin1Char('#'))
                && result.isValid() && result.name(QColor::HexRgb) == text
            ? result : QColor();
    };
    parsed.stroke = color(stroke);
    parsed.fill = fill.isEmpty() ? QColor() : color(fill);
    if (!validMetadata(parsed) || (!fill.isEmpty() && !parsed.fill.isValid())) {
        return false;
    }
    const QJsonArray actual = physicalSignature(path);
    if (!signaturesMatch(actual, object.value(QStringLiteral("path")).toArray())) {
        return false;
    }
    FPDF_PAGEOBJECT canonical = createPath(page, pageSize, parsed);
    if (!canonical) {
        return false;
    }
    const bool matches = signaturesMatch(actual, physicalSignature(canonical))
        && static_cast<bool>(FPDFPageObj_HasTransparency(path))
            == static_cast<bool>(FPDFPageObj_HasTransparency(canonical));
    FPDFPageObj_Destroy(canonical);
    if (!matches) {
        return false;
    }
    metadata = parsed;
    bytes = raw;
    return true;
}

inline bool closedPolygon(FPDF_PAGEOBJECT path, QVector<QPointF> &vertices,
                          QJsonArray *rawSegments = nullptr)
{
    vertices.clear();
    if (rawSegments) {
        *rawSegments = QJsonArray();
    }
    // Three/four corners, optionally followed by an explicit closing vertex.
    const int count = FPDFPath_CountSegments(path);
    if (count < 3 || count > 5) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        const FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(path, index);
        float x = 0.0F, y = 0.0F;
        if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        const int type = FPDFPathSegment_GetType(segment);
        const bool closes = FPDFPathSegment_GetClose(segment);
        if (type != (index == 0 ? FPDF_SEGMENT_MOVETO : FPDF_SEGMENT_LINETO)
            || closes != (index == count - 1)) {
            return false;
        }
        vertices.append(QPointF(x, y));
        if (rawSegments) {
            rawSegments->append(QJsonArray{x, y, type, closes});
        }
    }
    if (vertices.last() == vertices.first()) {
        vertices.removeLast();
    }
    return vertices.size() == 3 || vertices.size() == 4;
}

inline bool polygonsMatch(const QVector<QPointF> &actual,
                          const QVector<QPointF> &canonical)
{
    if (actual.size() != canonical.size()) {
        return false;
    }
    // Independent rounding of a midpoint and its two extrema can differ by
    // one 1/64 display-point step in paths written by earlier TrePDF versions.
    constexpr qreal tolerance = 1.0 / 64.0 + 0.0001;
    const int count = static_cast<int>(actual.size());
    for (int direction : {1, -1}) {
        for (int offset = 0; offset < count; ++offset) {
            bool matches = true;
            for (int index = 0; index < count; ++index) {
                const QPointF delta = actual[index]
                    - canonical[(offset + direction * index + count) % count];
                if (std::abs(delta.x()) > tolerance
                    || std::abs(delta.y()) > tolerance) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return true;
            }
        }
    }
    return false;
}

inline bool readEditableShape(FPDF_PAGE page, const QSizeF &pageSize,
                              FPDF_PAGEOBJECT path, Metadata &metadata,
                              QByteArray &bytes)
{
    assertPDFiumProcessLockHeld();
    bytes.clear();
    if (!path || FPDFPageObj_GetType(path) != FPDF_PAGEOBJ_PATH) {
        return false;
    }
    // A damaged or unrelated mark must never gain eligibility through the
    // fallback. Managed shapes keep their original, stricter validation.
    if (FPDFPageObj_CountMarks(path) != 0) {
        return readMetadata(page, pageSize, path, metadata, bytes);
    }
    if (!page || pageSize.isEmpty() || !std::isfinite(pageSize.width())
        || !std::isfinite(pageSize.height()) || pageSize.width() > 1000000.0
        || pageSize.height() > 1000000.0) {
        return false;
    }
    const QJsonArray actualSignature = physicalSignature(path);
    QVector<QPointF> vertices;
    QJsonArray rawSegments;
    if (actualSignature.isEmpty() || !closedPolygon(path, vertices, &rawSegments)) {
        return false;
    }

    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL stroked = false;
    unsigned int sr = 0, sg = 0, sb = 0, sa = 0;
    unsigned int fr = 0, fg = 0, fb = 0, fa = 0;
    float strokeWidth = 0.0F;
    if (!FPDFPath_GetDrawMode(path, &fillMode, &stroked) || !stroked
        || (fillMode != FPDF_FILLMODE_NONE && fillMode != FPDF_FILLMODE_WINDING
            && fillMode != FPDF_FILLMODE_ALTERNATE)
        || !FPDFPageObj_GetStrokeColor(path, &sr, &sg, &sb, &sa)
        || !FPDFPageObj_GetStrokeWidth(path, &strokeWidth)
        || !FPDFPageObj_GetFillColor(path, &fr, &fg, &fb, &fa)
        || (fillMode != FPDF_FILLMODE_NONE && fa != 0 && fa != sa)) {
        return false;
    }
    const CoordinateMapper mapper(page, pageSize);
    const qreal scale = mapper.strokeWidth(1.0);
    const qreal deviceStrokeWidth = strokeWidth / scale;
    if (!std::isfinite(scale) || scale <= 0.0
        || !std::isfinite(deviceStrokeWidth) || deviceStrokeWidth < 0.75 - 0.0001) {
        return false;
    }
    for (QPointF &vertex : vertices) {
        QPointF displayed;
        if (!mapper.devicePoint(vertex, displayed)) {
            return false;
        }
        vertex = displayed;
    }
    qreal left = vertices.first().x(), right = left;
    qreal top = vertices.first().y(), bottom = top;
    for (const QPointF &vertex : std::as_const(vertices)) {
        left = qMin(left, vertex.x());
        right = qMax(right, vertex.x());
        top = qMin(top, vertex.y());
        bottom = qMax(bottom, vertex.y());
    }
    const QRectF drawingRect(QPointF(left, top), QPointF(right, bottom));
    // Keep degenerate/tiny polygons outside a matcher with subpixel tolerance.
    if (drawingRect.width() < 0.25 || drawingRect.height() < 0.25) {
        return false;
    }
    const qreal inset = std::min({deviceStrokeWidth / 2.0,
                                  drawingRect.width() / 2.0,
                                  drawingRect.height() / 2.0});
    const QRectF frame = drawingRect.adjusted(-inset, -inset, inset, inset);
    Metadata parsed;
    parsed.frame = QRectF(frame.x() / pageSize.width(), frame.y() / pageSize.height(),
                          frame.width() / pageSize.width(),
                          frame.height() / pageSize.height());
    parsed.strokeWidthRatio = deviceStrokeWidth / pageSize.height();
    parsed.stroke = QColor(static_cast<int>(sr), static_cast<int>(sg),
                            static_cast<int>(sb));
    parsed.fill = fillMode == FPDF_FILLMODE_NONE || fa == 0
        ? QColor() : QColor(static_cast<int>(fr),
        static_cast<int>(fg), static_cast<int>(fb));
    parsed.opacity = sa / 255.0;
    if (!validMetadata(parsed)) {
        return false;
    }

    for (const auto type : {PDFEditController::RectangleShape,
                            PDFEditController::TriangleShape,
                            PDFEditController::DiamondShape}) {
        parsed.type = type;
        FPDF_PAGEOBJECT canonical = createPath(page, pageSize, parsed);
        if (!canonical) {
            continue;
        }
        // Transparent fill and stroke-only are equivalent for these simple
        // polygons. Match the original spelling on this temporary object so
        // color/transparency validation still observes the source paint state.
        // PDFium includes inactive fill alpha in HasTransparency even when
        // the path only strokes, so mirror that state on the temporary path.
        const bool configured = FPDFPath_SetDrawMode(canonical, fillMode, true)
            && FPDFPageObj_SetFillColor(canonical, fr, fg, fb, fa);
        QVector<QPointF> canonicalVertices;
        bool matches = configured && closedPolygon(canonical, canonicalVertices);
        for (QPointF &vertex : canonicalVertices) {
            QPointF displayed;
            if (!mapper.devicePoint(vertex, displayed)) {
                matches = false;
                break;
            }
            vertex = displayed;
        }
        matches = matches && polygonsMatch(vertices, canonicalVertices);
        const QJsonArray canonicalSignature = physicalSignature(canonical);
        if (matches && !canonicalSignature.isEmpty()) {
            // Geometry was compared allowing cyclic starts and both windings;
            // retain the existing strict physical comparison for paint/width.
            QJsonArray comparable = actualSignature;
            comparable[comparable.size() - 1] = canonicalSignature.last();
            matches = signaturesMatch(comparable, canonicalSignature)
                && static_cast<bool>(FPDFPageObj_HasTransparency(path))
                    == static_cast<bool>(FPDFPageObj_HasTransparency(canonical));
        } else {
            matches = false;
        }
        FPDFPageObj_Destroy(canonical);
        if (!matches) {
            continue;
        }
        // No mark is written during discovery. Save revalidates this exact
        // source snapshot, including its original segment spelling and paint.
        const QJsonObject snapshot{
            {QStringLiteral("kind"), QStringLiteral("unmarkedCanonicalShape")},
            {QStringLiteral("shape"), QJsonDocument::fromJson(
                encodeMetadata(parsed, path)).object()},
            {QStringLiteral("segments"), rawSegments},
            {QStringLiteral("transparency"),
                static_cast<bool>(FPDFPageObj_HasTransparency(path))}};
        const QByteArray fingerprint = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        if (fingerprint.size() > kMaximumMetadataBytes) {
            return false;
        }
        metadata = parsed;
        bytes = fingerprint;
        return true;
    }
    return false;
}

} // namespace PDFShapeUtils
