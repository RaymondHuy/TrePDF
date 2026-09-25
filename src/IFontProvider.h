#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>
#include <array>

// Providers describe a reviewed source; the service owns transport, cancellation,
// bounded staging and atomic installation. Never pass PDF text to a provider.
class IFontProvider
{
public:
    enum class Resource { Metadata, Stylesheet, Font };
    enum class Availability { Found, NotFound, Invalid };
    struct Family {
        Availability availability = Availability::NotFound;
        QString name;
        QString subsets;
        QString license;
        QUrl source;
        std::array<bool, 4> styles{}; // regular, italic, bold, bold italic
        bool vietnamese = false;
    };
    virtual ~IFontProvider() = default;
    // Empty URL means the provider has a reviewed, local family catalogue.
    virtual QUrl metadataUrl(const QString &family) const = 0;
    virtual Family resolve(const QString &family, const QByteArray &metadata) const = 0;
    virtual Resource faceResource() const = 0;
    virtual QUrl faceUrl(const Family &family, bool bold, bool italic) const = 0;
    // Optional reviewed mirrors of the same font. Used only after a transport
    // failure, never to bypass font validation or an unsafe redirect.
    virtual QList<QUrl> fallbackFaceUrls(const Family &family, bool bold, bool italic) const
    { Q_UNUSED(family); Q_UNUSED(bold); Q_UNUSED(italic); return {}; }
    virtual QUrl fontUrl(const QByteArray &stylesheet) const { Q_UNUSED(stylesheet); return {}; }
    virtual bool allowsUrl(const QUrl &url, Resource resource) const = 0;
    virtual bool allowsRedirect(const QUrl &from, const QUrl &to, Resource resource) const
    { Q_UNUSED(from); return allowsUrl(to, resource); }

    static bool isSecureUrl(const QUrl &url)
    {
        return url.isValid() && url.scheme() == QLatin1String("https")
            && !url.host().isEmpty() && url.userInfo().isEmpty()
            && url.port(-1) == -1 && !url.hasFragment();
    }
};
