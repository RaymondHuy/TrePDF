#include "PDFWatermark.h"
#include "PDFiumSynchronization.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QUuid>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include <cstring>
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>

namespace PDFWatermarks {
namespace {
constexpr char DataKey[] = "Data";
constexpr qsizetype MaximumImageBytes = 16 * 1024 * 1024;
// Original group assets retain at most 64 MiB. One decoded source plus its
// PDFium validation bitmap adds at most 32 MiB of bounded temporary storage.
constexpr qsizetype MaximumRetainedBytes = 64 * 1024 * 1024;
constexpr qsizetype MaximumMetadataBytes = 24 * 1024 * 1024;
constexpr qint64 MaximumScanMetadataBytes = 256 * 1024 * 1024;
constexpr int MaximumSegments = 32768;
constexpr int MaximumPageObjects = 100000;
constexpr qint64 MaximumScanObjects = 2000000;
constexpr qint64 MaximumScanSegments = 2000000;

struct ReadBudget {
    qint64 metadata = MaximumScanMetadataBytes;
    qint64 segments = MaximumScanSegments;
    qint64 decodedImageBytes = qint64(4) * 1024 * 1024 * 1024;
    bool exhausted = false;
};

bool finite(qreal value) { return std::isfinite(value); }
bool unit(qreal value) { return finite(value) && value >= 0 && value <= 1; }
bool cancelled(const std::shared_ptr<std::atomic_bool> &cancel)
{ return cancel && cancel->load(std::memory_order_relaxed); }

bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return pattern.match(id).hasMatch();
}

bool validImageSize(const QSize &size)
{
    return size.width() > 0 && size.height() > 0
        && size.width() <= 8192 && size.height() <= 8192
        && qint64(size.width()) * size.height() * 4 <= MaximumImageBytes;
}

bool validPageSize(const QSizeF &size)
{
    return finite(size.width()) && finite(size.height())
        && size.width() > 0 && size.height() > 0
        && size.width() <= 1000000 && size.height() <= 1000000;
}

bool validPath(const QPainterPath &path)
{
    if (path.isEmpty() || path.elementCount() > MaximumSegments)
        return false;
    const QRectF bounds = path.boundingRect();
    if (!validPageSize(bounds.size()))
        return false;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        if (!finite(element.x) || !finite(element.y)
            || std::abs(element.x) > 1000000 || std::abs(element.y) > 1000000)
            return false;
    }
    return true;
}

QJsonObject settings(const Spec &spec)
{
    return {{QStringLiteral("id"), spec.id},
        {QStringLiteral("image"), spec.imageMode},
        {QStringLiteral("text"), spec.text},
        {QStringLiteral("font"), spec.fontFamily},
        {QStringLiteral("size"), spec.fontSize},
        {QStringLiteral("color"), spec.color.name(QColor::HexRgb)},
        {QStringLiteral("opacity"), spec.opacity},
        {QStringLiteral("rotation"), spec.rotation},
        {QStringLiteral("scale"), spec.scale},
        {QStringLiteral("x"), spec.x}, {QStringLiteral("y"), spec.y},
        {QStringLiteral("behind"), spec.behind}};
}

bool parseSettings(const QJsonValue &value, Spec &spec)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.size() != 12)
        return false;
    for (const char *key : {"id", "text", "font", "color"}) {
        if (!object.value(QLatin1String(key)).isString())
            return false;
    }
    for (const char *key : {"image", "behind"}) {
        if (!object.value(QLatin1String(key)).isBool())
            return false;
    }
    for (const char *key : {"size", "opacity", "rotation", "scale", "x", "y"}) {
        const QJsonValue number = object.value(QLatin1String(key));
        if (!number.isDouble() || !finite(number.toDouble()))
            return false;
    }
    spec.id = object.value(QStringLiteral("id")).toString();
    spec.imageMode = object.value(QStringLiteral("image")).toBool();
    spec.text = object.value(QStringLiteral("text")).toString();
    spec.fontFamily = object.value(QStringLiteral("font")).toString();
    spec.fontSize = object.value(QStringLiteral("size")).toDouble();
    const QString color = object.value(QStringLiteral("color")).toString();
    spec.color = QColor(color);
    if (color.size() != 7 || spec.color.name(QColor::HexRgb) != color)
        return false;
    spec.opacity = object.value(QStringLiteral("opacity")).toDouble();
    spec.rotation = object.value(QStringLiteral("rotation")).toDouble();
    spec.scale = object.value(QStringLiteral("scale")).toDouble();
    spec.x = object.value(QStringLiteral("x")).toDouble();
    spec.y = object.value(QStringLiteral("y")).toDouble();
    spec.behind = object.value(QStringLiteral("behind")).toBool();
    return validSpec(spec, false);
}

