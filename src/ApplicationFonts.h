#pragma once

#include <QByteArray>
#include <QStringList>

namespace ApplicationFonts {

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
bool isFontFaceSelectable(const QString &family, bool bold, bool italic);
QByteArray bundledFontData(const QString &family, bool bold, bool italic);
FontFaceData systemFontFace(const QString &family, bool bold, bool italic);

} // namespace ApplicationFonts
