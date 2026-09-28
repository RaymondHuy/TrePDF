#pragma once

#include "IFontProvider.h"

// The bundled JSON catalogue lists exact families and upstream font paths.
// The service owns transport limits, font validation and atomic installation.
class CtanFontProvider final : public IFontProvider
{
public:
    QUrl metadataUrl(const QString &family) const override;
    Family resolve(const QString &family, const QByteArray &metadata) const override;
    Resource faceResource() const override;
    QUrl faceUrl(const Family &family, bool bold, bool italic) const override;
    QList<QUrl> fallbackFaceUrls(const Family &family, bool bold, bool italic) const override;
    bool allowsUrl(const QUrl &url, Resource resource) const override;
    bool allowsRedirect(const QUrl &from, const QUrl &to, Resource resource) const override;
};
