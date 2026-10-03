#include "PDFFileOpenHandler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QJSEngine>
#include <QQmlEngine>
#include <QSet>
#include <QTimer>

#include <utility>

PDFFileOpenHandler::PDFFileOpenHandler(QObject *parent)
    : QObject(parent)
{
}

PDFFileOpenHandler *PDFFileOpenHandler::create(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(scriptEngine);
    auto *application = QCoreApplication::instance();
    if (!application || engine->thread() != application->thread())
        return nullptr;

    auto *handler = application->findChild<PDFFileOpenHandler *>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!handler) {
        handler = new PDFFileOpenHandler(application);
        application->installEventFilter(handler);
    }
    QJSEngine::setObjectOwnership(handler, QJSEngine::CppOwnership);
    return handler;
}

QList<QUrl> PDFFileOpenHandler::localPdfFiles(const QList<QUrl> &urls) const
{
    QList<QUrl> files;
    QSet<QString> paths;
    for (const QUrl &url : urls) {
        if (!url.isValid() || !url.isLocalFile() || url.hasQuery() || url.hasFragment()
            || !url.userInfo().isEmpty() || url.port() != -1
            || (!url.host().isEmpty()
                && url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0)) {
            continue;
        }

        QUrl localUrl = url;
        localUrl.setHost(QString());
        const QFileInfo file(localUrl.toLocalFile());
        if (!file.isFile()
            || file.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString path = file.canonicalFilePath();
        if (path.isEmpty() || paths.contains(path))
            continue;
        paths.insert(path);
        files.append(QUrl::fromLocalFile(path));
    }
    return files;
}

void PDFFileOpenHandler::enqueueArguments(const QStringList &arguments)
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

QList<QUrl> PDFFileOpenHandler::takePendingFiles()
{
    return std::exchange(m_pendingFiles, {});
}

void PDFFileOpenHandler::enqueueFiles(const QList<QUrl> &urls)
{
    const QList<QUrl> files = localPdfFiles(urls);
    for (const QUrl &file : files) {
        if (!m_pendingFiles.contains(file))
            m_pendingFiles.append(file);
    }
    if (m_pendingFiles.isEmpty() || m_notificationQueued)
        return;

    // Finder may deliver requests before the QML window exists. Keep the batch
    // until QML drains it, and never open a document inside a native event.
    m_notificationQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_notificationQueued = false;
        if (!m_pendingFiles.isEmpty())
            emit filesPending();
    });
}

bool PDFFileOpenHandler::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != QCoreApplication::instance() || event->type() != QEvent::FileOpen)
        return QObject::eventFilter(watched, event);

    const auto *openEvent = static_cast<QFileOpenEvent *>(event);
    enqueueFiles({openEvent->url()});
    event->accept();
    return true;
}
