#pragma once

#include "PDFUnicodeFont.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QRawFont>
#include <QScopeGuard>
#include <QtMath>
#include <fpdf_edit.h>
#include <fpdfview.h>

// Some PDF symbol subsets expose Unicode bullets only in the PDF's ToUnicode
// map, not the font's cmap. Recover common bullets by matching the PDF's actual
// outline against that same embedded program, never a family-name substitution.
// Call only while holding the PDFium process lock. The original PDF is untouched.
namespace PDFSymbolFont {
namespace detail {
inline quint16 u16(const QByteArray &data, qsizetype at)
{
    return (quint16(quint8(data[at])) << 8) | quint8(data[at + 1]);
}
inline quint32 u32(const QByteArray &data, qsizetype at)
{
    return (quint32(u16(data, at)) << 16) | u16(data, at + 2);
}
inline void append32(QByteArray &data, quint32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        data.append(char(value >> shift));
}
inline void write32(QByteArray &data, qsizetype at, quint32 value)
{
    for (int byte = 0; byte < 4; ++byte)
        data[at + byte] = char(value >> (24 - 8 * byte));
}
inline quint32 checksum(const QByteArray &data)
{
    quint32 sum = 0;
    for (qsizetype at = 0; at < data.size(); at += 4) {
        quint32 word = 0;
        for (int i = 0; i < 4; ++i)
            word = (word << 8) | (at + i < data.size() ? quint8(data[at + i]) : 0);
        sum += word;
    }
    return sum;
}
inline bool hasOnlyLegacySymbolCoverage(const QByteArray &cmap)
{
    if (cmap.size() < 4 || u16(cmap, 0) != 0)
        return false;
    const int count = u16(cmap, 2);
    const qsizetype directoryEnd = 4 + count * 8;
    if (!count || count > 32 || directoryEnd > cmap.size())
        return false;
    bool symbolic = false;
    for (int index = 0; index < count; ++index) {
        const qsizetype entry = 4 + index * 8;
        const quint16 platform = u16(cmap, entry), encoding = u16(cmap, entry + 2);
        const quint32 offset = u32(cmap, entry + 4);
        if (offset < quint32(directoryEnd) || quint64(offset) + 6 > quint64(cmap.size()))
            return false;
        const quint16 format = u16(cmap, offset), length = u16(cmap, offset + 2);
        if (quint64(offset) + length > quint64(cmap.size()))
            return false;
        // Macintosh format 0 is byte-only. Other encodings can expose mappings
        // outside the domain we preserve, so leave the original font untouched.
        if (platform == 1 && encoding == 0 && format == 0 && length == 262)
            continue;
        if (platform != 3 || encoding != 0 || format != 4 || length < 16)
            return false;
        symbolic = true;
        const quint16 segmentBytes = u16(cmap, offset + 6);
        const int segments = segmentBytes / 2;
        if (!segments || (segmentBytes & 1) || 16 + segments * 8 > length)
            return false;
        quint16 previousEnd = 0;
        for (int segment = 0; segment < segments; ++segment) {
            const quint16 end = u16(cmap, offset + 14 + segment * 2);
            const quint16 start = u16(cmap, offset + 16 + segments * 2 + segment * 2);
            const qsizetype rangeOffsetAt = offset + 16 + segments * 6 + segment * 2;
            const quint16 rangeOffset = u16(cmap, rangeOffsetAt);
            if (start > end || (segment > 0 && start <= previousEnd))
                return false;
            const bool sentinel = segment == segments - 1 && start == 0xffff && end == 0xffff;
            if (!sentinel && !(end <= 0xff || (start >= 0xf000 && end <= 0xf0ff)))
                return false;
            if (rangeOffset && ((rangeOffset & 1)
                || rangeOffsetAt + rangeOffset + (end - start + 1) * 2 > offset + length))
                return false;
            previousEnd = end;
        }
        if (previousEnd != 0xffff)
            return false;
    }
    return symbolic;
}
inline QByteArray outlineKey(FPDF_FONT font, uint unicode)
{
    const auto path = FPDFFont_GetGlyphPath(font, unicode, 1000.0f);
    const int count = path ? FPDFGlyphPath_CountGlyphSegments(path) : 0;
    if (count <= 0 || count > 256)
        return {};
    QByteArray key;
    for (int index = 0; index < count; ++index) {
        const auto segment = FPDFGlyphPath_GetGlyphPathSegment(path, index);
        float x = 0, y = 0;
        if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !qIsFinite(x) || !qIsFinite(y) || qAbs(x) > 10000 || qAbs(y) > 10000)
            return {};
        append32(key, quint32(qRound(x * 1024)));
        append32(key, quint32(qRound(y * 1024)));
        key.append(char(FPDFPathSegment_GetType(segment)));
        key.append(char(FPDFPathSegment_GetClose(segment)));
    }
    return key;
}
} // namespace detail

