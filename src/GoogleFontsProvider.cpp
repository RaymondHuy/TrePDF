#include "GoogleFontsProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>

QUrl GoogleFontsProvider::metadataUrl(const QString &family) const
{
    QUrl url(QStringLiteral("https://api.fontsource.org/v1/fonts"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("family"), family);
    url.setQuery(query);
    return url;
}

IFontProvider::Family GoogleFontsProvider::resolve(
    const QString &requestedFamily, const QByteArray &metadata) const
{
    Family result;
    const auto document = QJsonDocument::fromJson(metadata);
    if (!document.isArray() || document.array().size() > 32) {
        result.availability = Availability::Invalid;
        return result;
    }
    for (const auto &value : document.array()) {
        if (!value.isObject()) {
            result.availability = Availability::Invalid;
            return result;
        }
        const auto font = value.toObject();
        const auto family = font.value(QStringLiteral("family")).toString();
        if (family.compare(requestedFamily, Qt::CaseInsensitive) != 0
            || font.value(QStringLiteral("type")).toString() != QLatin1String("google"))
            continue;
        result.availability = Availability::Invalid;
        if (!font.value(QStringLiteral("weights")).isArray()
            || !font.value(QStringLiteral("styles")).isArray()
            || !font.value(QStringLiteral("subsets")).isArray())
            return result;
        const auto subsets = font.value(QStringLiteral("subsets")).toArray();
        if (subsets.size() > 32)
            return result;
        QStringList requestedSubsets;
        for (const auto &entry : subsets) {
            const QString subset = entry.toString();
            if (!QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]{0,39}$"))
                     .match(subset).hasMatch())
                return result;
            requestedSubsets.append(subset);
        }
        const auto weights = font.value(QStringLiteral("weights")).toArray();
        const auto styles = font.value(QStringLiteral("styles")).toArray();
        const bool normal = styles.contains(QStringLiteral("normal"));
        const bool italic = styles.contains(QStringLiteral("italic"));
        result.name = family;
        result.styles = {weights.contains(400) && normal, weights.contains(400) && italic,
                         weights.contains(700) && normal, weights.contains(700) && italic};
        result.subsets = requestedSubsets.join(QLatin1Char(','));
        result.vietnamese = subsets.contains(QStringLiteral("vietnamese"));
        // Only the Google Fonts catalogue is accepted, whose fonts are licensed
        // for downloading and use/redistribution: https://developers.google.com/fonts/faq
        result.license = QStringLiteral("Google Fonts open-source catalogue");
        result.source = QUrl(QStringLiteral("https://fonts.google.com/"));
        result.availability = Availability::Found;
        return result;
    }
    return result;
}

QUrl GoogleFontsProvider::faceUrl(const Family &family, bool bold, bool italic) const
{
    QUrl url(QStringLiteral("https://fonts.googleapis.com/css"));
    QUrlQuery query;
    QString style = family.name + (bold ? QStringLiteral(":700") : QStringLiteral(":400"));
    if (italic)
        style += QStringLiteral("italic");
    query.addQueryItem(QStringLiteral("family"), style);
    query.addQueryItem(QStringLiteral("subset"), family.subsets);
    url.setQuery(query);
    return url;
}

QUrl GoogleFontsProvider::fontUrl(const QByteArray &stylesheet) const
{
    const QString css = QString::fromUtf8(stylesheet);
    const QRegularExpression expression(QStringLiteral("url\\(\\s*['\"]?(https://[^\\s)'\"]+)['\"]?\\s*\\)"));
    auto matches = expression.globalMatch(css);
    if (!matches.hasNext())
        return {};
    const QUrl url(matches.next().captured(1));
    // Refuse web-only language subsets instead of installing a partial face.
    if (matches.hasNext() || css.contains(QLatin1String("unicode-range"), Qt::CaseInsensitive)
        || !allowsUrl(url, Resource::Font))
        return {};
    return url;
}

bool GoogleFontsProvider::allowsUrl(const QUrl &url, Resource resource) const
{
    if (!isSecureUrl(url))
        return false;
    switch (resource) {
    case Resource::Metadata:
        return url.host() == QLatin1String("api.fontsource.org")
            && url.path() == QLatin1String("/v1/fonts");
    case Resource::Stylesheet:
        return url.host() == QLatin1String("fonts.googleapis.com")
            && url.path() == QLatin1String("/css");
    case Resource::Font:
        return url.host() == QLatin1String("fonts.gstatic.com")
            && url.path().startsWith(QLatin1String("/s/"))
            && url.path().endsWith(QLatin1String(".ttf")) && !url.hasQuery();
    }
    return false;
}
