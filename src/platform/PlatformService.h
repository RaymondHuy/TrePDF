#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

class PDFFileOpenHandler;
class QJSEngine;
class QQmlEngine;

// Platform adapter; the selected implementation owns native access and forwards
// durable local PDF paths to the shared file-open queue.
class PlatformService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool nativeFilesAvailable READ nativeFilesAvailable CONSTANT)
    Q_PROPERTY(bool nativeFilesBusy READ nativeFilesBusy NOTIFY nativeFilesBusyChanged)

public:
    explicit PlatformService(PDFFileOpenHandler *fileOpenHandler, QObject *parent);
    static PlatformService *create(QQmlEngine *engine, QJSEngine *scriptEngine);

    void enqueueArguments(const QStringList &arguments);
    bool nativeFilesAvailable() const;
    bool nativeFilesBusy() const { return m_nativeFilesBusy; }
    Q_INVOKABLE bool openNativePdfFiles();
    Q_INVOKABLE bool openNativeImageFile();
    Q_INVOKABLE QList<QUrl> nativePdfFiles(const QList<QUrl> &urls) const;
    Q_INVOKABLE bool importNativePdfFiles(const QList<QUrl> &urls);
    Q_INVOKABLE QUrl createLocalExportUrl(const QString &suggestedFileName);
    Q_INVOKABLE bool exportNativeFile(const QUrl &localUrl);

signals:
    void nativeFilesBusyChanged();
    void documentsImportFinished();
    void imageFileImported(const QUrl &url);
    void imageImportFinished();
    void exportFinished(const QUrl &localUrl, bool cancelled);
    void errorOccurred(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void enqueueFiles(const QList<QUrl> &urls);
    void setNativeFilesBusy(bool busy);
    bool openNativeFiles(bool images);
    void importPendingNativeFiles();

    QPointer<PDFFileOpenHandler> m_fileOpenHandler;
    bool m_nativeFilesBusy = false;
    QList<QUrl> m_pendingNativeFiles;
    QSet<QString> m_reservedExportPaths;
};
