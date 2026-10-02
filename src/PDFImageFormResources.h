#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

namespace PDFImageFormResources {

// Page map is output-page -> source-page. Writes only to a private staging
// path, and only when resources were dropped by PDFium's content generator.
bool restoreMissing(const QString &sourcePath, const QString &stagingPath,
                    const QString &outputPath, const QMap<int, int> &pageMap,
                    const QByteArray &sourcePassword, const QByteArray &stagingPassword,
                    bool &changed);

} // namespace PDFImageFormResources
