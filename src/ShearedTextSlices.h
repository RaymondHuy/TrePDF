#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// The source TextEdit owns shaping, input, preedit, selection and the cursor.
// Slice its single live texture at visual line and slope boundaries.
class ShearedTextSlices : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *editor READ editor WRITE setEditor NOTIFY editorChanged FINAL)
    Q_PROPERTY(QQuickItem *capture READ capture WRITE setCapture NOTIFY captureChanged FINAL)
    Q_PROPERTY(QRectF captureRect READ captureRect NOTIFY captureGeometryChanged FINAL)
    Q_PROPERTY(QSize captureSize READ captureSize NOTIFY captureGeometryChanged FINAL)
    Q_PROPERTY(QVariantList characterSpans READ characterSpans WRITE setCharacterSpans NOTIFY characterSpansChanged FINAL)

public:
    explicit ShearedTextSlices(QQuickItem *parent = nullptr);
    QQuickItem *editor() const { return m_editor; }
    QQuickItem *capture() const { return m_capture; }
    QVariantList characterSpans() const { return m_spans; }
    QRectF captureRect() const { return m_captureRect; }
    QSize captureSize() const { return m_captureSize; }
    void setEditor(QQuickItem *editor);
    void setCapture(QQuickItem *capture);
    void setCharacterSpans(const QVariantList &spans);
    Q_INVOKABLE QPointF unmapPoint(const QPointF &point) const;

signals:
    void captureGeometryChanged();
    void editorChanged();
    void captureChanged();
    void characterSpansChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &geometry, const QRectF &oldGeometry) override;

private:
    struct Slice {
        QRectF rect;
        QRectF paintRect;
        qreal slope;
        qreal baseline;
    };

    void attachDocument();
    void refreshCapture();
    QRectF m_captureRect;
    QSize m_captureSize;
    QMetaObject::Connection m_windowConnection;
    QPointer<QQuickItem> m_editor;
    QPointer<QQuickItem> m_capture;
    QPointer<QQuickTextDocument> m_quickDocument;
    QPointer<QTextDocument> m_document;
    QVariantList m_spans;
    QVector<Slice> m_slices;
    bool m_forwarding = false;
};
