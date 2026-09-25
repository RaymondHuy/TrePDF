#include "CtanFontProvider.h"

#include <QHostAddress>
#include <QStringList>

namespace {

struct CatalogFamily {
    const char *name;
    const char *stem;
};

// Reviewed upstream catalogue, not names inferred from an arbitrary URL:
// https://ctan.org/pkg/tex-gyre
// https://mirror.ctan.org/fonts/tex-gyre/opentype/
// The original files are static OpenType/CFF fonts, not TrueType fonts.
constexpr std::array<CatalogFamily, 5> kFamilies{{
    {"TeX Gyre Termes", "texgyretermes"},
    {"TeX Gyre Heros", "texgyreheros"},
    {"TeX Gyre Pagella", "texgyrepagella"},
    {"TeX Gyre Schola", "texgyreschola"},
    {"TeX Gyre Cursor", "texgyrecursor"}
}};

constexpr std::array<const char *, 4> kStyles{{
    "regular", "italic", "bold", "bolditalic"
}};

// Explicit HTTPS roots from https://ctan.org/mirrors (2026-09-24).
// The primary director can select a mirror that blocks unattended downloads;
// these are bounded alternatives within CTAN, not font discovery on the web.
constexpr std::array<const char *, 2> kFallbackRoots{{
    "https://ftp.fau.de/ctan", "https://ftp.jaist.ac.jp/pub/CTAN"
}};

const CatalogFamily *findFamily(const QString &name)
{
    const QString trimmed = name.trimmed();
    for (const CatalogFamily &family : kFamilies) {
        if (trimmed.compare(QLatin1String(family.name), Qt::CaseInsensitive) == 0)
            return &family;
    }
    return nullptr;
}

QString catalogPath(const CatalogFamily &family, const char *style)
{
    return QStringLiteral("/fonts/tex-gyre/opentype/")
        + QLatin1String(family.stem) + QLatin1Char('-')
        + QLatin1String(style) + QStringLiteral(".otf");
}

bool isSecureFontUrl(const QUrl &url)
{
    if (!IFontProvider::isSecureUrl(url) || url.hasQuery())
        return false;
    const QString host = url.host().toLower();
    QHostAddress address;
    return !address.setAddress(host)
        && host.contains(QLatin1Char('.'))
        && host != QLatin1String("localhost")
        && !host.endsWith(QLatin1String(".localhost"))
        && !host.endsWith(QLatin1String(".local"));
}

QString expectedCatalogPath(const QUrl &url)
{
    // Reject encoded separators/traversal, including double-encoded forms. The
    // audited paths and the normal CTAN mirror prefixes need no URL escapes.
    const QString path = url.path(QUrl::FullyEncoded);
    if (path.contains(QLatin1Char('%')) || path.contains(QLatin1Char('\\'))
        || path.contains(QStringLiteral("//")))
        return {};
    const QStringList segments = path.split(QLatin1Char('/'));
    if (segments.contains(QStringLiteral("."))
        || segments.contains(QStringLiteral("..")))
        return {};
    for (const CatalogFamily &family : kFamilies) {
        for (const char *style : kStyles) {
            const QString expected = catalogPath(family, style);
            if (path.endsWith(expected))
                return expected;
        }
    }
    return {};
}

} // namespace

QUrl CtanFontProvider::metadataUrl(const QString &family) const
{
    Q_UNUSED(family);
    return {};
}

IFontProvider::Family CtanFontProvider::resolve(
    const QString &name, const QByteArray &metadata) const
{
    Q_UNUSED(metadata);
    Family result;
    const CatalogFamily *family = findFamily(name);
    if (!family)
        return result;
    result.availability = Availability::Found;
    result.name = QLatin1String(family->name);
    result.subsets = QStringLiteral("latin,vietnamese");
    // The GFL permits distribution/modification under LPPL 1.3c or later.
    // https://ctan.org/license/gfl
    // https://www.latex-project.org/lppl/lppl-1-3c/
    result.license = QStringLiteral("GUST Font License 1.0 / LPPL-1.3c-or-later");
    result.source = QUrl(QStringLiteral("https://ctan.org/pkg/tex-gyre"));
    result.styles = {{true, true, true, true}};
    result.vietnamese = true;
    return result;
}

IFontProvider::Resource CtanFontProvider::faceResource() const
{
    return Resource::Font;
}

QUrl CtanFontProvider::faceUrl(const Family &family, bool bold, bool italic) const
{
    const CatalogFamily *entry = findFamily(family.name);
    if (!entry || family.availability != Availability::Found)
        return {};
    const size_t style = (bold ? 2U : 0U) + (italic ? 1U : 0U);
    return QUrl(QStringLiteral("https://mirror.ctan.org")
                + catalogPath(*entry, kStyles.at(style)));
}

QList<QUrl> CtanFontProvider::fallbackFaceUrls(
    const Family &family, bool bold, bool italic) const
{
    const QUrl primary = faceUrl(family, bold, italic);
    if (primary.isEmpty())
        return {};
    QList<QUrl> urls;
    for (const char *root : kFallbackRoots)
        urls.append(QUrl(QLatin1String(root) + primary.path()));
    return urls;
}

bool CtanFontProvider::allowsUrl(const QUrl &url, Resource resource) const
{
    if (resource != Resource::Font || !isSecureFontUrl(url))
        return false;
    const QString expected = expectedCatalogPath(url);
    if (expected.isEmpty())
        return false;
    if (url.host() == QLatin1String("mirror.ctan.org"))
        return url.path(QUrl::FullyEncoded) == expected;
    for (const char *root : kFallbackRoots) {
        if (url == QUrl(QLatin1String(root) + expected))
            return true;
    }
    return false;
}

bool CtanFontProvider::allowsRedirect(
    const QUrl &from, const QUrl &to, Resource resource) const
{
    if (resource != Resource::Font || !isSecureFontUrl(from)
        || !isSecureFontUrl(to))
        return false;
    const QString expected = expectedCatalogPath(from);
    if (expected.isEmpty() || expectedCatalogPath(to) != expected)
        return false;
    // Only the trusted CTAN multiplexor may nominate a different mirror host.
    // The service only invokes this with its currently approved request URL.
    // Mirrors may add /CTAN, /ctan, /tex-archive or another archive prefix, but
    // the exact audited font suffix must survive every hop unchanged.
    return (from.host() == QLatin1String("mirror.ctan.org") && allowsUrl(from, resource))
        || from.host() == to.host();
}