inline QByteArray withUnicodeBullets(FPDF_FONT source, const QByteArray &data)
{
    using namespace detail;
    constexpr qsizetype maximumBytes = 4 * 1024 * 1024;
    if (!source || FPDFFont_GetIsEmbedded(source) != 1 || data.size() < 12
        || data.size() > maximumBytes || u32(data, 0) != 0x00010000U)
        return data;
    const int count = u16(data, 4);
    if (!count || count > 256 || 12 + count * 16 > data.size())
        return data;
    qsizetype cmapEntry = -1, headOffset = -1;
    for (int i = 0; i < count; ++i) {
        const int entry = 12 + i * 16;
        const quint32 offset = u32(data, entry + 8), size = u32(data, entry + 12);
        if (offset < quint32(12 + count * 16) || quint64(offset) + size > quint64(data.size()))
            return data;
        if (u32(data, entry) == 0x636d6170U) {
            if (cmapEntry >= 0) return data;
            cmapEntry = entry;
        }
        if (u32(data, entry) == 0x68656164U) {
            if (headOffset >= 0 || size < 54) return data;
            headOffset = offset;
        }
    }
    if (cmapEntry < 0 || headOffset < 0)
        return data;
    const QByteArray oldCmap = data.mid(u32(data, cmapEntry + 8), u32(data, cmapEntry + 12));
    // This also rejects existing Unicode cmaps, including a repaired save.
    if (!hasOnlyLegacySymbolCoverage(oldCmap))
        return data;
    const QRawFont raw(data, 16, QFont::PreferNoHinting);
    if (!raw.isValid() || raw.fontTable("glyf").isEmpty()
        || !raw.fontTable("fvar").isEmpty())
        return data;
    const auto document = FPDF_CreateNewDocument();
    if (!document) return data;
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    QVector<char32_t> privateCharacters;
    for (char32_t character = 0xf000; character <= 0xf0ff; ++character) {
        if (raw.supportsCharacter(character))
            privateCharacters.append(character);
    }
    const auto font = PDFUnicodeFont::loadTrueType(document, data, raw, privateCharacters);
    if (!font) return data;
    const auto closeFont = qScopeGuard([&] { FPDFFont_Close(font); });
    const QByteArray missingOutline = outlineKey(font, 0);
    // A missing Unicode request may return a real .notdef/fallback path, not
    // nullptr. Exclude source-side missing paths as well as the probe's .notdef.
    const QList<QByteArray> sourceMissingOutlines{
        outlineKey(source, 0), outlineKey(source, 0xfffe), outlineKey(source, 0xffff)};
    QMap<uint, uint> mapping;
    QHash<QByteArray, uint> outlines;
    for (uint code = 0; code <= 255; ++code) {
        const auto legacy = raw.glyphIndexesForString(QString(QChar(ushort(code))));
        if (legacy.size() == 1 && legacy.first() && legacy.first() <= 65535)
            mapping.insert(code, legacy.first());
        const auto glyphs = raw.glyphIndexesForString(QString(QChar(ushort(0xf000 + code))));
        if (glyphs.size() != 1 || !glyphs.first() || glyphs.first() > 65535)
            continue;
        mapping.insert(0xf000 + code, glyphs.first());
        const QByteArray key = outlineKey(font, 0xf000 + code);
        if (key.isEmpty() || key == missingOutline)
            continue;
        auto found = outlines.find(key);
        if (found == outlines.end())
            outlines.insert(key, glyphs.first());
        else if (*found != glyphs.first())
            *found = 0; // Ambiguous outlines cannot establish a glyph identity.
    }
    bool repaired = false;
    for (uint unicode : {0x2022U, 0x25a0U, 0x25aaU, 0x25cfU, 0x25cbU}) {
        const QByteArray key = outlineKey(source, unicode);
        const uint glyph = outlines.value(key);
        if (!glyph || key == missingOutline || sourceMissingOutlines.contains(key))
            continue;
        mapping.insert(unicode, glyph);
        repaired = true;
    }
    if (!repaired)
        return data;
    // A Unicode format-12 map makes the recovered glyphs available to both Qt
    // layout and the explicit CID writer. Preserve the symbol/legacy aliases.
    QByteArray cmap = QByteArray::fromHex("000000010003000a0000000c000c0000");
    append32(cmap, 16 + uint(mapping.size()) * 12);
    append32(cmap, 0);
    append32(cmap, uint(mapping.size()));
    for (auto entry = mapping.cbegin(); entry != mapping.cend(); ++entry) {
        append32(cmap, entry.key());
        append32(cmap, entry.key());
        append32(cmap, entry.value());
    }
    QByteArray result = data;
    while (result.size() % 4) result.append('\0');
    write32(result, cmapEntry + 4, checksum(cmap));
    write32(result, cmapEntry + 8, quint32(result.size()));
    write32(result, cmapEntry + 12, quint32(cmap.size()));
    result += cmap;
    write32(result, headOffset + 8, 0);
    write32(result, headOffset + 8, 0xb1b0afbaU - checksum(result));
    if (result.size() > maximumBytes)
        return data;
    const QRawFont check(result, 16, QFont::PreferNoHinting);
    if (!check.isValid() || check.familyName() != raw.familyName())
        return data;
    for (auto entry = mapping.cbegin(); entry != mapping.cend(); ++entry) {
        const auto glyphs = check.glyphIndexesForString(QString(QChar(ushort(entry.key()))));
        if (glyphs.size() != 1 || glyphs.first() != entry.value())
            return data;
    }
    return result;
}
} // namespace PDFSymbolFont
