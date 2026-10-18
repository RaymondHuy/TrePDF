#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
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
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY stateChanged)
    Q_PROPERTY(QString editReason READ editReason NOTIFY stateChanged)
    Q_PROPERTY(QString sourceNotice READ sourceNotice NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(QVariantList objects READ objects NOTIFY stateChanged)

public:
    explicit BrowserDocument(QObject *parent = nullptr);
    ~BrowserDocument() override;
    bool ready() const { return m_ready; }
    bool busy() const { return m_busy; }
    QString fileName() const { return m_fileName; }
    QString error() const { return m_error; }
    QVariantList pages() const { return m_pages; }
    int pageCount() const { return m_pages.size(); }
    bool canEdit() const { return m_canEdit; }
    QString editReason() const { return m_editReason; }
    QString sourceNotice() const { return m_sourceNotice; }
    bool dirty() const { return m_dirty; }
    bool canUndo() const { return m_canUndo; }
    bool canRedo() const { return m_canRedo; }
    QVariantList objects() const { return m_objects; }
    Q_INVOKABLE void openFile();
    Q_INVOKABLE void closeFile();
    Q_INVOKABLE void downloadFile();
    Q_INVOKABLE bool replaceImage(int id);
    Q_INVOKABLE bool applyEdit(const QVariantMap &command);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void setPendingInput(bool pending);
    int requestPage(PageRaster *item, int page, int width);
    void cancelRequest(int request);
    void receiveMessage(const QByteArray &json);
    void receivePixels(int request, int generation, int width, int height,
                       const unsigned char *bytes);

signals:
    void stateChanged();
    void pagesChanged();
    void reset();
    void edited(int selectedId);

private:
    void clearPages();
    QHash<int, QPointer<PageRaster>> m_requests;
    QVariantList m_pages;
    QVariantList m_objects;
    QString m_fileName;
    QString m_error;
    QString m_editReason;
    QString m_sourceNotice;
    int m_generation = 0;
    int m_nextRequest = 0;
    bool m_ready = false;
    bool m_busy = false;
    bool m_canEdit = false;
    bool m_dirty = false;
    bool m_canUndo = false;
    bool m_canRedo = false;
};
