#pragma once

#include <QStringList>

namespace ApplicationFonts {

void registerBundledFonts();
const QStringList &bundledFontFamilies();
const QStringList &systemFontFamilies();
const QStringList &availableFontFamilies();

} // namespace ApplicationFonts
