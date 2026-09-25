#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace PDFOpenTypeFontWriter {
// Correct only the exact OpenType/CFF programs loaded by this save. PDFium's
// Type1 loader creates the CID mapping but labels its program as /FontFile.
// Source and destination must be distinct private staging files.
bool finalize(const QString &sourcePath, const QString &targetPath,
              const QList<QByteArray> &fonts, const QByteArray &password);
}
