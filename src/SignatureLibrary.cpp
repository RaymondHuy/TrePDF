#include "SignatureLibrary.h"

#include "PDFEditController.h"

#include <QBuffer>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QtTranslation>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kMaximumEntries = 32;
constexpr int kMaximumNameLength = 80;
constexpr int kMaximumTextLength = 128;
constexpr int kMaximumImageEdge = 1536;
constexpr qint64 kMaximumImagePixels = 1024 * 1024;
constexpr qint64 kMaximumSourcePixels = 16 * 1024 * 1024;
constexpr qint64 kMaximumSourceBytes = 32 * 1024 * 1024;
constexpr qsizetype kMaximumPngBytes = 2 * 1024 * 1024;
constexpr qint64 kMaximumLibraryBytes = 24 * 1024 * 1024;
constexpr int kMaximumStrokes = 64;
constexpr int kMaximumStrokePoints = 4096;
constexpr int kMaximumTotalPoints = 32768;
constexpr qsizetype kMaximumSimplificationComparisons = 200000;
constexpr qsizetype kMaximumRenderedPoints = 2048;
constexpr qreal kMaximumRenderedLength = 32768.0;
constexpr int kDrawingWidth = 1536;
constexpr int kDrawingHeight = 512;

bool validSize(const QSize &size, qint64 maximumPixels)
{
    return size.width() > 0 && size.height() > 0
        && qint64(size.width()) * size.height() <= maximumPixels;
}

QUrl imageUrl(const QByteArray &png)
{
    return QUrl(QStringLiteral("data:image/png;base64,")
                + QString::fromLatin1(png.toBase64()));
}

bool validKind(const QString &kind)
{
    return kind == QLatin1String("signature") || kind == QLatin1String("initials");
}

bool printable(const QString &text)
{
    return std::none_of(text.cbegin(), text.cend(), [](QChar character) {
        return character.isNull() || character.category() == QChar::Other_Control;
    });
}

QImage decodePng(const QByteArray &png)
{
    if (png.isEmpty() || png.size() > kMaximumPngBytes) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(png);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer, "png");
    const QSize size = reader.size();
    if (!validSize(size, kMaximumImagePixels)
        || size.width() > kMaximumImageEdge || size.height() > kMaximumImageEdge) {
        return {};
    }
    QImage image = reader.read();
    if (image.size() != size) {
        return {};
    }
    return image.convertToFormat(QImage::Format_ARGB32);
}

// Iterative RDP shares a work budget across all strokes. If simplification gets
// expensive, retain the bounded original points instead of changing the drawing.
QVector<QPointF> simplifyStroke(const QVector<QPointF> &points,
                              qsizetype &remainingComparisons)
{
    if (points.size() <= 2) {
        return points;
    }
    QVector<bool> keep(points.size(), false);
    keep.front() = true;
    keep.back() = true;
    QVector<QPair<qsizetype, qsizetype>> ranges{{0, points.size() - 1}};
    while (!ranges.isEmpty()) {
        const auto range = ranges.takeLast();
        const QPointF first = points.at(range.first);
        const QPointF direction = points.at(range.second) - first;
        const qreal lengthSquared = QPointF::dotProduct(direction, direction);
        qreal maximumDistanceSquared = 0.75 * 0.75;
        qsizetype furthest = -1;
        for (qsizetype index = range.first + 1; index < range.second; ++index) {
            if (remainingComparisons == 0) {
                return points;
            }
            --remainingComparisons;
            const QPointF offset = points.at(index) - first;
            const qreal fraction = lengthSquared > 0.0
                ? qBound<qreal>(0.0, QPointF::dotProduct(offset, direction) / lengthSquared, 1.0)
                : 0.0;
            const QPointF residual = offset - direction * fraction;
            const qreal distanceSquared = QPointF::dotProduct(residual, residual);
            if (distanceSquared > maximumDistanceSquared) {
                maximumDistanceSquared = distanceSquared;
                furthest = index;
            }
        }
        if (furthest >= 0) {
            keep[furthest] = true;
            ranges.append({range.first, furthest});
            ranges.append({furthest, range.second});
        }
    }
    QVector<QPointF> result;
    result.reserve(points.size());
    for (qsizetype index = 0; index < points.size(); ++index) {
        if (keep.at(index)) {
            result.append(points.at(index));
        }
    }
    return result;
}

} // namespace

