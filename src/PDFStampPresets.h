#pragma once

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QString>
#include <QTransform>

#include <algorithm>
#include <array>

// Fixed artwork labels are part of the PDF stamp, independent of UI language.
// Both QML and the PDF appearance use these same 160 x 50 point vector paths.
namespace PDFStampPresets {

// An exact leading PDF comment identifies the artwork layout generated here.
inline constexpr char kAppearanceMarker[] = "%TrePDFStampArtworkV1\n";
inline constexpr int kMaximumArtworkElements = 32'768;

struct Preset {
    const char *name;
    const char *text;
    const char *category;
    int red;
    int green;
    int blue;
    bool dynamic = false;
};

inline constexpr std::array<Preset, 24> presets{{
    {"DynamicApproved", "APPROVED", "dynamic", 29, 174, 137, true},
    {"DynamicRevised", "REVISED", "dynamic", 29, 174, 137, true},
    {"DynamicReviewed", "REVIEWED", "dynamic", 29, 174, 137, true},
    {"DynamicReceived", "RECEIVED", "dynamic", 29, 174, 137, true},
    {"DynamicConfidential", "CONFIDENTIAL", "dynamic", 29, 174, 137, true},
    {"Approved", "APPROVED", "standard", 32, 110, 180},
    {"Void", "VOID", "standard", 225, 58, 40},
    {"Verified", "VERIFIED", "standard", 32, 110, 180},
    {"Revised", "REVISED", "standard", 32, 110, 180},
    {"Reviewed", "REVIEWED", "standard", 32, 110, 180},
    {"Received", "RECEIVED", "standard", 32, 110, 180},
    {"Final", "FINAL", "standard", 32, 110, 180},
    {"Expired", "EXPIRED", "standard", 32, 110, 180},
    {"Emergency", "EMERGENCY", "standard", 225, 58, 40},
    {"Draft", "DRAFT", "standard", 32, 110, 180},
    {"Confidential", "CONFIDENTIAL", "standard", 32, 110, 180},
    {"Completed", "COMPLETED", "standard", 32, 110, 180},
    {"Accepted", "ACCEPTED", "signHere", 29, 174, 137},
    {"Witness", "WITNESS", "signHere", 29, 174, 137},
    {"SignHere", "SIGN HERE", "signHere", 29, 174, 137},
    {"Rejected", "REJECTED", "signHere", 225, 58, 40},
    {"Initial", "INITIAL", "signHere", 29, 174, 137},
    {"ForComment", "FOR COMMENT", "additional", 239, 108, 0},
    {"NotApproved", "NOT APPROVED", "additional", 198, 40, 40},
}};

inline const Preset *find(const QString &name)
{
    const auto match = std::find_if(presets.cbegin(), presets.cend(),
        [&name](const Preset &preset) { return name == QLatin1String(preset.name); });
    return match == presets.cend() ? nullptr : &*match;
}

inline QString detailText(const QString &author, const QDateTime &created)
{
    QString displayAuthor = author.simplified().toUpper().left(64);
    if (!displayAuthor.isEmpty() && displayAuthor.back().isHighSurrogate()) {
        displayAuthor.chop(1);
    }
    // Keep the captured timestamp's offset; reopening in another timezone must
    // not change the date printed on the stamp.
    return displayAuthor + QStringLiteral(", ")
        + created.toString(QStringLiteral("dd/MM/yy, HH:mm:ss"));
}

inline QPainterPath stroke(const QPainterPath &path, qreal width = 1.5)
{
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(path);
}

inline QPainterPath icon(const QString &name)
{
    QPainterPath lines;
    QPainterPath fill;
    const auto line = [&lines](qreal x1, qreal y1, qreal x2, qreal y2) {
        lines.moveTo(x1, y1);
        lines.lineTo(x2, y2);
    };
    if (name == QLatin1String("Approved")) {
        lines.moveTo(3, 12); lines.lineTo(9, 18); lines.lineTo(21, 5);
    } else if (name == QLatin1String("Void")) {
        line(5, 5, 19, 19); line(19, 5, 5, 19);
    } else if (name == QLatin1String("Verified")
               || name == QLatin1String("Emergency")) {
        lines.moveTo(12, 2); lines.lineTo(23, 22);
        lines.lineTo(1, 22); lines.closeSubpath();
        if (name == QLatin1String("Emergency")) {
            line(12, 8, 12, 14);
            fill.addEllipse(QPointF(12, 18), 1, 1);
        } else {
            lines.moveTo(7, 15); lines.lineTo(11, 18); lines.lineTo(16, 11);
        }
    } else if (name == QLatin1String("Revised")) {
        fill.moveTo(2, 22); fill.lineTo(4, 15); fill.lineTo(17, 2);
        fill.lineTo(22, 7); fill.lineTo(9, 20); fill.closeSubpath();
        line(16, 2, 22, 8);
    } else if (name == QLatin1String("Reviewed")) {
        lines.addEllipse(QRectF(2, 2, 15, 15)); line(15, 15, 23, 23);
    } else if (name == QLatin1String("Received")) {
        lines.addRect(QRectF(1, 5, 22, 15));
        lines.moveTo(2, 6); lines.lineTo(12, 14); lines.lineTo(22, 6);
    } else if (name == QLatin1String("Final")) {
        fill.addEllipse(QRectF(8, 1, 8, 10));
        fill.addRect(QRectF(10, 8, 4, 8));
        fill.addRoundedRect(QRectF(4, 15, 16, 6), 1, 1);
        fill.addRect(QRectF(3, 22, 18, 2));
    } else if (name == QLatin1String("Expired")) {
        lines.addEllipse(QRectF(1, 1, 22, 22));
        line(12, 4, 12, 6); line(12, 18, 12, 20);
        line(4, 12, 6, 12); line(18, 12, 20, 12);
        lines.moveTo(12, 7); lines.lineTo(12, 12); lines.lineTo(7, 15);
    } else if (name == QLatin1String("Draft")) {
        lines.addRect(QRectF(5, 1, 15, 22));
        for (int y = 6; y <= 18; y += 4) {
            lines.moveTo(8, y); lines.lineTo(10, y - 1);
            lines.lineTo(12, y + 1); lines.lineTo(14, y - 1);
            lines.lineTo(17, y);
        }
    } else if (name == QLatin1String("Confidential")) {
        lines.moveTo(6, 11); lines.lineTo(6, 7);
        lines.cubicTo(6, -1, 18, -1, 18, 7); lines.lineTo(18, 11);
        lines.addRoundedRect(QRectF(4, 10, 16, 13), 1, 1);
        fill.addEllipse(QRectF(10, 14, 4, 4));
        fill.addRect(QRectF(11, 17, 2, 3));
    } else if (name == QLatin1String("Completed")) {
        lines.moveTo(17, 15); lines.lineTo(17, 2); lines.lineTo(2, 2);
        lines.lineTo(2, 23); lines.lineTo(15, 23);
        for (int y = 6; y <= 14; y += 4) { line(5, y, 14, y); }
        lines.moveTo(13, 18); lines.lineTo(17, 22); lines.lineTo(23, 14);
    }
    fill.addPath(stroke(lines, name == QLatin1String("Approved")
        || name == QLatin1String("Void") ? 2.5 : 1.3));
    fill.setFillRule(Qt::WindingFill);
    return fill;
}

inline QPainterPath fittedText(const QString &text, const QRectF &box,
                               int pixelSize, bool leftAligned, bool bold)
{
    QFont font(QStringLiteral("Helvetica"));
    font.setPixelSize(pixelSize);
    font.setBold(bold);
    QPainterPath path;
    path.addText(QPointF(0, 0), font, text);
    const QRectF bounds = path.boundingRect();
    if (bounds.isEmpty()) {
        return {};
    }
    const qreal scale = qMin(1.0, qMin(box.width() / bounds.width(),
                                      box.height() / bounds.height()));
    QTransform transform;
    transform.translate(box.left() + (leftAligned ? 0.0
        : (box.width() - bounds.width() * scale) / 2.0),
        box.top() + (box.height() - bounds.height() * scale) / 2.0);
    transform.scale(scale, scale);
    transform.translate(-bounds.left(), -bounds.top());
    return transform.map(path);
}

inline QPainterPath artwork(const QString &name, const QString &text,
                            const QString &detail)
{
    const Preset *preset = find(name);
    if (!preset) {
        return {};
    }
    QPainterPath border;
    border.addRect(QRectF(3, 3, 154, 44));
    QPainterPath path = stroke(border, 1.5);
    if (preset->dynamic) {
        path.addPath(fittedText(text, QRectF(8, 8, 144, 17), 15, true, false));
        path.addPath(fittedText(detail, QRectF(8, 30, 144, 10), 7, true, true));
    } else {
        const QPainterPath symbol = icon(name);
        const bool hasIcon = !symbol.isEmpty();
        if (hasIcon) {
            path.addPath(QTransform::fromTranslate(8, 13).map(symbol));
        }
        path.addPath(fittedText(text, hasIcon ? QRectF(37, 10, 115, 30)
            : QRectF(8, 10, 144, 30), 18, false, false));
    }
    path.setFillRule(Qt::WindingFill);
    return path;
}

inline QString svgPath(const QPainterPath &path)
{
    QString data;
    data.reserve(path.elementCount() * 24);
    const auto point = [](const QPainterPath::Element &element) {
        return QString::number(element.x, 'f', 4) + QLatin1Char(' ')
            + QString::number(element.y, 'f', 4) + QLatin1Char(' ');
    };
    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        if (element.isMoveTo()) {
            data += QLatin1Char('M') + point(element);
        } else if (element.isLineTo()) {
            data += QLatin1Char('L') + point(element);
        } else if (element.type == QPainterPath::CurveToElement
                   && index + 2 < path.elementCount()) {
            data += QLatin1Char('C') + point(element)
                + point(path.elementAt(index + 1))
                + point(path.elementAt(index + 2));
            index += 2;
        }
    }
    return data;
}

} // namespace PDFStampPresets
