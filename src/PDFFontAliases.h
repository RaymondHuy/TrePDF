#pragma once

#include <QByteArray>
#include <QString>

// PDF subsets frequently share their family name with a complete local font.
// Private display aliases keep those subsets from replacing the local face in
// Qt's process-wide font database. Downloaded faces also use immutable aliases
// for worker layout. Alias programs/names are never embedded in saved PDFs.
namespace PDFFontAliases {

struct AliasData {
    QByteArray data;
    QString family;
    QString semanticFamily;
    QByteArray fingerprint;
    bool source = true;
};

AliasData isolate(const QByteArray &data, const QString &expectedFamily,
                  bool substitute = false);
// Keep the public family and all outlines/metrics, but give a TrueType save
// resource its own PostScript name so PDFium cannot merge different CID maps.
QByteArray withSaveResourceName(const QByteArray &data, const QByteArray &identity);
// GUI-thread lifetime management. All documents and downloaded faces share one
// Qt registration per alias; removing a duplicate registration on macOS can
// invalidate a worker's font engine even while another owner still uses it.
// Once its last owner releases it, a later isolate uses a fresh family name.
bool retain(const AliasData &alias);
void release(const QString &family);
QString semanticFamily(const QString &family);
QByteArray sourceFingerprint(const QString &family);
// Intrinsic style from the registered font program, independent of Qt's
// synthetic weight/slant for a requested QFont.
bool matchesStyle(const QString &family, bool bold, bool italic);

} // namespace PDFFontAliases