bool markerName(FPDF_PAGEOBJECTMARK mark)
{
    std::array<FPDF_WCHAR, 32> name{};
    unsigned long length = 0;
    return mark && FPDFPageObjMark_GetName(mark, name.data(), sizeof(name), &length)
        && length == sizeof(FPDF_WCHAR) * sizeof(MarkName)
        && name[length / sizeof(FPDF_WCHAR) - 1] == 0
        && QString::fromUtf16(reinterpret_cast<const char16_t *>(name.data()),
            static_cast<qsizetype>(length / sizeof(FPDF_WCHAR)) - 1)
            == QLatin1String(MarkName);
}

bool readBlob(FPDF_PAGEOBJECT object, QByteArray &bytes, ReadBudget &budget)
{
    if (FPDFPageObj_CountMarks(object) != 1)
        return false;
    const FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(object, 0);
    unsigned long length = 0;
    if (!markerName(mark) || FPDFPageObjMark_CountParams(mark) != 1
        || !FPDFPageObjMark_GetParamBlobValue(mark, DataKey, nullptr, 0, &length))
        return false;
    budget.metadata -= length;
    if (budget.metadata < 0) {
        budget.exhausted = true;
        return false;
    }
    if (!length || length > MaximumMetadataBytes)
        return false;
    bytes.resize(static_cast<qsizetype>(length));
    unsigned long actual = 0;
    return FPDFPageObjMark_GetParamBlobValue(mark, DataKey,
        reinterpret_cast<unsigned char *>(bytes.data()), length, &actual)
        && actual == length;
}

bool noClip(FPDF_PAGEOBJECT object)
{
    const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
    FPDF_BOOL active = false;
    return (!clip || FPDFClipPath_CountPaths(clip) <= 0)
        && FPDFPageObj_GetIsActive(object, &active) && active;
}

QJsonArray matrixValues(const FS_MATRIX &matrix)
{
    QJsonArray result;
    for (double v : {matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f}) {
        if (!finite(v) || std::abs(v) > 10000000)
            return {};
        result.append(v);
    }
    return result;
}

QJsonArray visualSignature(FPDF_PAGEOBJECT object, bool imageMode)
{
    if (!noClip(object) || FPDFPageObj_GetType(object)
        != (imageMode ? FPDF_PAGEOBJ_IMAGE : FPDF_PAGEOBJ_PATH))
        return {};
    FS_MATRIX matrix{};
    unsigned int red = 0, green = 0, blue = 0, alpha = 0;
    if (!FPDFPageObj_GetMatrix(object, &matrix)
        || (!imageMode && !FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)))
        return {};
    const QJsonArray transform = matrixValues(matrix);
    if (transform.size() != 6)
        return {};
    QJsonArray result{transform};
    if (imageMode)
        return result;
    result.append(QJsonArray{int(red), int(green), int(blue), int(alpha)});
    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL stroke = true;
    if (matrix.a != 1 || matrix.b != 0 || matrix.c != 0 || matrix.d != 1
        || matrix.e != 0 || matrix.f != 0
        || !FPDFPath_GetDrawMode(object, &fillMode, &stroke)
        || stroke || fillMode != FPDF_FILLMODE_WINDING)
        return {};
    const int count = FPDFPath_CountSegments(object);
    if (count < 2 || count > MaximumSegments)
        return {};
    QJsonArray segments;
    for (int i = 0; i < count; ++i) {
        const FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(object, i);
        float x = 0, y = 0;
        if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !finite(x) || !finite(y) || std::abs(x) > 10000000
            || std::abs(y) > 10000000)
            return {};
        const int type = FPDFPathSegment_GetType(segment);
        if (type != FPDF_SEGMENT_MOVETO && type != FPDF_SEGMENT_LINETO
            && type != FPDF_SEGMENT_BEZIERTO)
            return {};
        segments.append(QJsonArray{x, y, type, bool(FPDFPathSegment_GetClose(segment))});
    }
    result.append(segments);
    return result;
}

