#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class PageRaster;

// Browser-only platform adapter. PDF bytes and PDFium handles never enter the
// native editor's document/session state.
class BrowserDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QVariantList pages READ pages NOTIFY pagesChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged)

public:
    explicit BrowserDocument(QObject *parent = nullptr);
    ~BrowserDocument() override;
    bool ready() const { return m_ready; }
    bool busy() const { return m_busy; }
    QString fileName() const { return m_fileName; }
    QString error() const { return m_error; }
    QVariantList pages() const { return m_pages; }
    int pageCount() const { return m_pages.size(); }
    Q_INVOKABLE void openFile();
    Q_INVOKABLE void closeFile();
    Q_INVOKABLE void downloadFile();
    int requestPage(PageRaster *item, int page, int width);
    void cancelRequest(int request);
    void receiveMessage(const QByteArray &json);
    void receivePixels(int request, int generation, int width, int height,
                       const unsigned char *bytes);

signals:
    void stateChanged();
    void pagesChanged();
    void reset();

private:
    void clearPages();
    QHash<int, QPointer<PageRaster>> m_requests;
    QVariantList m_pages;
    QString m_fileName;
    QString m_error;
    int m_generation = 0;
    int m_nextRequest = 0;
    bool m_ready = false;
    bool m_busy = false;
};
