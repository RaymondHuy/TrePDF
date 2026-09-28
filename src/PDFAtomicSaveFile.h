#pragma once

#include <QSaveFile>

// QSaveFile's Windows rename cannot replace a file with live readers, even
// when those readers share deletion. Keep its staging/write checks and use
// Windows' atomic POSIX rename for the final PDF destination.
class PDFAtomicSaveFile final : public QSaveFile
{
public:
    explicit PDFAtomicSaveFile(const QString &filePath);
    ~PDFAtomicSaveFile() override;
    bool open(QIODevice::OpenMode mode) override;
    bool commit();

private:
#ifdef Q_OS_WIN
    QString m_targetPath;
    QString m_stagingPath;
#endif
};
