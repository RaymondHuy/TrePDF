#pragma once

#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

class QJSEngine;
class QQmlEngine;

class PDFFileOpenHandler : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(FileOpenHandler)
    QML_SINGLETON

public:
    // No default constructor: QML must use the application-owned factory instance.
    explicit PDFFileOpenHandler(QObject *parent);
    static PDFFileOpenHandler *create(QQmlEngine *engine, QJSEngine *scriptEngine);

    void enqueueArguments(const QStringList &arguments);
    Q_INVOKABLE QList<QUrl> takePendingFiles();
    Q_INVOKABLE QList<QUrl> localPdfFiles(const QList<QUrl> &urls) const;

signals:
    void filesPending();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void enqueueFiles(const QList<QUrl> &urls);

    QList<QUrl> m_pendingFiles;
    bool m_notificationQueued = false;
};