bool numericArrayMatches(const QJsonArray &actual, const QJsonArray &expected,
                         qreal minimumTolerance, qreal relativeTolerance)
{
    if (actual.size() != expected.size())
        return false;
    for (qsizetype i = 0; i < actual.size(); ++i) {
        if (!actual[i].isDouble() || !expected[i].isDouble())
            return false;
        const qreal a = actual[i].toDouble(), b = expected[i].toDouble();
        const qreal tolerance = qMax(minimumTolerance,
            relativeTolerance * qMax(std::abs(a), std::abs(b)));
        if (!finite(a) || !finite(b) || std::abs(a - b) > tolerance)
            return false;
    }
    return true;
}

bool signatureMatches(const QJsonArray &actual, const QJsonArray &expected,
                      bool imageMode)
{
    if (actual.size() != (imageMode ? 1 : 3) || actual.size() != expected.size()
        || actual[0].toArray().size() != 6
        || !numericArrayMatches(actual[0].toArray(), expected[0].toArray(), 0.0001, 0.000002))
        return false;
    if (imageMode)
        return true;
    if (actual[1].toArray().size() != 4
        || !numericArrayMatches(actual[1].toArray(), expected[1].toArray(), 1, 0))
        return false;
    const QJsonArray a = actual[2].toArray(), b = expected[2].toArray();
    if (a.size() < 2 || a.size() > MaximumSegments || a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        const QJsonArray left = a[i].toArray(), right = b[i].toArray();
        if (left.size() != 4 || right.size() != 4
            || !right[3].isBool() || left[3].toBool() != right[3].toBool()
            || !right[2].isDouble() || left[2].toInt(-1) != right[2].toDouble()
            || !numericArrayMatches(QJsonArray{left[0], left[1]},
                QJsonArray{right[0], right[1]}, 0.0001, 0.000002))
            return false;
    }
    return true;
}

QTransform pageMapping(FPDF_PAGE page, const QSizeF &size, bool &ok)
{
    ok = false;
    if (!validPageSize(size))
        return {};
    const int width = qMax(1, qRound(size.width() * 64));
    const int height = qMax(1, qRound(size.height() * 64));
    double x0, y0, xx, yx, xy, yy;
    if (!FPDF_DeviceToPage(page, 0, 0, width, height, 0, 0, 0, &x0, &y0)
        || !FPDF_DeviceToPage(page, 0, 0, width, height, 0, width, 0, &xx, &yx)
        || !FPDF_DeviceToPage(page, 0, 0, width, height, 0, 0, height, &xy, &yy))
        return {};
    QTransform transform((xx - x0) / size.width(), (yx - y0) / size.width(),
        (xy - x0) / size.height(), (yy - y0) / size.height(), x0, y0);
    ok = finite(transform.determinant()) && std::abs(transform.determinant()) > 1e-12;
    return transform;
}

