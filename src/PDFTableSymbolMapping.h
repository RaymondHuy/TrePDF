#pragma once

#include <string_view>

namespace PDFTableSymbolMapping {

// Private-use code points have meaning only in their original symbol font.
// Restrict this conversion to the two known Wingdings glyphs and exact font
// names, allowing only the standard six-uppercase-letter PDF subset prefix.
inline char32_t unicodeSymbol(char32_t codepoint, std::string_view fontName)
{
    if (codepoint != 0xf0fc && codepoint != 0xf0fd) {
        return codepoint;
    }
    if (fontName.size() > 7 && fontName[6] == '+') {
        bool subsetPrefix = true;
        for (std::size_t i = 0; i < 6; ++i) {
            if (fontName[i] < 'A' || fontName[i] > 'Z') {
                subsetPrefix = false;
                break;
            }
        }
        if (subsetPrefix) {
            fontName.remove_prefix(7);
        }
    }
    if (fontName != "Wingdings" && fontName != "Wingdings-Regular") {
        return codepoint;
    }
    return codepoint == 0xf0fc ? U'\u2713' : U'\u2612';
}

}
