#include "ApplicationFonts.h"

#include <QDir>
#include <QDirIterator>
#include <QFontDatabase>

#include <utility>

namespace {

struct FontRegistry {
    QStringList bundledFamilies;
    QStringList systemFamilies;
    QStringList availableFamilies;
    bool initialized = false;
};

FontRegistry &fontRegistry()
{
    static FontRegistry registry;
    return registry;
}

} // namespace

namespace ApplicationFonts {

void registerBundledFonts()
{
    FontRegistry &registry = fontRegistry();
    if (registry.initialized) {
        return;
    }
    registry.initialized = true;
    registry.systemFamilies = QFontDatabase::families();

    QDirIterator fontFiles(
        QStringLiteral(":/trepdf/fonts"),
        QStringList{
            QStringLiteral("*.ttf"),
            QStringLiteral("*.TTF"),
            QStringLiteral("*.otf"),
            QStringLiteral("*.OTF"),
            QStringLiteral("*.ttc"),
            QStringLiteral("*.TTC")},
        QDir::Files,
        QDirIterator::Subdirectories);
    while (fontFiles.hasNext()) {
        const int fontId = QFontDatabase::addApplicationFont(fontFiles.next());
        if (fontId < 0) {
            continue;
        }
        for (const QString &family : QFontDatabase::applicationFontFamilies(fontId)) {
            if (!registry.bundledFamilies.contains(family, Qt::CaseInsensitive)) {
                registry.bundledFamilies.append(family);
            }
        }
    }
    registry.bundledFamilies.sort(Qt::CaseInsensitive);

    registry.availableFamilies = registry.bundledFamilies;
    for (const QString &family : std::as_const(registry.systemFamilies)) {
        if (!registry.availableFamilies.contains(family, Qt::CaseInsensitive)) {
            registry.availableFamilies.append(family);
        }
    }
}

const QStringList &bundledFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().bundledFamilies;
}

const QStringList &systemFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().systemFamilies;
}

const QStringList &availableFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().availableFamilies;
}

} // namespace ApplicationFonts