// Render at the native pixel size to validate the PDF image and its soft mask.
// The original PNG in metadata retains alpha exactly when
// the user changes opacity later; recovering it from a faded bitmap is lossy.
bool imagePixelsMatch(FPDF_DOCUMENT document, FPDF_PAGE page,
                      FPDF_PAGEOBJECT object, const QImage &source, qreal opacity)
{
    unsigned int width = 0, height = 0;
    FS_MATRIX matrix{};
    if (!FPDFImageObj_GetImagePixelSize(object, &width, &height)
        || width != unsigned(source.width()) || height != unsigned(source.height())
        || !FPDFPageObj_GetMatrix(object, &matrix))
        return false;
    if (!FPDFImageObj_SetMatrix(object, width, 0, 0, height, 0, 0)) {
        FPDFPageObj_SetMatrix(object, &matrix);
        return false;
    }
    const FPDF_BITMAP bitmap = FPDFImageObj_GetRenderedBitmap(document, page, object);
    FPDFPageObj_SetMatrix(object, &matrix);
    if (!bitmap)
        return false;
    const int format = FPDFBitmap_GetFormat(bitmap);
    bool match = FPDFBitmap_GetWidth(bitmap) == source.width()
        && FPDFBitmap_GetHeight(bitmap) == source.height()
        && (format == FPDFBitmap_BGRA || format == FPDFBitmap_BGRx
            || format == FPDFBitmap_BGRA_Premul);
    if (match) {
        const QImage::Format qformat = format == FPDFBitmap_BGRA_Premul
            ? QImage::Format_ARGB32_Premultiplied
            : (format == FPDFBitmap_BGRA ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        const QImage actual(static_cast<const uchar *>(FPDFBitmap_GetBuffer(bitmap)),
            source.width(), source.height(), FPDFBitmap_GetStride(bitmap), qformat);
        for (int y = 0; y < source.height() && match; ++y) {
            const auto *a = reinterpret_cast<const QRgb *>(actual.constScanLine(y));
            const auto *b = reinterpret_cast<const QRgb *>(source.constScanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                const QRgb actualPixel = format == FPDFBitmap_BGRA_Premul ? a[x]
                    : qPremultiply(format == FPDFBitmap_BGRx ? a[x] | 0xff000000U : a[x]);
                const QRgb expectedPixel = qPremultiply(qRgba(qRed(b[x]), qGreen(b[x]),
                    qBlue(b[x]), qRound(qAlpha(b[x]) * opacity)));
                if (std::abs(qRed(actualPixel) - qRed(expectedPixel)) > 2
                    || std::abs(qGreen(actualPixel) - qGreen(expectedPixel)) > 2
                    || std::abs(qBlue(actualPixel) - qBlue(expectedPixel)) > 2
                    || std::abs(qAlpha(actualPixel) - qAlpha(expectedPixel)) > 2) {
                    match = false;
                    break;
                }
            }
        }
    }
    FPDFBitmap_Destroy(bitmap);
    return match;
}

bool decodeImage(const QString &encoded, QImage &image, ReadBudget &budget)
{
    if (encoded.isEmpty() || encoded.size() > MaximumMetadataBytes)
        return false;
    const auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > MaximumImageBytes)
        return false;
    QBuffer buffer;
    buffer.setData(decoded.decoded);
    if (!buffer.open(QIODevice::ReadOnly))
        return false;
    QImageReader reader(&buffer, "PNG");
    if (!validImageSize(reader.size()))
        return false;
    budget.decodedImageBytes -= qint64(reader.size().width()) * reader.size().height() * 4;
    if (budget.decodedImageBytes < 0) {
        budget.exhausted = true;
        return false;
    }
    image = reader.read().convertToFormat(QImage::Format_ARGB32);
    return !image.isNull() && validImageSize(image.size());
}

QJsonArray imageMatrix(FPDF_PAGE page, const Spec &spec)
{
    const QSizeF size(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page));
    bool ok = false;
    const QTransform toPage = pageMapping(page, size, ok);
    if (!ok || spec.image.isNull())
        return {};
    const QTransform positioned = placement(spec, size);
    const QPointF bottomLeft = toPage.map(positioned.map(QPointF(0, spec.image.height())));
    const QPointF bottomRight = toPage.map(positioned.map(QPointF(spec.image.width(), spec.image.height())));
    const QPointF topLeft = toPage.map(positioned.map(QPointF(0, 0)));
    return {bottomRight.x() - bottomLeft.x(), bottomRight.y() - bottomLeft.y(),
        topLeft.x() - bottomLeft.x(), topLeft.y() - bottomLeft.y(),
        bottomLeft.x(), bottomLeft.y()};
}

bool readManaged(FPDF_DOCUMENT document, FPDF_PAGE page, FPDF_PAGEOBJECT object,
                 Spec &spec, QByteArray &fingerprint, ReadBudget &budget)
{
    if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_PATH) {
        const int segments = FPDFPath_CountSegments(object);
        if (segments < 0)
            return false;
        budget.segments -= segments;
        if (budget.segments < 0) {
            budget.exhausted = true;
            return false;
        }
    }
    QByteArray raw;
    if (!readBlob(object, raw, budget))
        return false;
    const QJsonDocument json = QJsonDocument::fromJson(raw);
    if (!json.isObject())
        return false;
    const QJsonObject root = json.object();
    if (root.size() != 4 || !root.value(QStringLiteral("version")).isDouble()
        || root.value(QStringLiteral("version")).toDouble() != 1
        || !parseSettings(root.value(QStringLiteral("settings")), spec)
        || !root.value(QStringLiteral("png")).isString()
        || !root.value(QStringLiteral("visual")).isArray())
        return false;
    const QJsonArray visual = visualSignature(object, spec.imageMode);
    if (!signatureMatches(visual, root.value(QStringLiteral("visual")).toArray(), spec.imageMode))
        return false;
    if (spec.imageMode) {
        if (!decodeImage(root.value(QStringLiteral("png")).toString(), spec.image, budget)
            || !numericArrayMatches(visual[0].toArray(), imageMatrix(page, spec), 0.0001, 0.000002)
            || !imagePixelsMatch(document, page, object, spec.image, spec.opacity))
            return false;
    } else {
        const QJsonArray color = visual[1].toArray();
        if (!root.value(QStringLiteral("png")).toString().isEmpty()
            || std::abs(color[3].toInt() - qRound(spec.opacity * 255)) > 1
            || std::abs(color[0].toInt() - spec.color.red()) > 1
            || std::abs(color[1].toInt() - spec.color.green()) > 1
            || std::abs(color[2].toInt() - spec.color.blue()) > 1)
            return false;
    }
    fingerprint = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
    return true;
}

