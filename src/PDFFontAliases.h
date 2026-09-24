#pragma once

#include <QByteArray>
#include <QString>

// PDF subsets frequently share their family name with a complete local font.
// Private display aliases keep those subsets from replacing the local face in
// Qt's process-wide font database. Display aliases are never used for Save.
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
bool retain(const AliasData &alias);
void release(const QString &family);
QString semanticFamily(const QString &family);
QByteArray sourceFingerprint(const QString &family);

} // namespace PDFFontAliases