SignatureLibrary::SignatureLibrary(QObject *parent)
    : SignatureLibrary(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                           .filePath(QStringLiteral("signatures.json")), parent)
{
}

SignatureLibrary::SignatureLibrary(const QString &storagePath, QObject *parent)
    : QObject(parent), m_storagePath(storagePath)
{
    load();
}

QVariantList SignatureLibrary::entries() const
{
    QVariantList result;
    result.reserve(m_entries.size());
    for (const Entry &entry : m_entries) {
        result.append(QVariantMap{{QStringLiteral("id"), entry.id},
                                  {QStringLiteral("name"), entry.name},
                                  {QStringLiteral("kind"), entry.kind},
                                  {QStringLiteral("previewUrl"), imageUrl(entry.png)}});
    }
    return result;
}

void SignatureLibrary::setError(const QString &message)
{
    if (m_error != message) {
        m_error = message;
        emit errorChanged();
    }
}

bool SignatureLibrary::fail(const QString &message)
{
    setError(message);
    return false;
}

void SignatureLibrary::clearPreview()
{
    setError({});
    if (m_preview.isNull() && m_previewUrl.isEmpty()) {
        return;
    }
    m_preview = {};
    m_previewPng.clear();
    m_previewUrl = QUrl();
    emit previewChanged();
}