struct BuiltObject {
    FPDF_PAGEOBJECT object = nullptr;
    FPDF_BITMAP bitmap = nullptr;
    ~BuiltObject() {
        if (object) FPDFPageObj_Destroy(object);
        if (bitmap) FPDFBitmap_Destroy(bitmap);
    }
};

bool buildObject(FPDF_DOCUMENT document, FPDF_PAGE page, const Spec &spec,
                 BuiltObject &built)
{
    if (!validSpec(spec))
        return false;
    const QSizeF pageSize(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page));
    bool mappingOk = false;
    const QTransform toPage = pageMapping(page, pageSize, mappingOk);
    if (!mappingOk)
        return false;
    const QTransform positioned = placement(spec, pageSize);
    if (spec.imageMode) {
        built.object = FPDFPageObj_NewImageObj(document);
        if (!built.object)
            return false;
        const QImage source = spec.image.convertToFormat(QImage::Format_ARGB32);
        built.bitmap = FPDFBitmap_Create(source.width(), source.height(), 1);
        if (!built.bitmap)
            return false;
        auto *target = static_cast<uchar *>(FPDFBitmap_GetBuffer(built.bitmap));
        const int stride = FPDFBitmap_GetStride(built.bitmap);
        if (!target || stride < source.width() * 4)
            return false;
        for (int y = 0; y < source.height(); ++y) {
            auto *destination = reinterpret_cast<QRgb *>(target + y * stride);
            const auto *pixels = reinterpret_cast<const QRgb *>(source.constScanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                const QRgb pixel = pixels[x];
                destination[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel),
                    qRound(qAlpha(pixel) * spec.opacity));
            }
        }
        if (!FPDFImageObj_SetBitmap(nullptr, 0, built.object, built.bitmap))
            return false;
        // PDF images have their origin at the lower left; QImage at the top left.
        const QJsonArray matrix = imageMatrix(page, spec);
        if (matrix.size() != 6)
            return false;
        if (!FPDFImageObj_SetMatrix(built.object,
            matrix[0].toDouble(), matrix[1].toDouble(), matrix[2].toDouble(),
            matrix[3].toDouble(), matrix[4].toDouble(), matrix[5].toDouble()))
            return false;
    } else {
        const QPainterPath path = (positioned * toPage).map(spec.textPath);
        for (int i = 0; i < path.elementCount(); ++i) {
            const auto e = path.elementAt(i);
            bool success = false;
            if (i == 0) {
                built.object = FPDFPageObj_CreateNewPath(float(e.x), float(e.y));
                success = built.object;
            } else if (e.isMoveTo()) {
                success = FPDFPath_MoveTo(built.object, float(e.x), float(e.y));
            } else if (e.isLineTo()) {
                success = FPDFPath_LineTo(built.object, float(e.x), float(e.y));
            } else if (e.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
                const auto c2 = path.elementAt(++i), end = path.elementAt(++i);
                success = FPDFPath_BezierTo(built.object, float(e.x), float(e.y),
                    float(c2.x), float(c2.y), float(end.x), float(end.y));
            }
            if (!success)
                return false;
        }
        if (!FPDFPath_SetDrawMode(built.object, FPDF_FILLMODE_WINDING, false))
            return false;
    }
    const QColor color = spec.imageMode ? QColor(Qt::white) : spec.color;
    if (!FPDFPageObj_SetFillColor(built.object, color.red(), color.green(), color.blue(),
        spec.imageMode ? 255 : qRound(spec.opacity * 255)))
        return false;
    FPDFPageObj_SetBlendMode(built.object, "Normal");
    const QJsonArray visual = visualSignature(built.object, spec.imageMode);
    if (visual.isEmpty())
        return false;
    QByteArray png;
    if (spec.imageMode) {
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !spec.image.save(&buffer, "PNG")
            || png.size() > MaximumImageBytes)
            return false;
    }
    const QJsonObject root{{QStringLiteral("version"), 1},
        {QStringLiteral("settings"), settings(spec)},
        {QStringLiteral("png"), QString::fromLatin1(png.toBase64())},
        {QStringLiteral("visual"), visual}};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaximumMetadataBytes)
        return false;
    const FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(built.object, MarkName);
    return mark && FPDFPageObjMark_SetBlobParam(document, built.object, mark, DataKey,
        reinterpret_cast<const unsigned char *>(bytes.constData()), bytes.size());
}

