#pragma once

#include "PDFDocument.h"

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

class PDFPageItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PDFPageView)
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(QColor pageColor READ pageColor WRITE setPageColor NOTIFY pageColorChanged FINAL)

public:
    explicit PDFPageItem(QQuickItem *parent = nullptr);

    PDFDocument *document() const;
    int page() const;
    qreal zoom() const;
    QColor pageColor() const;

    void setDocument(PDFDocument *document);
    void setPage(int page);
    void setZoom(qreal zoom);
    void setPageColor(const QColor &pageColor);

    void paint(QPainter *painter) override;

signals:
    void documentChanged();
    void pageChanged();
    void zoomChanged();
    void pageColorChanged();

private:
    void refreshMetrics();
    void invalidateCache();

    QPointer<PDFDocument> m_document;
    int m_page = 0;
    qreal m_zoom = 1.0;
    QColor m_pageColor = Qt::white;
    bool m_cacheDirty = true;
    QImage m_cachedImage;
    QSize m_cachedPixelSize;
    int m_cachedPage = -1;
    qreal m_cachedZoom = 0.0;
    PDFDocument *m_cachedDocument = nullptr;
};
