#pragma once

#include "PDFFormDesignerController.h"

#include <QColor>
#include <QFont>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class FormFieldLayer : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFFormDesignerController *controller READ controller
                   WRITE setController NOTIFY controllerChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(bool pooled READ pooled WRITE setPooled NOTIFY pooledChanged FINAL)
    Q_PROPERTY(bool formsActive READ formsActive WRITE setFormsActive
                   NOTIFY formsActiveChanged FINAL)
    Q_PROPERTY(bool fieldsVisible READ fieldsVisible WRITE setFieldsVisible
                   NOTIFY fieldsVisibleChanged FINAL)
    Q_PROPERTY(bool fieldsInteractive READ fieldsInteractive
                   WRITE setFieldsInteractive NOTIFY fieldsInteractiveChanged FINAL)
    Q_PROPERTY(bool moveEnabled READ moveEnabled WRITE setMoveEnabled
                   NOTIFY moveEnabledChanged FINAL)
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged FINAL)
    Q_PROPERTY(bool sourceFieldsVisible READ sourceFieldsVisible
                   WRITE setSourceFieldsVisible
                   NOTIFY sourceFieldsVisibleChanged FINAL)
    Q_PROPERTY(qreal pointScale READ pointScale WRITE setPointScale
                   NOTIFY pointScaleChanged FINAL)
    Q_PROPERTY(qreal textFieldFontPointSize READ textFieldFontPointSize
                   CONSTANT FINAL)
    Q_PROPERTY(qreal textFieldBorderPointWidth READ textFieldBorderPointWidth
                   CONSTANT FINAL)
    Q_PROPERTY(QColor textFieldBorderColor READ textFieldBorderColor
                   CONSTANT FINAL)
    Q_PROPERTY(qreal textFieldMinimumHorizontalPaddingPoints
                   READ textFieldMinimumHorizontalPaddingPoints
                   CONSTANT FINAL)
    Q_PROPERTY(qreal textFieldHorizontalPaddingRatio
                   READ textFieldHorizontalPaddingRatio CONSTANT FINAL)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor
                   NOTIFY accentColorChanged FINAL)
    Q_PROPERTY(QString activeFieldId READ activeFieldId
                   NOTIFY activeFieldChanged FINAL)
    Q_PROPERTY(QRectF activeFieldRect READ activeFieldRect
                   NOTIFY activeFieldChanged FINAL)
    Q_PROPERTY(QString activeFieldValue READ activeFieldValue
                   NOTIFY activeFieldChanged FINAL)
    Q_PROPERTY(int activeFieldType READ activeFieldType
                   NOTIFY activeFieldChanged FINAL)
    Q_PROPERTY(int activeFieldFlags READ activeFieldFlags
                   NOTIFY activeFieldChanged FINAL)
    Q_PROPERTY(bool activeFieldSourceBacked READ activeFieldSourceBacked
                   NOTIFY activeFieldChanged FINAL)

public:
    explicit FormFieldLayer(QQuickItem *parent = nullptr);

    PDFFormDesignerController *controller() const;
    int page() const;
    bool pooled() const;
    bool formsActive() const;
    bool fieldsVisible() const;
    bool fieldsInteractive() const;
    bool moveEnabled() const;
    bool moving() const;
    bool sourceFieldsVisible() const;
    qreal pointScale() const;
    qreal textFieldFontPointSize() const;
    qreal textFieldBorderPointWidth() const;
    QColor textFieldBorderColor() const;
    qreal textFieldMinimumHorizontalPaddingPoints() const;
    qreal textFieldHorizontalPaddingRatio() const;
    QColor accentColor() const;
    QString activeFieldId() const;
    QRectF activeFieldRect() const;
    QString activeFieldValue() const;
    int activeFieldType() const;
    int activeFieldFlags() const;
    bool activeFieldSourceBacked() const;

    void setController(PDFFormDesignerController *controller);
    void setPage(int page);
    void setPooled(bool pooled);
    void setFormsActive(bool active);
    void setFieldsVisible(bool visible);
    void setFieldsInteractive(bool interactive);
    void setMoveEnabled(bool enabled);
    void setSourceFieldsVisible(bool visible);
    void setPointScale(qreal scale);
    void setAccentColor(const QColor &color);

    Q_INVOKABLE bool beginFieldMove(const QString &fieldId);
    Q_INVOKABLE bool updateFieldMove(qreal normalizedDeltaX,
                                     qreal normalizedDeltaY);
    Q_INVOKABLE bool finishFieldMove();
    Q_INVOKABLE void cancelFieldMove();

signals:
    void controllerChanged();
    void pageChanged();
    void pooledChanged();
    void formsActiveChanged();
    void fieldsVisibleChanged();
    void fieldsInteractiveChanged();
    void moveEnabledChanged();
    void movingChanged();
    void sourceFieldsVisibleChanged();
    void pointScaleChanged();
    void accentColorChanged();
    void activeFieldChanged();
    void fieldActivated(const QString &fieldId);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;

private:
    struct RenderField
    {
        QString id;
        int type = PDFFormDesignerController::Text;
        QRectF normalizedRect;
        QString value;
        int flags = PDFFormDesignerController::NoFieldFlags;
        bool sourceBacked = false;
        bool selected = false;
    };

    void refreshFields();
    void refreshActiveField();
    void resetMoveState();
    void resetPointerInteraction();
    QRectF itemRect(const QRectF &normalizedRect) const;

    PDFFormDesignerController *m_controller = nullptr;
    int m_page = -1;
    bool m_pooled = false;
    bool m_formsActive = false;
    bool m_fieldsVisible = false;
    bool m_fieldsInteractive = false;
    bool m_moveEnabled = false;
    bool m_moving = false;
    bool m_sourceFieldsVisible = false;
    qreal m_pointScale = 1.0;
    QColor m_accentColor = QColor(QStringLiteral("#d85f12"));
    QFont m_renderFont;
    QFont m_textFieldRenderFont;
    QVector<RenderField> m_renderFields;
    QString m_activeFieldId;
    QRectF m_activeFieldRect;
    QString m_activeFieldValue;
    int m_activeFieldType = -1;
    int m_activeFieldFlags = PDFFormDesignerController::NoFieldFlags;
    bool m_activeFieldSourceBacked = false;
    QString m_movingFieldId;
    QRectF m_moveStartRect;
    QRectF m_movePreviewRect;
    QString m_pressedFieldId;
    QPointF m_pressPosition;
};