class ScanDocument {
public:
    explicit ScanDocument(const QString &path, const QByteArray &password)
    {
        PDFiumProcessLock lock;
        m_document = FPDF_LoadDocument(QFile::encodeName(path).constData(),
            password.isEmpty() ? nullptr : password.constData());
    }
    ~ScanDocument()
    {
        if (m_document) {
            PDFiumProcessLock lock;
            FPDF_CloseDocument(m_document);
        }
    }
    FPDF_DOCUMENT get() const { return m_document; }
private:
    FPDF_DOCUMENT m_document = nullptr;
};

class PageHandle {
public:
    explicit PageHandle(FPDF_PAGE page) : m_page(page) {}
    ~PageHandle() { if (m_page) FPDF_ClosePage(m_page); }
    FPDF_PAGE get() const { return m_page; }
private:
    FPDF_PAGE m_page;
};
} // namespace

bool validSpec(const Spec &spec, bool requireArtwork)
{
    if (!validId(spec.id) || spec.text.size() > MaximumTextLength
        || spec.fontFamily.size() > 256 || spec.fontFamily.contains(QChar::Null)
        || spec.text.contains(QChar::Null) || !unit(spec.opacity)
        || !unit(spec.x) || !unit(spec.y) || !finite(spec.rotation)
        || spec.rotation < -180 || spec.rotation > 180
        || !finite(spec.scale) || spec.scale < 0.01 || spec.scale > 1
        || !finite(spec.fontSize) || spec.fontSize < 1 || spec.fontSize > 512
        || !spec.color.isValid() || spec.color.alpha() != 255)
        return false;
    if (spec.imageMode)
        return !requireArtwork || (!spec.image.isNull() && validImageSize(spec.image.size()));
    return !spec.text.trimmed().isEmpty() && !spec.fontFamily.trimmed().isEmpty()
        && (!requireArtwork || validPath(spec.textPath));
}

bool prepareText(Spec &spec, QString &error)
{
    error.clear();
    if (!validSpec(spec, false)) {
        error = QStringLiteral("Invalid watermark settings.");
        return false;
    }
    if (spec.imageMode)
        return validSpec(spec);
    // QPainterPath::addText works in device pixels. A fixed 256 pixel font plus
    // a final scale preserves fractional PDF point sizes independently of DPI.
    QFont font(spec.fontFamily);
    font.setPixelSize(256);
    font.setStyleStrategy(QFont::PreferAntialias);
    const QFontMetricsF metrics(font);
    QPainterPath path;
    qreal baseline = 0;
    const QStringList lines = spec.text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        path.addText(QPointF(0, baseline), font, line);
        baseline += metrics.lineSpacing();
    }
    spec.textPath = QTransform::fromScale(spec.fontSize / 256, spec.fontSize / 256).map(path);
    spec.textPath.setFillRule(Qt::WindingFill);
    if (!validPath(spec.textPath)) {
        spec.textPath = {};
        error = QStringLiteral("Watermark text has no supported visible glyphs or is too complex.");
        return false;
    }
    return true;
}

QTransform placement(const Spec &spec, const QSizeF &pageSize)
{
    if (!validPageSize(pageSize))
        return {};
    const QRectF artwork = spec.imageMode ? QRectF(QPointF(), spec.image.size())
                                         : spec.textPath.boundingRect();
    if (artwork.isEmpty())
        return {};
    const qreal angle = spec.rotation * 3.14159265358979323846 / 180;
    const qreal baseScale = spec.imageMode ? pageSize.width() * spec.scale / artwork.width() : 1;
    const qreal margin = qMin<qreal>(12, qMin(pageSize.width(), pageSize.height()) / 4);
    const QSizeF available(pageSize.width() - 2 * margin, pageSize.height() - 2 * margin);
    QTransform rotation(std::cos(angle) * baseScale, std::sin(angle) * baseScale,
        -std::sin(angle) * baseScale, std::cos(angle) * baseScale, 0, 0);
    const QRectF initial = rotation.mapRect(artwork);
    const qreal fit = qMin<qreal>(1, qMin(available.width() / initial.width(),
                                         available.height() / initial.height()));
    QTransform result(rotation.m11() * fit, rotation.m12() * fit,
        rotation.m21() * fit, rotation.m22() * fit, 0, 0);
    const QRectF bounds = result.mapRect(artwork);
    return QTransform(result.m11(), result.m12(), result.m21(), result.m22(),
        margin + (available.width() - bounds.width()) * spec.x - bounds.left(),
        margin + (available.height() - bounds.height()) * spec.y - bounds.top());
}

