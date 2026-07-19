#pragma once

#include <QByteArray>
#include <QStringList>

namespace ApplicationFonts {

enum class FontLanguage {
    Vietnamese,
    Chinese,
    Japanese,
    Korean
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
QStringList fontFamiliesSupportingText(const QString &text,
                                       bool bold = false,
                                       bool italic = false);
bool isFontFaceSelectable(const QString &family, bool bold, bool italic);
bool fontFaceSupportsText(const QString &family,
                          bool bold,
                          bool italic,
                          const QString &text);
int calibratedFontStretch(const QString &family,
                          int targetStretch,
                          bool bold,
                          bool italic,
                          int pixelSize);
QByteArray bundledFontData(const QString &family, bool bold, bool italic);
FontFaceData systemFontFace(const QString &family, bool bold, bool italic);

} // namespace ApplicationFonts
