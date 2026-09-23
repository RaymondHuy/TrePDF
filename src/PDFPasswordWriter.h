#pragma once

#include <QByteArray>
#include <QString>

class PDFPasswordWriter
{
public:
    // Writes only to a separate staging path. The caller validates and commits it.
    static bool encryptFile(const QString &sourcePath,
                            const QString &stagingPath,
                            const QByteArray &password,
                            const QByteArray &sourcePassword = {});
};