void paint(QPainter &painter, const Spec &spec, const QSizeF &pageSize)
{
    if (!validSpec(spec) || !validPageSize(pageSize))
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setOpacity(painter.opacity() * spec.opacity);
    painter.setTransform(placement(spec, pageSize), true);
    if (spec.imageMode)
        painter.drawImage(QPointF(), spec.image);
    else
        painter.fillPath(spec.textPath, spec.color);
    painter.restore();
}

bool hasMark(FPDF_PAGEOBJECT object)
{
    assertPDFiumProcessLockHeld();
    if (!object)
        return false;
    const int count = FPDFPageObj_CountMarks(object);
    // Marks are inspected without parsing their potentially large payloads.
    for (int i = 0; i < qMin(count, 256); ++i) {
        if (markerName(FPDFPageObj_GetMark(object, i)))
            return true;
    }
    // Excessive mark nesting is not safe for ordinary content editing either.
    return count > 256;
}

namespace {
ScanResult scanDocument(FPDF_DOCUMENT document,
                        const std::shared_ptr<std::atomic_bool> &cancel,
                        bool acquireLocks)
{
    ScanResult result;
    int pageCount = 0;
    {
        std::unique_ptr<PDFiumProcessLock> lock;
        if (acquireLocks) lock = std::make_unique<PDFiumProcessLock>();
        pageCount = FPDF_GetPageCount(document);
    }
    if (pageCount < 0 || pageCount > MaximumPages) {
        result.error = QStringLiteral("The document exceeds the watermark scan page limit.");
        return result;
    }
    QHash<QString, int> groups;
    QSet<QString> rejected;
    qint64 objectBudget = MaximumScanObjects;
    ReadBudget readBudget;
    qsizetype retained = 0;
    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        if (cancelled(cancel))
            return {};
        std::unique_ptr<PDFiumProcessLock> lock;
        if (acquireLocks) lock = std::make_unique<PDFiumProcessLock>();
        const PageHandle page(FPDF_LoadPage(document, pageIndex));
        if (!page.get()) {
            result.error = QStringLiteral("Could not read a page during watermark scanning.");
            result.groups.clear();
            return result;
        }
        const int count = FPDFPage_CountObjects(page.get());
        if (count < 0 || count > MaximumPageObjects || (objectBudget -= count) < 0) {
            result.error = QStringLiteral("The document exceeds the watermark scan object limit.");
            result.groups.clear();
            return result;
        }
        QSet<QString> pageIds;
        for (int i = 0; i < count; ++i) {
            if (cancelled(cancel))
                return {};
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page.get(), i);
            if (!hasMark(object))
                continue;
            Spec spec;
            QByteArray fingerprint;
            const bool safe = readManaged(document, page.get(), object, spec, fingerprint, readBudget);
            if (readBudget.exhausted) {
                result.error = QStringLiteral("The document exceeds the watermark artwork scan limit.");
                result.groups.clear();
                return result;
            }
            if (!safe) {
                if (validId(spec.id))
                    rejected.insert(spec.id);
                continue;
            }
            if (pageIds.contains(spec.id)) {
                rejected.insert(spec.id);
                continue;
            }
            pageIds.insert(spec.id);
            if (!groups.contains(spec.id)) {
                const qsizetype bytes = spec.image.sizeInBytes();
                if (groups.size() >= MaximumGroups || bytes > MaximumRetainedBytes - retained) {
                    result.error = QStringLiteral("The document exceeds the watermark group or image memory limit.");
                    result.groups.clear();
                    return result;
                }
                retained += bytes;
                groups.insert(spec.id, result.groups.size());
                result.groups.append(Group{spec, {}, {}});
            }
            Group &group = result.groups[groups.value(spec.id)];
            if (settings(group.spec) != settings(spec)
                || (spec.imageMode && group.spec.image != spec.image)) {
                rejected.insert(spec.id);
                continue;
            }
            group.pages.insert(pageIndex);
            group.fingerprints.insert(pageIndex, fingerprint);
        }
    }
    result.groups.removeIf([&rejected](const Group &group) { return rejected.contains(group.spec.id); });
    result.succeeded = !cancelled(cancel);
    return result;
}
} // namespace

