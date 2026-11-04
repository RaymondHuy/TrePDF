#include "FontSourceProvider.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>

namespace {
constexpr qsizetype maximumCatalogBytes = 2 * 1024 * 1024;
constexpr qsizetype maximumFamilies = 2048;
constexpr qint64 maximumFontBytes = 16 * 1024 * 1024;
constexpr auto repository = "https://github.com/tre-base/FontSource";
constexpr auto rawRoot = "https://raw.githubusercontent.com/tre-base/FontSource/";
constexpr std::array<const char *, 4> styles{"regular", "italic", "bold", "bolditalic"};

bool validText(const QString &text, qsizetype maximum)
{
    return !text.isEmpty() && text.size() <= maximum && text == text.trimmed()
        && std::none_of(text.cbegin(), text.cend(), [](QChar character) {
            return character.category() == QChar::Other_Control;
        });
}

size_t styleIndex(bool bold, bool italic)
{
    return (bold ? 2U : 0U) + (italic ? 1U : 0U);
}

QByteArray bundledCatalog()
{
    QFile file(QStringLiteral(":/trepdf/font-catalog/fontsource-catalog.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumCatalogBytes)
        return {};
    const auto bytes = file.read(maximumCatalogBytes + 1);
    return file.error() == QFile::NoError ? bytes : QByteArray{};
}
} // namespace

struct FontSourceProvider::Catalog {
    struct Face {
        QUrl url;
        qint64 bytes = 0;
        QByteArray sha256;
    };
    struct Entry {
        Family metadata;
        std::array<Face, 4> faces;
    };
    QHash<QString, Entry> families;
    QSet<QUrl> urls;

    const Entry *find(const QString &name) const
    {
        const auto found = families.constFind(name.trimmed().toCaseFolded());
        return found == families.cend() ? nullptr : &found.value();
    }
};

std::shared_ptr<const FontSourceProvider::Catalog> FontSourceProvider::parseCatalog(
    const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > maximumCatalogBytes)
        return {};
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return {};
    const auto root = document.object();
    static const QRegularExpression commitPattern(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    static const QRegularExpression keyPattern(QStringLiteral("\\A[a-z0-9]+(?:-[a-z0-9]+)*\\z"));
    static const QRegularExpression versionPattern(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.+-]{0,63}\\z"));
    static const QRegularExpression filePattern(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.+-]*\\.(?:ttf|otf)\\z"));
    static const QRegularExpression hashPattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    const auto revision = root.value(QStringLiteral("revision")).toString();
    if (root.value(QStringLiteral("schemaVersion")).toDouble(-1) != 1
        || root.value(QStringLiteral("repository")).toString() != QLatin1String(repository)
        || !commitPattern.match(revision).hasMatch()
        || !root.value(QStringLiteral("families")).isArray())
        return {};
    const auto families = root.value(QStringLiteral("families")).toArray();
    if (families.isEmpty() || families.size() > maximumFamilies)
        return {};
    auto result = std::make_shared<Catalog>();
    result->families.reserve(families.size());
    result->urls.reserve(families.size() * qsizetype(styles.size()));
    QSet<QString> keys;
    for (const auto &value : families) {
        if (!value.isObject())
            return {};
        const auto entry = value.toObject();
        Catalog::Entry family;
        family.metadata.name = entry.value(QStringLiteral("name")).toString();
        family.metadata.license = entry.value(QStringLiteral("license")).toString();
        const auto key = entry.value(QStringLiteral("key")).toString();
        const auto version = entry.value(QStringLiteral("version")).toString();
        const auto directory = QStringLiteral("fonts/") + key + QLatin1Char('/') + version + QLatin1Char('/');
        const auto source = QLatin1String(repository) + QStringLiteral("/tree/") + revision
            + QLatin1Char('/') + directory.chopped(1);
        const auto foldedName = family.metadata.name.toCaseFolded();
        if (!validText(family.metadata.name, 128) || !validText(family.metadata.license, 1024)
            || key.size() > 128 || !keyPattern.match(key).hasMatch() || keys.contains(key)
            || !versionPattern.match(version).hasMatch()
            || result->families.contains(foldedName)
            || entry.value(QStringLiteral("source")).toString() != source
            || !entry.value(QStringLiteral("vietnamese")).isBool()
            || !entry.value(QStringLiteral("faces")).isObject())
            return {};
        family.metadata.source = QUrl(source);
        family.metadata.vietnamese = entry.value(QStringLiteral("vietnamese")).toBool();
        family.metadata.subsets = family.metadata.vietnamese
            ? QStringLiteral("latin,vietnamese") : QStringLiteral("latin");
        const auto faces = entry.value(QStringLiteral("faces")).toObject();
        if (faces.isEmpty() || faces.size() > qsizetype(styles.size()))
            return {};
        qsizetype matched = 0;
        for (size_t index = 0; index < styles.size(); ++index) {
            const auto found = faces.constFind(QLatin1String(styles[index]));
            if (found == faces.constEnd())
                continue;
            if (!found.value().isObject())
                return {};
            const auto face = found.value().toObject();
            const auto path = face.value(QStringLiteral("path")).toString();
            const auto hash = face.value(QStringLiteral("sha256")).toString();
            const auto sizeValue = face.value(QStringLiteral("bytes"));
            const auto size = sizeValue.toInteger(-1);
            if (path.size() > 512 || !path.startsWith(directory)
                || !filePattern.match(path.sliced(directory.size())).hasMatch()
                || !hashPattern.match(hash).hasMatch()
                || !sizeValue.isDouble() || size < 12 || size > maximumFontBytes
                || sizeValue.toDouble() != double(size))
                return {};
            const QUrl url(QLatin1String(rawRoot) + revision + QLatin1Char('/') + path);
            if (result->urls.contains(url))
                return {};
            family.faces[index] = {url, size, QByteArray::fromHex(hash.toLatin1())};
            family.metadata.styles[index] = true;
            result->urls.insert(url);
            ++matched;
        }
        if (matched != faces.size())
            return {};
        family.metadata.availability = Availability::Found;
        keys.insert(key);
        result->families.insert(foldedName, std::move(family));
    }
    return result;
}

FontSourceProvider::FontSourceProvider()
{
    static const auto catalog = parseCatalog(bundledCatalog());
    m_catalog = catalog;
}

FontSourceProvider::FontSourceProvider(const QByteArray &catalogBytes)
    : m_catalog(parseCatalog(catalogBytes))
{
}

QUrl FontSourceProvider::metadataUrl(const QString &family) const
{
    Q_UNUSED(family);
    return {};
}

IFontProvider::Family FontSourceProvider::resolve(
    const QString &family, const QByteArray &metadata) const
{
    Q_UNUSED(metadata);
    Family result;
    if (!m_catalog) {
        result.availability = Availability::Invalid;
        return result;
    }
    const auto *entry = m_catalog->find(family);
    return entry ? entry->metadata : result;
}

IFontProvider::Resource FontSourceProvider::faceResource() const
{
    return Resource::Font;
}

QUrl FontSourceProvider::faceUrl(const Family &family, bool bold, bool italic) const
{
    const auto *entry = m_catalog ? m_catalog->find(family.name) : nullptr;
    return entry && family.availability == Availability::Found
        ? entry->faces[styleIndex(bold, italic)].url : QUrl{};
}

bool FontSourceProvider::acceptsFaceData(
    const Family &family, bool bold, bool italic, const QByteArray &data) const
{
    const auto *entry = m_catalog ? m_catalog->find(family.name) : nullptr;
    if (!entry || family.availability != Availability::Found)
        return false;
    const auto &face = entry->faces[styleIndex(bold, italic)];
    return !face.url.isEmpty() && data.size() == face.bytes
        && QCryptographicHash::hash(data, QCryptographicHash::Sha256) == face.sha256;
}

bool FontSourceProvider::allowsUrl(const QUrl &url, Resource resource) const
{
    // Compare the encoded path too: QUrl equality can equate escaped unreserved
    // characters. No escapes, alternate host, branch or URL parameters are needed.
    return m_catalog && resource == Resource::Font && isSecureUrl(url)
        && !url.hasQuery() && !url.path(QUrl::FullyEncoded).contains(QLatin1Char('%'))
        && m_catalog->urls.contains(url);
}

bool FontSourceProvider::allowsRedirect(const QUrl &from, const QUrl &to, Resource resource) const
{
    Q_UNUSED(from);
    Q_UNUSED(to);
    Q_UNUSED(resource);
    // raw.githubusercontent.com serves the pinned blob directly. Fail closed if
    // this changes rather than trusting a new host/path or another catalogue face.
    return false;
}
