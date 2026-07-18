#pragma once

#include <QByteArray>
#include <QStringList>

namespace ApplicationFonts {

void registerBundledFonts();
const QStringList &bundledFontFamilies();
const QStringList &systemFontFamilies();
const QStringList &availableFontFamilies();
QByteArray bundledFontData(const QString &family, bool bold, bool italic);

} // namespace ApplicationFonts
