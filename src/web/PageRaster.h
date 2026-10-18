#pragma once

#include "BrowserDocument.h"
#include <QQuickItem>
#include <QTimer>

class PageRaster : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(BrowserDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged)
    Q_PROPERTY(int renderWidth READ renderWidth WRITE setRenderWidth NOTIFY renderWidthChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)

public:
    explicit PageRaster(QQuickItem *parent = nullptr);
    ~PageRaster() override;
    BrowserDocument *document() const { return m_document; }
    void setDocument(BrowserDocument *document);
    int page() const { return m_page; }
    void setPage(int page);
    int renderWidth() const { return m_renderWidth; }
    void setRenderWidth(int width);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    void acceptImage(int request, QImage image);
    void acceptError(int request, const QString &error);

signals:
    void documentChanged();
    void pageChanged();
    void renderWidthChanged();
    void activeChanged();
    void statusChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;

private:
    void invalidate(bool clear);
    QPointer<BrowserDocument> m_document;
    QTimer m_debounce;
    QImage m_pendingImage;
    QString m_error;
    int m_page = -1;
    int m_renderWidth = 0;
    int m_request = 0;
    bool m_active = false;
    bool m_loading = false;
    bool m_clear = true;
    QMetaObject::Connection m_sceneGraphConnection;
};
