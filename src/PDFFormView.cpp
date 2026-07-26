#include "PDFFormView.h"

#include <QClipboard>
#include <QCursor>
#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>

#include <fpdf_fwlevent.h>

PDFFormView::PDFFormView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemAcceptsInputMethod, true);
    setAcceptedMouseButtons(Qt::NoButton);
    setCursor(Qt::ArrowCursor);
}

PDFFormController *PDFFormView::controller() const
{
    return m_controller;
}

int PDFFormView::page() const
{
    return m_page;
}

bool PDFFormView::interactive() const
{
    return m_interactive;
}

void PDFFormView::setController(PDFFormController *controller)
{
    if (m_controller == controller) {
        return;
    }
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    if (m_controller) {
        connect(
            m_controller,
            &PDFFormController::inputStateChanged,
            this,
            &PDFFormView::updateInputMethod);
        connect(
            m_controller,
            &PDFFormController::inputStateChanged,
            this,
            &PDFFormView::updateCursor);
        connect(m_controller, &QObject::destroyed, this, [this]() {
            m_controller = nullptr;
            m_pointerInside = false;
            m_mouseDown = false;
            updateInputMethod();
            updateCursor();
            emit controllerChanged();
        });
    }
    updateInputMethod();
    updateCursor();
    emit controllerChanged();
}

void PDFFormView::setPage(int page)
{
    if (m_page == page) {
        return;
    }
    m_page = page;
    emit pageChanged();
}

void PDFFormView::setInteractive(bool interactive)
{
    if (m_interactive == interactive) {
        return;
    }
    m_interactive = interactive;
    setAcceptedMouseButtons(interactive ? Qt::LeftButton : Qt::NoButton);
    setAcceptHoverEvents(interactive);
    if (!m_interactive) {
        m_pointerInside = false;
        m_mouseDown = false;
        if (hasActiveFocus()) {
            setFocus(false);
        }
    }
    updateInputMethod();
    updateCursor();
    emit interactiveChanged();
}

bool PDFFormView::event(QEvent *event)
{
    if (m_interactive
        && hasActiveFocus()
        && event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->matches(QKeySequence::Copy)
            || keyEvent->matches(QKeySequence::Cut)
            || keyEvent->matches(QKeySequence::Paste)
            || keyEvent->matches(QKeySequence::SelectAll)
            || keyEvent->matches(QKeySequence::Undo)
            || keyEvent->matches(QKeySequence::Redo)
            || keyEvent->key() == Qt::Key_Tab
            || keyEvent->key() == Qt::Key_Backtab) {
            event->accept();
            return true;
        }
    }
    return QQuickItem::event(event);
}

void PDFFormView::focusInEvent(QFocusEvent *event)
{
    QQuickItem::focusInEvent(event);
    updateInputMethod();
}

void PDFFormView::focusOutEvent(QFocusEvent *event)
{
    if (m_controller) {
        m_controller->killFocus();
    }
    m_mouseDown = false;
    QQuickItem::focusOutEvent(event);
    updateInputMethod();
}

void PDFFormView::hoverEnterEvent(QHoverEvent *event)
{
    if (!m_interactive || !m_controller) {
        event->ignore();
        return;
    }
    m_pointerInside = true;
    m_lastPointerPosition = event->position();
    sendPointerEvent(
        PDFFormWorker::PointerEventType::Move,
        event->position(),
        event->modifiers(),
        false);
    setCursor(Qt::ArrowCursor);
    event->accept();
}

void PDFFormView::hoverMoveEvent(QHoverEvent *event)
{
    if (!m_interactive || !m_controller) {
        event->ignore();
        return;
    }
    m_pointerInside = true;
    m_lastPointerPosition = event->position();
    sendPointerEvent(
        PDFFormWorker::PointerEventType::Move,
        event->position(),
        event->modifiers(),
        false);
    event->accept();
}

void PDFFormView::hoverLeaveEvent(QHoverEvent *event)
{
    m_pointerInside = false;
    if (m_interactive && m_controller) {
        sendPointerEvent(
            PDFFormWorker::PointerEventType::Move,
            QPointF(-1.0, -1.0),
            Qt::NoModifier,
            false);
    }
    updateCursor();
    event->accept();
}

