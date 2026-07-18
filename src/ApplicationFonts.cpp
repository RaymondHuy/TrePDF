#include "ApplicationFonts.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QVector>

#include <utility>

namespace {

struct FontRegistry {
    struct BundledFont {
        QString resourcePath;
        QString family;
        bool bold = false;
        bool italic = false;
    };

    QStringList bundledFamilies;
    QStringList systemFamilies;
    QStringList availableFamilies;
    QVector<BundledFont> bundledFonts;
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
        const QString resourcePath = fontFiles.next();
        const int fontId = QFontDatabase::addApplicationFont(resourcePath);
        if (fontId < 0) {
            continue;
        }
        const QString lowerFileName = QFileInfo(resourcePath).fileName().toLower();
        const bool bold = lowerFileName.contains(QStringLiteral("bold"))
            || lowerFileName.contains(QStringLiteral("black"))
            || lowerFileName.contains(QStringLiteral("heavy"));
        const bool italic = lowerFileName.contains(QStringLiteral("italic"))
            || lowerFileName.contains(QStringLiteral("oblique"))
            || lowerFileName.contains(QStringLiteral("slanted"));
        for (const QString &family : QFontDatabase::applicationFontFamilies(fontId)) {
            if (!registry.bundledFamilies.contains(family, Qt::CaseInsensitive)) {
                registry.bundledFamilies.append(family);
            }
            registry.bundledFonts.append(FontRegistry::BundledFont{
                resourcePath,
                family,
                bold,
                italic});
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

QByteArray bundledFontData(const QString &family, bool bold, bool italic)
{
    registerBundledFonts();

    const FontRegistry::BundledFont *bestMatch = nullptr;
    int bestScore = -1;
    for (const FontRegistry::BundledFont &font : std::as_const(fontRegistry().bundledFonts)) {
        if (font.family.compare(family, Qt::CaseInsensitive) != 0) {
            continue;
        }

        const int score = (font.bold == bold ? 2 : 0)
            + (font.italic == italic ? 2 : 0);
        if (score > bestScore) {
            bestMatch = &font;
            bestScore = score;
        }
        if (score == 4) {
            break;
        }
    }
    if (!bestMatch || bestScore != 4) {
        return {};
    }

    QFile file(bestMatch->resourcePath);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace ApplicationFonts
