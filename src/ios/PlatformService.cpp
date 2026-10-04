#include "../platform/PlatformService.h"

#include "IOSDocumentAccess.h"
#include "../PDFFileOpenHandler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QJSEngine>
#include <QQmlEngine>

#include <utility>

PlatformService::PlatformService(PDFFileOpenHandler *fileOpenHandler, QObject *parent)
    : QObject(parent)
    , m_fileOpenHandler(fileOpenHandler)
{
    QCoreApplication::instance()->installEventFilter(this);
}

bool PlatformService::nativeFilesAvailable() const { return true; }

PlatformService *PlatformService::create(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(scriptEngine);
    auto *application = QCoreApplication::instance();
    if (!application || engine->thread() != application->thread())
        return nullptr;
    auto *service = application->findChild<PlatformService *>(
        QString(), Qt::FindDirectChildrenOnly);
    if (service)
        QJSEngine::setObjectOwnership(service, QJSEngine::CppOwnership);
    return service;
}

void PlatformService::enqueueArguments(const QStringList &arguments)
{
    QList<QUrl> urls;
    bool positionalOnly = false;
    for (const QString &argument : arguments) {
        if (!positionalOnly && argument == QStringLiteral("--")) {
            positionalOnly = true;
            continue;
        }
        if (argument.isEmpty() || (!positionalOnly && argument.startsWith(QLatin1Char('-'))))
            continue;
        const QUrl url(argument);
        if (QDir::isAbsolutePath(argument) || url.scheme().isEmpty())
            urls.append(QUrl::fromLocalFile(QFileInfo(argument).absoluteFilePath()));
        else
            urls.append(url);
    }
    enqueueFiles(urls);
}

void PlatformService::setNativeFilesBusy(bool busy)
{
    if (m_nativeFilesBusy == busy)
        return;
    m_nativeFilesBusy = busy;
    emit nativeFilesBusyChanged();
}

bool PlatformService::openNativePdfFiles()
{
    return openNativeFiles(false);
}

bool PlatformService::openNativeImageFile()
{
    return openNativeFiles(true);
}

QList<QUrl> PlatformService::nativePdfFiles(const QList<QUrl> &urls) const
{
    QList<QUrl> result;
    for (const QUrl &url : urls) {
        // Do not stat provider URLs before the import operation acquires access.
        if (url.isValid() && url.isLocalFile() && !url.hasQuery() && !url.hasFragment()
            && url.userInfo().isEmpty() && url.port() == -1
            && (url.host().isEmpty() || url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0)
            && QFileInfo(url.toLocalFile()).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0
            && !result.contains(url))
            result.append(url);
    }
    return result;
}

bool PlatformService::importNativePdfFiles(const QList<QUrl> &urls)
{
    const auto files = nativePdfFiles(urls);
    if (files.isEmpty())
        return false;
    enqueueFiles(files);
    return true;
}

bool PlatformService::openNativeFiles(bool images)
{
    if (m_nativeFilesBusy)
        return false;
    setNativeFilesBusy(true);
    const QPointer<PlatformService> guard(this);
    if (IOSDocumentAccess::openFiles(images, [guard, images](IOSDocumentAccess::Result result) {
            if (!guard)
                return;
            guard->setNativeFilesBusy(false);
            if (!result.error.isEmpty())
                emit guard->errorOccurred(result.error);
            if (images) {
                if (!result.files.isEmpty())
                    emit guard->imageFileImported(result.files.first());
                emit guard->imageImportFinished();
            } else {
                guard->enqueueFiles(result.files);
                emit guard->documentsImportFinished();
            }
            guard->importPendingNativeFiles();
        })) {
        return true;
    }
    setNativeFilesBusy(false);
    //% "The Files picker is not available. Close any open dialog and try again."
    emit errorOccurred(qtTrId("IOSFilesPickerUnavailable"));
    if (images)
        emit imageImportFinished();
    else
        emit documentsImportFinished();
    return false;
}

