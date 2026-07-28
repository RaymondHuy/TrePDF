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
    Q_PROPERTY(bool sourceFieldsVisible READ sourceFieldsVisible
                   WRITE setSourceFieldsVisible
                   NOTIFY sourceFieldsVisibleChanged FINAL)
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
    bool sourceFieldsVisible() const;
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
    void setSourceFieldsVisible(bool visible);
    void setAccentColor(const QColor &color);

signals:
    void controllerChanged();
    void pageChanged();
    void pooledChanged();
    void formsActiveChanged();
    void fieldsVisibleChanged();
    void fieldsInteractiveChanged();
    void sourceFieldsVisibleChanged();
    void accentColorChanged();
    void activeFieldChanged();
    void fieldActivated(const QString &fieldId);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;

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
    QRectF itemRect(const QRectF &normalizedRect) const;

    PDFFormDesignerController *m_controller = nullptr;
    int m_page = -1;
    bool m_pooled = false;
    bool m_formsActive = false;
    bool m_fieldsVisible = false;
    bool m_fieldsInteractive = false;
    bool m_sourceFieldsVisible = false;
    QColor m_accentColor = QColor(QStringLiteral("#d85f12"));
    QFont m_renderFont;
    QVector<RenderField> m_renderFields;
    QString m_activeFieldId;
    QRectF m_activeFieldRect;
    QString m_activeFieldValue;
    int m_activeFieldType = -1;
    int m_activeFieldFlags = PDFFormDesignerController::NoFieldFlags;
    bool m_activeFieldSourceBacked = false;
};
