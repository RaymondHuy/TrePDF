#pragma once
#include "IFontProvider.h"

class GoogleFontsProvider final : public IFontProvider
{
public:
    QUrl metadataUrl(const QString &family) const override;
    Family resolve(const QString &family, const QByteArray &metadata) const override;
    Resource faceResource() const override { return Resource::Stylesheet; }
    QUrl faceUrl(const Family &family, bool bold, bool italic) const override;
    QUrl fontUrl(const QByteArray &stylesheet) const override;
    bool allowsUrl(const QUrl &url, Resource resource) const override;
};