QUrl PlatformService::createLocalExportUrl(const QString &suggestedFileName)
{
    QString name = QFileInfo(suggestedFileName).fileName();
    name.replace(QLatin1Char(':'), QLatin1Char('_'));
    const QFileInfo info(name);
    const QString suffix = info.suffix().toLower();
    if ((suffix != QStringLiteral("pdf") && suffix != QStringLiteral("xlsx"))
        || info.completeBaseName().isEmpty() || name.size() > 180) {
        //% "Choose a PDF or Excel file name with at most 180 characters."
        emit errorOccurred(qtTrId("IOSFilesInvalidExportName"));
        return {};
    }
    const QDir directory(IOSDocumentAccess::managedDirectory());
    if (!QDir().mkpath(directory.absolutePath())) {
        //% "TrePDF could not create a local document. Check the available storage."
        emit errorOccurred(qtTrId("IOSFilesLocalStorageFailed"));
        return {};
    }
    QString path = directory.filePath(name);
    for (int index = 2; QFileInfo::exists(path) || m_reservedExportPaths.contains(path); ++index)
        path = directory.filePath(info.completeBaseName() + QStringLiteral(" (%1).").arg(index) + suffix);
    m_reservedExportPaths.insert(path);
    return QUrl::fromLocalFile(path);
}

bool PlatformService::exportNativeFile(const QUrl &localUrl)
{
    if (m_nativeFilesBusy)
        return false;
    const QString suffix = QFileInfo(localUrl.toLocalFile()).suffix().toLower();
    if (!IOSDocumentAccess::isManagedFile(localUrl)
        || (suffix != QStringLiteral("pdf") && suffix != QStringLiteral("xlsx"))) {
        //% "Save the document in TrePDF before exporting it to Files."
        emit errorOccurred(qtTrId("IOSFilesExportNeedsLocalFile"));
        return false;
    }
    setNativeFilesBusy(true);
    const QPointer<PlatformService> guard(this);
    if (IOSDocumentAccess::exportFile(localUrl, [guard, localUrl](IOSDocumentAccess::Result result) {
            if (!guard)
                return;
            guard->setNativeFilesBusy(false);
            if (!result.error.isEmpty())
                emit guard->errorOccurred(result.error);
            emit guard->exportFinished(localUrl, result.cancelled || !result.error.isEmpty());
            guard->importPendingNativeFiles();
        })) {
        return true;
    }
    setNativeFilesBusy(false);
    //% "The Files picker is not available. Close any open dialog and try again."
    emit errorOccurred(qtTrId("IOSFilesPickerUnavailable"));
    return false;
}

void PlatformService::importPendingNativeFiles()
{
    if (m_nativeFilesBusy || m_pendingNativeFiles.isEmpty())
        return;
    setNativeFilesBusy(true);
    const auto urls = std::exchange(m_pendingNativeFiles, {});
    const QPointer<PlatformService> guard(this);
    IOSDocumentAccess::importPdfFiles(urls, [guard](IOSDocumentAccess::Result result) {
        if (!guard)
            return;
        guard->setNativeFilesBusy(false);
        if (!result.error.isEmpty())
            emit guard->errorOccurred(result.error);
        guard->enqueueFiles(result.files);
        guard->importPendingNativeFiles();
    });
}

void PlatformService::enqueueFiles(const QList<QUrl> &urls)
{
    QList<QUrl> managedUrls;
    bool exceeded = false;
    for (const QUrl &url : nativePdfFiles(urls)) {
        if (IOSDocumentAccess::isManagedFile(url)) {
            managedUrls.append(url);
        } else if (!m_pendingNativeFiles.contains(url)) {
            if (m_pendingNativeFiles.size() >= 32) {
                exceeded = true;
                continue;
            }
            // FileOpen events can precede QML. The iOS adapter copies external
            // documents before passing durable local paths to the shared queue.
            m_pendingNativeFiles.append(url);
        }
    }
    if (exceeded) {
        //% "Select at most 32 PDF documents at a time."
        emit errorOccurred(qtTrId("IOSFilesTooManyDocuments"));
    }
    if (m_fileOpenHandler && !managedUrls.isEmpty()) {
        QStringList arguments{QStringLiteral("--")};
        for (const QUrl &url : std::as_const(managedUrls))
            arguments.append(url.toLocalFile());
        m_fileOpenHandler->enqueueArguments(arguments);
    }
    importPendingNativeFiles();
}

bool PlatformService::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != QCoreApplication::instance() || event->type() != QEvent::FileOpen)
        return QObject::eventFilter(watched, event);
    const auto *openEvent = static_cast<QFileOpenEvent *>(event);
    enqueueFiles({openEvent->url()});
    event->accept();
    return true;
}