void PDFFormView::mousePressEvent(QMouseEvent *event)
{
    if (!m_interactive || !m_controller || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_pointerInside = true;
    m_mouseDown = true;
    forceActiveFocus(Qt::MouseFocusReason);
    m_lastPointerPosition = event->position();
    sendPointerEvent(
        PDFFormWorker::PointerEventType::Press,
        event->position(),
        event->modifiers(),
        true);
    event->accept();
}

void PDFFormView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_interactive || !m_controller || !m_mouseDown) {
        event->ignore();
        return;
    }
    m_lastPointerPosition = event->position();
    sendPointerEvent(
        PDFFormWorker::PointerEventType::Move,
        event->position(),
        event->modifiers(),
        true);
    event->accept();
}

void PDFFormView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_interactive || !m_controller || !m_mouseDown
        || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_lastPointerPosition = event->position();
    sendPointerEvent(
        PDFFormWorker::PointerEventType::Release,
        event->position(),
        event->modifiers(),
        false);
    m_mouseDown = false;
    event->accept();
}

void PDFFormView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_interactive || !m_controller || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_pointerInside = true;
    m_mouseDown = true;
    m_lastPointerPosition = event->position();
    sendPointerEvent(
        PDFFormWorker::PointerEventType::DoubleClick,
        event->position(),
        event->modifiers(),
        true);
    event->accept();
}

void PDFFormView::mouseUngrabEvent()
{
    if (m_mouseDown && m_controller) {
        sendPointerEvent(
            PDFFormWorker::PointerEventType::Release,
            QPointF(-1.0, -1.0),
            Qt::NoModifier,
            false);
    }
    m_mouseDown = false;
    QQuickItem::mouseUngrabEvent();
}

void PDFFormView::keyPressEvent(QKeyEvent *event)
{
    if (!m_interactive || !m_controller) {
        event->ignore();
        return;
    }

    if (event->matches(QKeySequence::Copy)) {
        m_controller->copySelection(false);
    } else if (event->matches(QKeySequence::Cut)) {
        m_controller->copySelection(true);
    } else if (event->matches(QKeySequence::Paste)) {
        const QClipboard *clipboard = QGuiApplication::clipboard();
        m_controller->replaceSelection(
            clipboard ? clipboard->text() : QString());
    } else if (event->matches(QKeySequence::SelectAll)) {
        m_controller->selectAll();
    } else if (event->matches(QKeySequence::Undo)) {
        m_controller->undo();
    } else if (event->matches(QKeySequence::Redo)) {
        m_controller->redo();
    } else {
        const int key = virtualKey(event->key());
        QString text = event->text();
        const bool commandModifier
            = event->modifiers().testFlag(Qt::MetaModifier)
            || (event->modifiers().testFlag(Qt::ControlModifier)
                && !event->modifiers().testFlag(Qt::AltModifier));
        if (commandModifier) {
            text.clear();
        }
        if (event->key() == Qt::Key_Backtab) {
            m_controller->keyEvent(
                FWL_VKEY_Tab,
                eventModifiers(
                    event->modifiers() | Qt::ShiftModifier,
                    false,
                    event->isAutoRepeat()),
                {});
        } else {
            m_controller->keyEvent(
                key,
                eventModifiers(
                    event->modifiers(),
                    false,
                    event->isAutoRepeat()),
                text);
        }
    }
    event->accept();
}

void PDFFormView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!m_interactive
        || !m_controller
        || !m_controller->acceptsTextInput()) {
        event->ignore();
        return;
    }
    if (!event->commitString().isEmpty()) {
        m_controller->replaceSelection(event->commitString());
    }
    // PDFium does not expose a preedit API. The platform IME still owns and
    // displays composition; committed text is inserted into the active field.
    event->accept();
}

QVariant PDFFormView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return m_interactive
            && m_controller
            && m_controller->acceptsTextInput();
    case Qt::ImHints:
        return QVariant::fromValue(Qt::ImhNone);
    case Qt::ImCursorRectangle:
        return QRectF(
            m_lastPointerPosition.x(),
            m_lastPointerPosition.y(),
            1.0,
            18.0);
    case Qt::ImSurroundingText:
        return QString();
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return 0;
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

