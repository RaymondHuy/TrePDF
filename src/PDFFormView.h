#pragma once

#include "PDFFormController.h"

#include <QPointF>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QFocusEvent;
class QHoverEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;

class PDFFormView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFFormController *controller READ controller WRITE setController
                   NOTIFY controllerChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive
                   NOTIFY interactiveChanged FINAL)
    Q_PROPERTY(QVariantList choiceFields READ choiceFields
                   NOTIFY choiceFieldsChanged FINAL)

public:
    explicit PDFFormView(QQuickItem *parent = nullptr);

    PDFFormController *controller() const;
    int page() const;
    bool interactive() const;
    QVariantList choiceFields() const;

    void setController(PDFFormController *controller);
    void setPage(int page);
    void setInteractive(bool interactive);
    Q_INVOKABLE void selectChoiceOption(int sourcePage,
                                        int annotationIndex,
                                        int optionIndex,
                                        bool selected,
                                        bool toggle,
                                        const QString &identityToken);
    Q_INVOKABLE void focusChoiceField(int sourcePage,
                                      int annotationIndex,
                                      const QString &identityToken);

signals:
    void controllerChanged();
    void pageChanged();
    void interactiveChanged();
    void choiceFieldsChanged();
    void choiceFieldFocusFinished(int sourcePage,
                                  int annotationIndex,
                                  const QString &identityToken,
                                  bool focused);

protected:
    bool event(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    int eventModifiers(Qt::KeyboardModifiers modifiers,
                       bool leftButtonDown = false,
                       bool autoRepeat = false) const;
    static int virtualKey(int qtKey);
    void sendPointerEvent(PDFFormWorker::PointerEventType type,
                          const QPointF &position,
                          Qt::KeyboardModifiers modifiers,
                          bool leftButtonDown);
    void requestChoiceFields();
    void loadChoiceFields();
    void clearChoiceFields();
    void updateInputMethod();
    void updateCursor();

    QPointer<PDFFormController> m_controller;
    int m_page = 0;
    bool m_interactive = false;
    bool m_pointerInside = false;
    bool m_mouseDown = false;
    QPointF m_lastPointerPosition;
    QVariantList m_choiceFields;
    quint64 m_choiceFieldsRequestSerial = 0;
    QTimer m_choiceFieldsRefreshTimer;
};
