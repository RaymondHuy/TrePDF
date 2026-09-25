#pragma once

#include <QByteArray>
#include <QCryptographicHash>

// Independent licence evidence for exact, unmodified CTAN files whose legacy
// OS/2 version 3 metadata sets both Preview & Print and Editable (0x000c).
// Do not relax the generic fsType rule or identify an exception by family name.
// Source, versions and checksums: docs/ctan-font-manifest.md (2026-09-24).
namespace ReviewedCtanFonts {

inline bool permitsEditableEmbedding(const QByteArray &data,
                                    quint16 os2Version, quint16 fsType)
{
    if (os2Version != 3 || fsType != 0x000cU || !data.startsWith("OTTO"))
        return false;

    struct Face { qsizetype size; const char *sha256; };
    // GUST Font License / LPPL-1.3c-or-later. Pagella 2.501 already has valid
    // installable-embedding metadata and deliberately needs no exception here.
    static constexpr Face faces[] = {
        // TeX Gyre Cursor 2.004: bold, bold italic, italic, regular.
        {87288, "95e9755bfa05759e8d1f8fba21a919ba46091e1275aeb5bf831dffd9c93b041e"},
        {90360, "6aa5c41f489b898aa890c543e7dcd5e732c63894babc65569e01ee7e46533d82"},
        {87536, "545f24a2a9e5dd5ace865fe5e80134f55a37794b36e26abae5424aebff600056"},
        {84652, "0667deb48aa0e88be8f499c4d308e8b9116f290e7f969b0f5a34ee15c9644272"},
        // TeX Gyre Heros 2.004.
        {135204, "b170162835f4efc288886dd4231406dc47e19b614cf4416836635599d44a7d60"},
        {135264, "166fc6d068d9c9974281555cb3d730365537a9b676ab269bb5163f5a75496505"},
        {139208, "6473df7fa107b3fb4be38973710afe22b0640c2ac076d5337cf126bed9aa108c"},
        {133600, "6ae1a09d5a940367b7aaaa91ee8bd8a2c333bfe193e7096e23f931357d62081f"},
        // TeX Gyre Schola 2.005.
        {141752, "988c7b2a0ff0eae77d1df3338751b3c47a5e759117734a375b8e7f9de80e698b"},
        {144864, "95f085da44c04817771f2a3a754b6eeb442b9a79e79f51dc3b0612e3cc0d75cc"},
        {143152, "2f45b8c394037951aaec73d947f0b0c3af715950f8f207c9f4290ba37a53a4d2"},
        {140596, "935b82e25f56b1d1276ca82793205e8ce254fbb37ebd38bedf7388cef21fbf44"},
        // TeX Gyre Termes 2.004.
        {139480, "2fb3e952065fa153c7e4e64e04b98b9d79225739b6025aa3f0f0782d299ff61e"},
        {144180, "1bf6af99cb0e26c12951317032d79b96ae009551e59ccf02a5b24f325ecfec87"},
        {141324, "6dd103a1672e50568cd2f8a706ccd48443d44d7d073a59d2286f4e6f746575d6"},
        {140932, "cc3fe7c707b81428d23d54df3eadd9228a2bf6a4d43125d94df56f5f63134659"}
    };
    for (const Face &face : faces) {
        if (data.size() == face.size
            && QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() == face.sha256)
            return true;
    }
    return false;
}

} // namespace ReviewedCtanFonts
