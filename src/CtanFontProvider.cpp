#include "CtanFontProvider.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

struct CatalogFamily {
    IFontProvider::Family metadata;
    std::array<QString, 4> paths;
};

struct Catalog {
    QHash<QString, CatalogFamily> families;
    QSet<QString> paths;
    bool valid = false;
};

constexpr std::array<const char *, 4> kStyles{{
    "regular", "italic", "bold", "bolditalic"
}};

constexpr qint64 kMaximumCatalogBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumCatalogFamilies = 4096;

// Explicit HTTPS roots from https://ctan.org/mirrors (2026-09-24).
// The primary director can select a mirror that blocks unattended downloads;
// these are bounded alternatives within CTAN, not font discovery on the web.
constexpr std::array<const char *, 2> kFallbackRoots{{
    "https://ftp.fau.de/ctan", "https://ftp.jaist.ac.jp/pub/CTAN"
}};

bool validCatalogText(const QString &text, qsizetype maximum)
{
    return !text.isEmpty() && text.size() <= maximum && text == text.trimmed()
        && std::none_of(text.cbegin(), text.cend(), [](QChar character) {
            return character.category() == QChar::Other_Control;
        });
}

bool validCatalogPath(const QString &path)
{
    static const QRegularExpression expression(QStringLiteral(
        "^/(?:[A-Za-z0-9_.+-]+/)*[A-Za-z0-9_.+-]+\\.(?:otf|ttf)$"),
        QRegularExpression::CaseInsensitiveOption);
    if (path.size() > 512 || !expression.match(path).hasMatch())
        return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    return !segments.contains(QStringLiteral("."))
        && !segments.contains(QStringLiteral(".."));
}

Catalog loadCatalog()
{
    QFile file(QStringLiteral(":/trepdf/font-catalog/ctan-catalog.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumCatalogBytes)
        return {};
    const QByteArray bytes = file.read(kMaximumCatalogBytes + 1);
    if (bytes.size() > kMaximumCatalogBytes || file.error() != QFile::NoError)
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return {};
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toDouble(-1) != 1
        || !root.value(QStringLiteral("families")).isArray())
        return {};
    const QJsonArray families = root.value(QStringLiteral("families")).toArray();
    if (families.isEmpty() || families.size() > kMaximumCatalogFamilies)
        return {};

    Catalog result;
    result.families.reserve(families.size());
    result.paths.reserve(families.size() * kStyles.size());
    static const QRegularExpression packagePath(
        QStringLiteral("^/pkg/[A-Za-z0-9][A-Za-z0-9_.+-]{0,127}$"));
    static const QRegularExpression subsetName(QStringLiteral("^[a-z][a-z0-9-]{0,39}$"));
    for (const QJsonValue &value : families) {
        if (!value.isObject())
            return {};
        const QJsonObject entry = value.toObject();
        CatalogFamily family;
        family.metadata.name = entry.value(QStringLiteral("name")).toString();
        family.metadata.license = entry.value(QStringLiteral("license")).toString();
        const QString source = entry.value(QStringLiteral("source")).toString();
        family.metadata.source = QUrl(source);
        if (!validCatalogText(family.metadata.name, 128)
            || !validCatalogText(family.metadata.license, 1024)
            || source.size() > 512
            || !IFontProvider::isSecureUrl(family.metadata.source)
            || family.metadata.source.host() != QLatin1String("ctan.org")
            || family.metadata.source.hasQuery()
            || !packagePath.match(family.metadata.source.path(QUrl::FullyEncoded)).hasMatch())
            return {};
        const QString key = family.metadata.name.toCaseFolded();
        if (result.families.contains(key)
            || !entry.value(QStringLiteral("subsets")).isArray()
            || !entry.value(QStringLiteral("faces")).isObject())
            return {};
        const QJsonArray subsets = entry.value(QStringLiteral("subsets")).toArray();
        if (subsets.size() > 32)
            return {};
        QStringList subsetNames;
        for (const QJsonValue &subset : subsets) {
            const QString name = subset.toString();
            if (!subsetName.match(name).hasMatch() || subsetNames.contains(name))
                return {};
            subsetNames.append(name);
        }
        family.metadata.subsets = subsetNames.join(QLatin1Char(','));
        family.metadata.vietnamese = subsetNames.contains(QStringLiteral("vietnamese"));

        const QJsonObject faces = entry.value(QStringLiteral("faces")).toObject();
        if (faces.isEmpty() || faces.size() > qsizetype(kStyles.size()))
            return {};
        qsizetype matchedStyles = 0;
        for (size_t style = 0; style < kStyles.size(); ++style) {
            const auto found = faces.constFind(QLatin1String(kStyles[style]));
            if (found == faces.constEnd())
                continue;
            const QString path = found.value().toString();
            if (!validCatalogPath(path) || result.paths.contains(path))
                return {};
            family.paths[style] = path;
            family.metadata.styles[style] = true;
            result.paths.insert(path);
            ++matchedStyles;
        }
        if (matchedStyles != faces.size())
            return {};
        family.metadata.availability = IFontProvider::Availability::Found;
        result.families.insert(key, std::move(family));
    }
    result.valid = true;
    return result;
}

const Catalog &catalog()
{
    static const Catalog result = loadCatalog();
    return result;
}

const CatalogFamily *findFamily(const QString &name)
{
    const Catalog &data = catalog();
    const auto found = data.families.constFind(name.trimmed().toCaseFolded());
    return found == data.families.cend() ? nullptr : &found.value();
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
    if (path.size() > 2048 || path.contains(QLatin1Char('%')) || path.contains(QLatin1Char('\\'))
        || path.contains(QStringLiteral("//")))
        return {};
    const QStringList segments = path.split(QLatin1Char('/'));
    if (segments.contains(QStringLiteral("."))
        || segments.contains(QStringLiteral("..")))
        return {};
    const Catalog &data = catalog();
    // Mirrors may prepend an archive directory. Probe only complete path
    // segments so lookup cost does not grow with the number of font families.
    for (qsizetype slash = 0; slash >= 0; slash = path.indexOf(QLatin1Char('/'), slash + 1)) {
        const QString suffix = path.sliced(slash);
        if (data.paths.contains(suffix))
            return suffix;
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
    if (!catalog().valid) {
        result.availability = Availability::Invalid;
        return result;
    }
    const CatalogFamily *family = findFamily(name);
    if (!family)
        return result;
    return family->metadata;
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
    if (entry->paths[style].isEmpty())
        return {};
    return QUrl(QStringLiteral("https://mirror.ctan.org") + entry->paths[style]);
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
