#pragma once

#include "IFontProvider.h"

#include <memory>

// Reviewed metadata is bundled; font bytes come from one immutable GitHub commit.
// The service retains transport, cancellation and atomic font installation.
class FontSourceProvider final : public IFontProvider
{
public:
    FontSourceProvider();
    // Explicit snapshots allow parser validation without replacing app resources.
    explicit FontSourceProvider(const QByteArray &catalogBytes);

    QUrl metadataUrl(const QString &family) const override;
    Family resolve(const QString &family, const QByteArray &metadata) const override;
    Resource faceResource() const override;
    QUrl faceUrl(const Family &family, bool bold, bool italic) const override;
    bool acceptsFaceData(const Family &family, bool bold, bool italic,
                         const QByteArray &data) const override;
    bool allowsUrl(const QUrl &url, Resource resource) const override;
    bool allowsRedirect(const QUrl &from, const QUrl &to, Resource resource) const override;

private:
    struct Catalog;
    static std::shared_ptr<const Catalog> parseCatalog(const QByteArray &bytes);
    std::shared_ptr<const Catalog> m_catalog;
};
