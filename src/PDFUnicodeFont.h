#pragma once

#include <QByteArray>
#include <QRawFont>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>

#include <fpdf_edit.h>

namespace PDFUnicodeFont {

namespace detail {

inline bool validCharacters(const QVector<char32_t> &characters)
{
    if (characters.isEmpty() || characters.size() > 65535)
        return false;

    char32_t previous = 0;
    for (char32_t character : characters) {
        if (character <= previous || character > 0x10ffff
            || (character >= 0xd800 && character <= 0xdfff)) {
            return false;
        }
        previous = character;
    }
    return true;
}

} // namespace detail

// Each Unicode character gets its own CID, even when several characters share
// an outline. PDFium's automatic font loader otherwise emits overlapping
// ToUnicode mappings whose selected alias depends on the PDFium version.
// The caller supplies the QRawFont for this program and owns resource naming.
inline FPDF_FONT loadTrueType(FPDF_DOCUMENT document,
                             const QByteArray &fontData,
                             const QRawFont &rawFont,
                             const QVector<char32_t> &characters)
{
    constexpr qsizetype maximumFontBytes = 64 * 1024 * 1024;
    if (!document || fontData.isEmpty() || fontData.size() > maximumFontBytes
        || fontData.startsWith("OTTO") || !rawFont.isValid()
        || !detail::validCharacters(characters)) {
        return nullptr;
    }

    QByteArray glyphMap((characters.size() + 1) * 2, '\0');
    QByteArray unicodeMap = QByteArrayLiteral(
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /TrePDFUnicode def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
    const auto hex = [](uint value) {
        return QByteArray::number(value, 16).rightJustified(4, '0');
    };
    for (qsizetype start = 0; start < characters.size(); start += 100) {
        const qsizetype end = qMin(start + 100, characters.size());
        unicodeMap += QByteArray::number(end - start) + " beginbfchar\n";
        for (qsizetype index = start; index < end; ++index) {
            const QString character = QString::fromUcs4(&characters[index], 1);
            const auto glyphs = rawFont.glyphIndexesForString(character);
            if (glyphs.size() != 1 || glyphs.front() == 0 || glyphs.front() > 65535)
                return nullptr;

            const uint glyph = glyphs.front();
            glyphMap[(index + 1) * 2] = char(glyph >> 8);
            glyphMap[(index + 1) * 2 + 1] = char(glyph & 0xff);
            unicodeMap += '<' + hex(uint(index + 1)) + "> <";
            for (QChar unit : character)
                unicodeMap += hex(unit.unicode());
            unicodeMap += ">\n";
        }
        unicodeMap += "endbfchar\n";
    }
    unicodeMap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";

    return FPDFText_LoadCidType2Font(document,
        reinterpret_cast<const uint8_t *>(fontData.constData()),
        static_cast<uint32_t>(fontData.size()), unicodeMap.constData(),
        reinterpret_cast<const uint8_t *>(glyphMap.constData()),
        static_cast<uint32_t>(glyphMap.size()));
}

// Use the same validated character list passed to a successful loadTrueType(). Explicit
// charcodes also preserve supplementary Unicode, which PDFium's reverse
// ToUnicode lookup cannot represent as a single UTF-16 code unit.
inline bool setText(FPDF_PAGEOBJECT object,
                    const QVector<char32_t> &characters,
                    const QString &text)
{
    // The font load validates ordering once; spaced text can call this for
    // every glyph, so rescanning a full face here would be quadratic.
    if (!object || characters.isEmpty() || characters.size() > 65535
        || text.isEmpty() || !text.isValidUtf16()) {
        return false;
    }

    QVector<uint32_t> codes;
    const auto unicode = text.toUcs4();
    codes.reserve(unicode.size());
    for (char32_t character : unicode) {
        const auto found = std::lower_bound(characters.cbegin(), characters.cend(), character);
        if (found == characters.cend() || *found != character)
            return false;
        codes.append(static_cast<uint32_t>(found - characters.cbegin() + 1));
    }
    return FPDFText_SetCharcodes(object, codes.constData(), size_t(codes.size()));
}

} // namespace PDFUnicodeFont