bool SignatureLibrary::setPreview(QImage image)
{
    if (image.isNull()) {
        //% "Create or choose a signature before placing it."
        return fail(qtTrId("ESignPreviewRequired"));
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(pixels[x]) > 0) {
                left = qMin(left, x);
                right = qMax(right, x);
                top = qMin(top, y);
                bottom = qMax(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) {
        //% "The signature is empty. Add text, draw a stroke, or choose another image."
        return fail(qtTrId("ESignEmptySignature"));
    }
    const QRect crop = QRect(QPoint(left, top), QPoint(right, bottom))
                           .adjusted(-4, -4, 4, 4).intersected(image.rect());
    image = image.copy(crop);
    const qreal scale = qMin<qreal>(1.0, qMin(
        qreal(kMaximumImageEdge) / qMax(image.width(), image.height()),
        std::sqrt(qreal(kMaximumImagePixels) / (qreal(image.width()) * image.height()))));
    if (scale < 1.0) {
        image = image.scaled(qMax(1, int(std::floor(image.width() * scale))),
                             qMax(1, int(std::floor(image.height() * scale))),
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")
        || png.size() > kMaximumPngBytes) {
        //% "The signature image is too complex. Choose a smaller image."
        return fail(qtTrId("ESignImageTooComplex"));
    }
    m_preview = std::move(image);
    m_previewPng = std::move(png);
    m_previewUrl = imageUrl(m_previewPng);
    setError({});
    emit previewChanged();
    return true;
}

bool SignatureLibrary::prepareText(const QString &text, int style, const QColor &color)
{
    clearPreview();
    const QString normalized = text.trimmed();
    if (normalized.isEmpty() || normalized.size() > kMaximumTextLength
        || !printable(normalized) || style < 0 || style > 2
        || !color.isValid() || color.alpha() == 0) {
        //% "Enter a signature of 1 to 128 characters and choose a visible color."
        return fail(qtTrId("ESignInvalidText"));
    }
    QFont font;
    font.setPixelSize(128);
    if (style == 0) {
        const QStringList families = QFontDatabase::families();
        const QStringList preferred{QStringLiteral("Snell Roundhand"),
                                    QStringLiteral("Apple Chancery"),
                                    QStringLiteral("Segoe Script"),
                                    QStringLiteral("URW Chancery L")};
        bool found = false;
        for (const QString &family : preferred) {
            if (families.contains(family, Qt::CaseInsensitive)) {
                font.setFamily(family);
                found = true;
                break;
            }
        }
        if (!found) {
            font.setFamily(QStringLiteral("Times New Roman"));
            font.setStyleHint(QFont::Serif);
            font.setItalic(true);
        }
    } else if (style == 1) {
        font.setFamily(QStringLiteral("Times New Roman"));
        font.setStyleHint(QFont::Serif);
        font.setItalic(true);
    } else {
        font.setFamily(QStringLiteral("Helvetica"));
        font.setStyleHint(QFont::SansSerif);
    }
    QPainterPath path;
    path.addText(0.0, 0.0, font, normalized);
    const QRectF bounds = path.boundingRect();
    if (bounds.isEmpty() || !std::isfinite(bounds.width()) || !std::isfinite(bounds.height())) {
        //% "The selected text could not be drawn. Try another signature style."
        return fail(qtTrId("ESignTextRenderFailed"));
    }
    const qreal scale = qMin<qreal>(2.0, qMin(1520.0 / bounds.width(), 1000.0 / bounds.height()));
    QImage image(QSize(int(std::ceil(bounds.width() * scale)) + 16,
                       int(std::ceil(bounds.height() * scale)) + 16), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(8.0, 8.0);
    painter.scale(scale, scale);
    painter.translate(-bounds.topLeft());
    painter.fillPath(path, color);
    painter.end();
    return setPreview(std::move(image));
}

bool SignatureLibrary::prepareDrawing(const QVariantList &strokes,
                                      const QColor &color, qreal strokeWidth)
{
    clearPreview();
    const auto invalidDrawing = [this]() {
        //% "Draw a signature with at most 64 strokes and 32,768 points."
        return fail(qtTrId("ESignInvalidDrawing"));
    };
    if (strokes.isEmpty() || strokes.size() > kMaximumStrokes || !color.isValid()
        || color.alpha() == 0 || !std::isfinite(strokeWidth)
        || strokeWidth < 0.5 || strokeWidth > 12.0) {
        return invalidDrawing();
    }
    QVector<QVector<QPointF>> paths;
    qsizetype totalPoints = 0;
    qsizetype remainingComparisons = kMaximumSimplificationComparisons;
    qsizetype renderedPoints = 0;
    qreal renderedLength = 0.0;
    for (const QVariant &strokeValue : strokes) {
        const QVariantList stroke = strokeValue.toList();
        totalPoints += stroke.size();
        if (stroke.isEmpty() || stroke.size() > kMaximumStrokePoints
            || totalPoints > kMaximumTotalPoints) {
            return invalidDrawing();
        }
        QVector<QPointF> points;
        points.reserve(stroke.size());
        for (const QVariant &pointValue : stroke) {
            bool xValid = false;
            bool yValid = false;
            qreal x = 0.0;
            qreal y = 0.0;
            if (pointValue.metaType() == QMetaType::fromType<QPointF>()) {
                const QPointF point = pointValue.toPointF();
                x = point.x();
                y = point.y();
                xValid = yValid = true;
            } else {
                const QVariantMap point = pointValue.toMap();
                x = point.value(QStringLiteral("x")).toDouble(&xValid);
                y = point.value(QStringLiteral("y")).toDouble(&yValid);
            }
            if (!xValid || !yValid || !std::isfinite(x) || !std::isfinite(y)
                || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                return invalidDrawing();
            }
            const QPointF next(x * (kDrawingWidth - 1), y * (kDrawingHeight - 1));
            if (points.isEmpty() || next != points.constLast()) {
                points.append(next);
            }
        }
        QVector<QPointF> simplified = simplifyStroke(points, remainingComparisons);
        renderedPoints += simplified.size();
        for (qsizetype index = 1; index < simplified.size(); ++index) {
            const QPointF delta = simplified.at(index) - simplified.at(index - 1);
            renderedLength += std::hypot(delta.x(), delta.y());
        }
        // Dense overlapping paths can be expensive for the antialiased rasterizer,
        // even when raw input and simplification work are bounded.
        if (renderedPoints > kMaximumRenderedPoints
            || renderedLength > kMaximumRenderedLength) {
            //% "This drawing is too complex. Remove a few strokes or use a signature image."
            return fail(qtTrId("ESignDrawingTooComplex"));
        }
        paths.append(std::move(simplified));
    }
    QImage image(kDrawingWidth, kDrawingHeight, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    // The UI drawing pad is 900 x 300 logical units; preserve its physical pen width.
    painter.setPen(QPen(color, strokeWidth * kDrawingWidth / 900.0,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const QVector<QPointF> &points : paths) {
        if (points.size() == 1) {
            painter.drawPoint(points.constFirst());
            continue;
        }
        QPainterPath path(points.constFirst());
        for (qsizetype index = 1; index < points.size(); ++index) {
            path.lineTo(points.at(index));
        }
        painter.drawPath(path);
    }
    painter.end();
    return setPreview(std::move(image));
}

bool SignatureLibrary::prepareImage(const QUrl &source, bool removeWhite)
{
    clearPreview();
    if (!source.isLocalFile() || source.toLocalFile().isEmpty()) {
        //% "Choose a local signature image."
        return fail(qtTrId("ESignLocalImageOnly"));
    }
    const QFileInfo file(source.toLocalFile());
    if (!file.isFile() || !file.isReadable() || file.size() <= 0) {
        //% "The signature image could not be opened."
        return fail(qtTrId("ESignImageOpenFailed"));
    }
    QImageReader reader(file.absoluteFilePath());
    reader.setAutoTransform(true);
    if (file.size() > kMaximumSourceBytes || !validSize(reader.size(), kMaximumSourcePixels)) {
        //% "Choose a signature image up to 16 megapixels and 32 MiB."
        return fail(qtTrId("ESignImageTooLarge"));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        //% "The signature image could not be opened."
        return fail(qtTrId("ESignImageOpenFailed"));
    }
    if (!validSize(image.size(), kMaximumSourcePixels)) {
        //% "Choose a signature image up to 16 megapixels and 32 MiB."
        return fail(qtTrId("ESignImageTooLarge"));
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    if (removeWhite) {
        for (int y = 0; y < image.height(); ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = pixels[x];
                const int whiteness = qMin(qRed(pixel), qMin(qGreen(pixel), qBlue(pixel)));
                if (whiteness >= 235) {
                    // A small fade band avoids hard edges around antialiased handwriting.
                    const int alpha = qAlpha(pixel) * (255 - whiteness) / 20;
                    pixels[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel), alpha);
                }
            }
        }
    }
    return setPreview(std::move(image));
}

bool SignatureLibrary::prepareDate(const QString &isoDate, int format)
{
    clearPreview();
    const QDate date = QDate::fromString(isoDate, Qt::ISODate);
    if (!date.isValid() || date.toString(Qt::ISODate) != isoDate || format < 0 || format > 2) {
        //% "Enter a valid date in YYYY-MM-DD format."
        return fail(qtTrId("ESignInvalidDate"));
    }
    const QString pattern = format == 0 ? QStringLiteral("dd/MM/yyyy")
        : format == 1 ? QStringLiteral("MM/dd/yyyy") : QStringLiteral("yyyy-MM-dd");
    return prepareText(date.toString(pattern), 2, QColor(Qt::black));
}

bool SignatureLibrary::prepareCheck(bool cross)
{
    clearPreview();
    QImage image(256, 256, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, 22.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (cross) {
        painter.drawLine(QPointF(44, 44), QPointF(212, 212));
        painter.drawLine(QPointF(212, 44), QPointF(44, 212));
    } else {
        QPainterPath path(QPointF(32, 128));
        path.lineTo(100, 196);
        path.lineTo(226, 48);
        painter.drawPath(path);
    }
    painter.end();
    return setPreview(std::move(image));
}

bool SignatureLibrary::savePreview(const QString &name, const QString &kind)
{
    if (!hasPreview()) {
        //% "Create or choose a signature before placing it."
        return fail(qtTrId("ESignPreviewRequired"));
    }
    const QString normalized = name.trimmed();
    if (normalized.isEmpty() || normalized.size() > kMaximumNameLength
        || !printable(normalized) || !validKind(kind)) {
        //% "Give the signature a name of 1 to 80 characters."
        return fail(qtTrId("ESignInvalidName"));
    }
    if (m_entries.size() >= kMaximumEntries) {
        //% "Your signature library is full. Remove an entry before adding another (maximum 32)."
        return fail(qtTrId("ESignLibraryFull"));
    }
    QVector<Entry> next = m_entries;
    next.append({QUuid::createUuid().toString(QUuid::WithoutBraces), normalized, kind, m_previewPng});
    if (!persist(next)) {
        return false;
    }
    m_entries = std::move(next);
    setError({});
    emit entriesChanged();
    return true;
}

bool SignatureLibrary::selectEntry(const QString &id)
{
    clearPreview();
    for (const Entry &entry : m_entries) {
        if (entry.id == id) {
            return setPreview(decodePng(entry.png));
        }
    }
    //% "This saved signature is no longer available."
    return fail(qtTrId("ESignEntryMissing"));
}

bool SignatureLibrary::removeEntry(const QString &id)
{
    QVector<Entry> next = m_entries;
    const auto entry = std::find_if(next.cbegin(), next.cend(), [&id](const Entry &item) {
        return item.id == id;
    });
    if (entry == next.cend()) {
        //% "This saved signature is no longer available."
        return fail(qtTrId("ESignEntryMissing"));
    }
    next.removeAt(std::distance(next.cbegin(), entry));
    if (!persist(next)) {
        return false;
    }
    m_entries = std::move(next);
    setError({});
    emit entriesChanged();
    return true;
}

QVariantMap SignatureLibrary::place(PDFEditController *controller, int page,
                                   qreal x, qreal y, qreal widthPoints)
{
    if (!controller || !hasPreview()) {
        //% "Create or choose a signature before placing it."
        fail(qtTrId("ESignPreviewRequired"));
        return {};
    }
    if (!std::isfinite(widthPoints) || widthPoints < 1.0 || widthPoints > 2000.0) {
        //% "Choose a signature width between 1 and 2,000 PDF points."
        fail(qtTrId("ESignInvalidWidth"));
        return {};
    }
    const QVariantMap result = controller->addImageData(page, x, y, m_preview, widthPoints);
    if (result.isEmpty()) {
        //% "The signature could not be placed. Check that this PDF permits editing and is not being saved."
        fail(qtTrId("ESignPlacementFailed"));
    } else {
        setError({});
    }
    return result;
}

void SignatureLibrary::load()
{
    QFile file(m_storagePath);
    if (!file.exists()) {
        return;
    }
    const auto invalidLibrary = [this]() {
        m_storageReadable = false;
        //% "The saved signature library could not be read. Its file has been left unchanged."
        fail(qtTrId("ESignLibraryReadFailed"));
    };
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kMaximumLibraryBytes) {
        invalidLibrary();
        return;
    }
    const QByteArray bytes = file.read(kMaximumLibraryBytes + 1);
    if (bytes.size() > kMaximumLibraryBytes || file.error() != QFileDevice::NoError) {
        invalidLibrary();
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const QJsonObject object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || object.value(QStringLiteral("version")).toInt(-1) != 1
        || !object.value(QStringLiteral("entries")).isArray()) {
        invalidLibrary();
        return;
    }
    const QJsonArray savedEntries = object.value(QStringLiteral("entries")).toArray();
    if (savedEntries.size() > kMaximumEntries) {
        invalidLibrary();
        return;
    }
    QVector<Entry> entries;
    QSet<QString> ids;
    for (const QJsonValue &value : savedEntries) {
        const QJsonObject saved = value.toObject();
        Entry entry{saved.value(QStringLiteral("id")).toString(),
                    saved.value(QStringLiteral("name")).toString(),
                    saved.value(QStringLiteral("kind")).toString(), {}};
        const QString encoded = saved.value(QStringLiteral("png")).toString();
        const QUuid uuid(entry.id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != entry.id
            || ids.contains(entry.id) || entry.name.trimmed().isEmpty()
            || entry.name.size() > kMaximumNameLength || !printable(entry.name)
            || !validKind(entry.kind) || encoded.size() > (kMaximumPngBytes + 2) / 3 * 4) {
            invalidLibrary();
            return;
        }
        const auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decodePng(decoded.decoded).isNull()) {
            invalidLibrary();
            return;
        }
        entry.png = decoded.decoded;
        ids.insert(entry.id);
        entries.append(std::move(entry));
    }
    m_entries = std::move(entries);
}

bool SignatureLibrary::persist(const QVector<Entry> &entries)
{
    if (!m_storageReadable) {
        //% "The saved signature library could not be read. Its file has been left unchanged."
        return fail(qtTrId("ESignLibraryReadFailed"));
    }
    QJsonArray values;
    for (const Entry &entry : entries) {
        values.append(QJsonObject{{QStringLiteral("id"), entry.id},
                                  {QStringLiteral("name"), entry.name},
                                  {QStringLiteral("kind"), entry.kind},
                                  {QStringLiteral("png"), QString::fromLatin1(entry.png.toBase64())}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                      {QStringLiteral("entries"), values}})
                                 .toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaximumLibraryBytes) {
        //% "The signature library is too large. Remove an entry or choose a simpler image."
        return fail(qtTrId("ESignLibraryTooLarge"));
    }
    const auto writeFailed = [this]() {
        //% "The signature library could not be saved. Check the available disk space and folder permissions."
        return fail(qtTrId("ESignLibraryWriteFailed"));
    };
    if (m_storagePath.isEmpty() || !QDir().mkpath(QFileInfo(m_storagePath).absolutePath())) {
        return writeFailed();
    }
    QSaveFile file(m_storagePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        return writeFailed();
    }
    return true;
}