int PDFFormView::eventModifiers(
    Qt::KeyboardModifiers modifiers,
    bool leftButtonDown,
    bool autoRepeat) const
{
    int result = 0;
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        result |= FWL_EVENTFLAG_ShiftKey;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        result |= FWL_EVENTFLAG_ControlKey;
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        result |= FWL_EVENTFLAG_AltKey;
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        result |= FWL_EVENTFLAG_MetaKey;
    }
    if (modifiers.testFlag(Qt::KeypadModifier)) {
        result |= FWL_EVENTFLAG_KeyPad;
    }
    if (leftButtonDown) {
        result |= FWL_EVENTFLAG_LeftButtonDown;
    }
    if (autoRepeat) {
        result |= FWL_EVENTFLAG_AutoRepeat;
    }
    return result;
}

int PDFFormView::virtualKey(int qtKey)
{
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return FWL_VKEY_0 + qtKey - Qt::Key_0;
    }
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return FWL_VKEY_A + qtKey - Qt::Key_A;
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return FWL_VKEY_F1 + qtKey - Qt::Key_F1;
    }

    switch (qtKey) {
    case Qt::Key_Backspace:
        return FWL_VKEY_Back;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return FWL_VKEY_Tab;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return FWL_VKEY_Return;
    case Qt::Key_Escape:
        return FWL_VKEY_Escape;
    case Qt::Key_Space:
        return FWL_VKEY_Space;
    case Qt::Key_PageUp:
        return FWL_VKEY_Prior;
    case Qt::Key_PageDown:
        return FWL_VKEY_Next;
    case Qt::Key_End:
        return FWL_VKEY_End;
    case Qt::Key_Home:
        return FWL_VKEY_Home;
    case Qt::Key_Left:
        return FWL_VKEY_Left;
    case Qt::Key_Up:
        return FWL_VKEY_Up;
    case Qt::Key_Right:
        return FWL_VKEY_Right;
    case Qt::Key_Down:
        return FWL_VKEY_Down;
    case Qt::Key_Insert:
        return FWL_VKEY_Insert;
    case Qt::Key_Delete:
        return FWL_VKEY_Delete;
    default:
        return FWL_VKEY_Unknown;
    }
}

void PDFFormView::sendPointerEvent(
    PDFFormWorker::PointerEventType type,
    const QPointF &position,
    Qt::KeyboardModifiers modifiers,
    bool leftButtonDown)
{
    if (!m_controller || width() <= 0.0 || height() <= 0.0) {
        return;
    }
    m_controller->pointerEvent(
        m_page,
        type,
        eventModifiers(modifiers, leftButtonDown),
        position,
        QSize(qMax(1, qRound(width())), qMax(1, qRound(height()))));
}

void PDFFormView::updateInputMethod()
{
    if (m_interactive
        && hasActiveFocus()
        && m_controller
        && !m_controller->hasFocus()
        && !m_controller->focusRequestPending()
        && !m_mouseDown) {
        setFocus(false);
    }
    if (QInputMethod *inputMethod = QGuiApplication::inputMethod()) {
        inputMethod->update(
            Qt::ImEnabled
            | Qt::ImCursorRectangle
            | Qt::ImHints);
    }
}

void PDFFormView::updateCursor()
{
    Qt::CursorShape cursor = Qt::ArrowCursor;
    if (m_interactive
        && m_pointerInside
        && m_controller) {
        switch (m_controller->cursorType()) {
        case FXCT_VBEAM:
            cursor = Qt::IBeamCursor;
            break;
        case FXCT_HBEAM:
            cursor = Qt::SizeVerCursor;
            break;
        case FXCT_HAND:
            cursor = Qt::PointingHandCursor;
            break;
        case FXCT_NESW:
            cursor = Qt::SizeBDiagCursor;
            break;
        case FXCT_NWSE:
            cursor = Qt::SizeFDiagCursor;
            break;
        default:
            break;
        }
    }
    setCursor(cursor);
}
