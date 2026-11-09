#pragma once

#include "ApplicationFonts.h"

#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QVector>
#include <cmath>
#include <utility>

namespace PDFTableCellFormat {
inline constexpr int MaximumSpansPerCell = 256;
inline constexpr int MaximumSpansPerTable = 1024;

struct Style {
    QString family;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    qreal fontSize = 12.0;
    bool operator==(const Style &) const = default;
};
struct Span {
    int start = 0;
    int length = 0;
    Style style;
    bool operator==(const Span &) const = default;
};
struct Cell {
    Style defaultStyle;
    QVector<Span> spans;
    bool operator==(const Cell &) const = default;
};
using Cells = QVector<Cell>;

inline bool validFontSize(qreal size)
{
    return std::isfinite(size) && size >= 1.0 && size <= 512.0 && size == std::floor(size);
}

inline bool validStyle(const Style &style)
{
    if (style.family.isEmpty() || style.family.size() > 256 || !style.family.isValidUtf16()
        || style.family != style.family.trimmed() || !validFontSize(style.fontSize))
        return false;
    for (const auto character : style.family) {
        if (character.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

inline Cell plain(const QString &text, const Style &style)
{
    Cell result{style, {}};
    if (!text.isEmpty())
        result.spans.append({0, int(text.size()), style});
    return result;
}

inline bool valid(const Cell &cell, const QString &text, int maximumSpans = MaximumSpansPerCell)
{
    if (!validStyle(cell.defaultStyle) || cell.spans.size() > maximumSpans)
        return false;
    int end = 0;
    for (const auto &span : cell.spans) {
        if (span.start != end || span.length <= 0 || span.length > text.size() - end
            || !validStyle(span.style)
            || (end > 0 && text.at(end).isLowSurrogate()))
            return false;
        end += span.length;
    }
    return end == text.size();
}

inline Style styleAt(const Cell &cell, int position, bool preferPrevious = false)
{
    const int index = preferPrevious && position > 0 ? position - 1 : position;
    for (const auto &span : cell.spans) {
        if (index >= span.start && index < span.start + span.length)
            return span.style;
    }
    // A final empty paragraph inherits its preceding newline in QTextDocument.
    if (!cell.spans.isEmpty()
        && index == cell.spans.constLast().start + cell.spans.constLast().length)
        return cell.spans.constLast().style;
    return cell.defaultStyle;
}

inline QFont displayFont(const Style &style)
{
    const QString alias = ApplicationFonts::displayFamilyForStyle(style.family, style.bold, style.italic);
    QFont font(alias.isEmpty() ? style.family : alias);
    font.setPixelSize(qMax(1, qRound(style.fontSize)));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
#ifdef Q_OS_WIN
    font.setHintingPreference(QFont::PreferNoHinting);
#endif
    font.setStretch(ApplicationFonts::calibratedFontStretch(alias, 100,
        style.bold, style.italic, qMax(1, qRound(style.fontSize))));
    return font;
}

inline QJsonObject styleJson(const Style &style)
{
    return {{QStringLiteral("family"), style.family}, {QStringLiteral("bold"), style.bold},
            {QStringLiteral("italic"), style.italic}, {QStringLiteral("underline"), style.underline},
            {QStringLiteral("fontSize"), style.fontSize}};
}

inline bool readStyle(const QJsonObject &object, Style &style, bool requireFontSize = false)
{
    const auto size = object.value(QStringLiteral("fontSize"));
    if (!object.value(QStringLiteral("family")).isString()
        || !object.value(QStringLiteral("bold")).isBool()
        || !object.value(QStringLiteral("italic")).isBool()
        || !object.value(QStringLiteral("underline")).isBool()
        || ((requireFontSize || !size.isUndefined()) && !size.isDouble()))
        return false;
    style = {object.value(QStringLiteral("family")).toString(),
             object.value(QStringLiteral("bold")).toBool(),
             object.value(QStringLiteral("italic")).toBool(),
             object.value(QStringLiteral("underline")).toBool(),
             size.isUndefined() ? 12.0 : size.toDouble()};
    return validStyle(style);
}

inline QJsonObject toJson(const Cell &cell)
{
    QJsonArray spans;
    for (const auto &span : cell.spans) {
        auto object = styleJson(span.style);
        object.insert(QStringLiteral("start"), span.start);
        object.insert(QStringLiteral("length"), span.length);
        spans.append(object);
    }
    return {{QStringLiteral("defaultStyle"), styleJson(cell.defaultStyle)},
            {QStringLiteral("spans"), spans}};
}

inline bool fromJson(const QJsonObject &object, const QString &text, Cell &cell,
                     int maximumSpans = MaximumSpansPerCell, bool requireFontSize = false)
{
    Cell result;
    if (!object.value(QStringLiteral("defaultStyle")).isObject()
        || !readStyle(object.value(QStringLiteral("defaultStyle")).toObject(), result.defaultStyle, requireFontSize)
        || !object.value(QStringLiteral("spans")).isArray())
        return false;
    const auto spans = object.value(QStringLiteral("spans")).toArray();
    if (spans.size() > maximumSpans)
        return false;
    for (const auto &value : spans) {
        if (!value.isObject())
            return false;
        const auto entry = value.toObject();
        const auto start = entry.value(QStringLiteral("start"));
        const auto length = entry.value(QStringLiteral("length"));
        Span span;
        if (!start.isDouble() || !length.isDouble()
            || start.toDouble() < 0 || start.toDouble() > text.size()
            || length.toDouble() <= 0 || length.toDouble() > text.size()
            || std::floor(start.toDouble()) != start.toDouble()
            || std::floor(length.toDouble()) != length.toDouble()
            || !readStyle(entry, span.style, requireFontSize))
            return false;
        span.start = start.toInt(-1);
        span.length = length.toInt(-1);
        result.spans.append(span);
    }
    if (!valid(result, text, maximumSpans))
        return false;
    cell = std::move(result);
    return true;
}

inline QVariantMap toVariant(const Cell &cell) { return toJson(cell).toVariantMap(); }
inline bool fromVariant(const QVariantMap &map, const QString &text, Cell &cell,
                        int maximumSpans = MaximumSpansPerCell)
{
    return fromJson(QJsonObject::fromVariantMap(map), text, cell, maximumSpans);
}
} // namespace PDFTableCellFormat
