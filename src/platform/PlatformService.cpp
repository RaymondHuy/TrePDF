#include "PlatformService.h"
#include "../PDFFileOpenHandler.h"
#include <QCoreApplication>
#include <QJSEngine>
#include <QQmlEngine>

PlatformService::PlatformService(PDFFileOpenHandler *handler, QObject *parent)
    : QObject(parent), m_fileOpenHandler(handler)
{
}

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
    if (m_fileOpenHandler)
        m_fileOpenHandler->enqueueArguments(arguments);
}

bool PlatformService::nativeFilesAvailable() const { return false; }
bool PlatformService::openNativePdfFiles() { return false; }
bool PlatformService::openNativeImageFile() { return false; }
QList<QUrl> PlatformService::nativePdfFiles(const QList<QUrl> &urls) const
{
    return m_fileOpenHandler ? m_fileOpenHandler->localPdfFiles(urls) : QList<QUrl>{};
}
bool PlatformService::importNativePdfFiles(const QList<QUrl> &) { return false; }
QUrl PlatformService::createLocalExportUrl(const QString &) { return {}; }
bool PlatformService::exportNativeFile(const QUrl &) { return false; }
bool PlatformService::eventFilter(QObject *watched, QEvent *event)
{
    return QObject::eventFilter(watched, event);
}
