#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>

namespace ApplicationFonts {

enum class FontLanguage {
    Vietnamese,
    English,
    Chinese,
    Japanese,
    Korean,
    Symbol
};

enum class FontSource {
    Any,
    System,
    Downloaded,
    Bundled
};

struct FontFaceData {
    enum class Error {
        None,
        Unavailable,
        UnsupportedFormat,
        EmbeddingRestricted,
        TooLarge
    };

    QByteArray data;
    QString family;
    // Immutable private Qt name for layout on workers that may have cached a
    // fallback for the public family before this face was downloaded.
    QString layoutFamily;
    Error error = Error::Unavailable;

    bool isValid() const
    {
        return error == Error::None && !data.isEmpty();
    }
};

void registerBundledFonts();
const QStringList &bundledFontFamilies();
const QStringList &systemFontFamilies();
const QStringList &availableFontFamilies(bool bold = false,
                                         bool italic = false);
const QStringList &fontFamiliesForLanguage(FontLanguage language,
                                           bool bold = false,
                                           bool italic = false);
bool isFontFaceSelectable(const QString &family, bool bold, bool italic);
// Select a source when resolving one exact face; Any checks system, cache, then
// bundled fonts without treating another source's coverage as that face's own.
bool fontFaceSupportsText(const QString &family,
                          bool bold,
                          bool italic,
                          const QString &text,
                          FontSource source = FontSource::Any);
// Resolve a private display alias for live Qt layout without changing its
// semantic source family. Downloaded faces can replace stale public-family
// fallbacks. Private aliases have one face; another style needs an exact local
// face of the same semantic family.
// Returns the original family when no safe local match exists.
QString displayFamilyForStyle(const QString &family, bool bold, bool italic,
                              const QString &text = {});
int calibratedFontStretch(const QString &family,
                          int targetStretch,
                          bool bold,
                          bool italic,
                          int pixelSize);
QByteArray bundledFontData(const QString &family, bool bold, bool italic);
FontFaceData systemFontFace(const QString &family, bool bold, bool italic);
QString downloadedFontDirectory();
const QStringList &downloadedFontFamilies();
FontFaceData downloadedFontFace(const QString &family, bool bold, bool italic);
// Validates a complete face without registering it or writing to private storage.
bool validateDownloadedFontData(const QByteArray &data, const QString &family,
                                bool bold, bool italic, uint requiredCodePoint,
                                QString &error);
struct DownloadedFontData {
    QByteArray data;
    QString family;
    bool bold = false;
    bool italic = false;
    uint requiredCodePoint = 0;
};
// Installs at most four validated faces together, rolling back a failed batch.
bool installDownloadedFonts(const QList<DownloadedFontData> &fonts, QString &error);
// Called on the GUI thread only, after the caller has obtained download consent.
// Fonts are private to TrePDF and are never installed in the operating system.
bool installDownloadedFont(const QByteArray &data, const QString &family,
                           bool bold, bool italic, uint requiredCodePoint,
                           QString &error);

} // namespace ApplicationFonts