ScanResult scan(const QString &path, const QByteArray &password,
                const std::shared_ptr<std::atomic_bool> &cancel)
{
    assertPDFiumProcessLockNotHeld();
    if (cancelled(cancel))
        return {};
    const ScanDocument document(path, password);
    if (!document.get()) {
        ScanResult result;
        result.error = QStringLiteral("Could not open the PDF for watermark scanning.");
        return result;
    }
    return scanDocument(document.get(), cancel, true);
}

bool validateDocument(FPDF_DOCUMENT document, QString &error)
{
    assertPDFiumProcessLockHeld();
    error.clear();
    if (!document) {
        error = QStringLiteral("Invalid watermark staging document.");
        return false;
    }
    const ScanResult result = scanDocument(document, {}, false);
    error = result.error;
    return result.succeeded;
}

bool apply(FPDF_DOCUMENT document, FPDF_PAGE page,
           const QVector<PageChange> &changes, bool preview, QString &error)
{
    assertPDFiumProcessLockHeld();
    error.clear();
    if (changes.isEmpty())
        return true;
    const auto fail = [&error](const char *message) { error = QString::fromUtf8(message); return false; };
    if (!document || !page || changes.size() > MaximumGroups)
        return fail("Invalid watermark change batch.");
    const int count = FPDFPage_CountObjects(page);
    if (count < 0 || count > MaximumPageObjects)
        return fail("The page exceeds the watermark object limit.");
    QHash<QString, const PageChange *> requested;
    for (const PageChange &change : changes) {
        if (!validId(change.spec.id) || requested.contains(change.spec.id)
            || (change.add && !validSpec(change.spec)))
            return fail("Invalid or duplicate watermark settings.");
        requested.insert(change.spec.id, &change);
    }
    QHash<QString, FPDF_PAGEOBJECT> originals;
    ReadBudget readBudget;
    for (int i = 0; i < count; ++i) {
        const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
        if (!hasMark(object))
            continue;
        Spec source;
        QByteArray fingerprint;
        const bool safe = readManaged(document, page, object, source, fingerprint, readBudget);
        if (readBudget.exhausted)
            return fail("The page exceeds the watermark artwork validation limit.");
        const auto found = requested.constFind(source.id);
        if (found == requested.cend())
            continue;
        if (!safe || originals.contains(source.id)
            || (*found)->expectedFingerprint.isEmpty()
            || (*found)->expectedFingerprint != fingerprint)
            return fail("The source watermark changed or is no longer safe to edit.");
        originals.insert(source.id, object);
    }
    std::vector<std::unique_ptr<BuiltObject>> built;
    built.reserve(changes.size());
    qsizetype bitmapBytes = 0;
    for (const PageChange &change : changes) {
        if (!change.expectedFingerprint.isEmpty() && !originals.contains(change.spec.id))
            return fail("The source watermark could not be found.");
        auto next = std::make_unique<BuiltObject>();
        if (change.add && change.spec.imageMode) {
            bitmapBytes += qint64(change.spec.image.width()) * change.spec.image.height() * 4;
            if (bitmapBytes > MaximumRetainedBytes)
                return fail("The page exceeds the watermark image memory limit.");
        }
        if (change.add && !buildObject(document, page, change.spec, *next))
            return fail("Could not prepare watermark artwork for this page.");
        built.push_back(std::move(next));
    }
    // All source identities and replacements are validated before mutations.
    // A caller must discard this independent document if any mutation fails.
    for (auto object : originals) {
        if (preview) {
            if (!FPDFPageObj_SetIsActive(object, false))
                return fail("Could not hide the original watermark preview.");
        } else {
            if (!FPDFPage_RemoveObject(page, object))
                return fail("Could not remove the original watermark.");
            FPDFPageObj_Destroy(object);
        }
    }
    size_t index = 0;
    for (const PageChange &change : changes) {
        BuiltObject &next = *built[index++];
        if (!next.object)
            continue;
        const int insertion = change.spec.behind ? 0 : FPDFPage_CountObjects(page);
        if (!FPDFPage_InsertObjectAtIndex(page, next.object, insertion))
            return fail("Could not insert watermark artwork.");
        next.object = nullptr; // The page owns it now; retain bitmap until generation.
    }
    if (!FPDFPage_GenerateContent(page))
        return fail("Could not generate the page with its watermark changes.");
    return true;
}
} // namespace PDFWatermarks
