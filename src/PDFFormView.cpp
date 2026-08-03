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
#include <QVariantMap>

#include <fpdf_fwlevent.h>

#include <utility>

PDFFormView::PDFFormView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemAcceptsInputMethod, true);
    setAcceptedMouseButtons(Qt::NoButton);
    setCursor(Qt::ArrowCursor);
    m_choiceFieldsRefreshTimer.setInterval(40);
    m_choiceFieldsRefreshTimer.setSingleShot(true);
    connect(
        &m_choiceFieldsRefreshTimer,
        &QTimer::timeout,
        this,
        &PDFFormView::loadChoiceFields);
}

PDFFormController *PDFFormView::controller() const
{
    return m_controller;
}

PDFFormDesignerController *PDFFormView::formDesignerController() const
{
    return m_formDesignerController;
}

bool PDFFormView::formDesignMode() const
{
    return m_formDesignMode;
}

int PDFFormView::page() const
{
    return m_page;
}

bool PDFFormView::interactive() const
{
    return m_interactive;
}

QVariantList PDFFormView::choiceFields() const
{
    if (!m_formDesignMode || !m_formDesignerController) {
        return m_choiceFields;
    }

    // The designer draws rehydrated TrePDF dropdowns at their draft rectangle.
    // Keeping the runtime overlay for the same source widget would expose a
    // stale second dropdown at the original rectangle after a move.
    QVariantList visibleFields;
    visibleFields.reserve(m_choiceFields.size());
    for (const QVariant &choiceFieldValue : m_choiceFields) {
        const QVariantMap choiceField = choiceFieldValue.toMap();
        if (m_formDesignerController->managesSourceChoiceField(
                choiceField.value(QStringLiteral("sourcePage")).toInt(),
                choiceField.value(QStringLiteral("annotationIndex")).toInt(),
                choiceField.value(QStringLiteral("identityToken")).toString())) {
            continue;
        }
        visibleFields.append(choiceFieldValue);
    }
    return visibleFields;
}

void PDFFormView::setController(PDFFormController *controller)
{
    if (m_controller == controller) {
        return;
    }
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    clearChoiceFields();
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
        connect(
            m_controller,
            &PDFFormController::formRenderStateChanged,
            this,
            [this](int page, quint64) {
                if (page < 0 || page == m_page) {
                    requestChoiceFields();
                }
            });
        connect(
            m_controller,
            &PDFFormController::interactiveChanged,
            this,
            &PDFFormView::requestChoiceFields);
        connect(
            m_controller,
            &PDFFormController::documentChanged,
            this,
            [this]() {
                clearChoiceFields();
                requestChoiceFields();
            });
        connect(m_controller, &QObject::destroyed, this, [this]() {
            m_controller = nullptr;
            m_pointerInside = false;
            m_pointerOverDesignerSource = false;
            m_designerPointerBlocked = false;
            m_mouseDown = false;
            ++m_choiceFieldsRequestSerial;
            m_choiceFieldsRefreshTimer.stop();
            clearChoiceFields();
            updateInputMethod();
            updateCursor();
            emit controllerChanged();
        });
    }
    requestChoiceFields();
    updateInputMethod();
    updateCursor();
    emit controllerChanged();
}

void PDFFormView::setFormDesignerController(
    PDFFormDesignerController *formDesignerController)
{
    if (m_formDesignerController == formDesignerController) {
        return;
    }
    if (m_formDesignerController) {
        disconnect(m_formDesignerController, nullptr, this, nullptr);
    }
    m_formDesignerController = formDesignerController;
    if (m_formDesignerController) {
        connect(
            m_formDesignerController,
            &PDFFormDesignerController::textFieldsChanged,
            this,
            [this](int page) {
                if (m_formDesignMode
                    && (page < 0 || page == m_page)) {
                    emit choiceFieldsChanged();
                }
            });
        connect(
            m_formDesignerController,
            &QObject::destroyed,
            this,
            [this]() {
                m_formDesignerController = nullptr;
                m_pointerOverDesignerSource = false;
                m_designerPointerBlocked = false;
                updateCursor();
                emit choiceFieldsChanged();
                emit formDesignerControllerChanged();
            });
    }
    m_pointerOverDesignerSource = false;
    m_designerPointerBlocked = false;
    updateCursor();
    emit choiceFieldsChanged();
    emit formDesignerControllerChanged();
}

void PDFFormView::setFormDesignMode(bool formDesignMode)
{
    if (m_formDesignMode == formDesignMode) {
        return;
    }
    m_formDesignMode = formDesignMode;
    m_pointerOverDesignerSource = false;
    m_designerPointerBlocked = false;
    updateCursor();
    emit choiceFieldsChanged();
    emit formDesignModeChanged();
}

void PDFFormView::setPage(int page)
{
    if (m_page == page) {
        return;
    }
    m_page = page;
    m_pointerOverDesignerSource = false;
    m_designerPointerBlocked = false;
    clearChoiceFields();
    requestChoiceFields();
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
        m_pointerOverDesignerSource = false;
        m_designerPointerBlocked = false;
        m_mouseDown = false;
        if (hasActiveFocus()) {
            setFocus(false);
        }
    }
    requestChoiceFields();
    updateInputMethod();
    updateCursor();
    emit interactiveChanged();
}

void PDFFormView::selectChoiceOption(
    int sourcePage,
    int annotationIndex,
    int optionIndex,
    bool selected,
    bool toggle,
    const QString &identityToken)
{
    if (!m_interactive
        || !m_controller
        || !m_controller->document()
        || m_controller->document()->sourcePageIndex(m_page)
            != sourcePage) {
        requestChoiceFields();
        return;
    }
    m_controller->selectChoiceOption(
        m_page,
        annotationIndex,
        optionIndex,
        selected,
        sourcePage,
        toggle,
        identityToken);
    forceActiveFocus(Qt::MouseFocusReason);
}

void PDFFormView::focusChoiceField(
    int sourcePage,
    int annotationIndex,
    const QString &identityToken)
{
    if (!m_interactive
        || !m_controller
        || !m_controller->document()
        || m_controller->document()->sourcePageIndex(m_page)
            != sourcePage) {
        requestChoiceFields();
        emit choiceFieldFocusFinished(
            sourcePage,
            annotationIndex,
            identityToken,
            false);
        return;
    }
    m_controller->focusChoiceField(
        m_page,
        annotationIndex,
        sourcePage,
        identityToken,
        [view = QPointer<PDFFormView>(this),
         controller = QPointer<PDFFormController>(m_controller),
         sourcePage,
         annotationIndex,
         identityToken](bool focused) {
            if (!view
                || view->m_controller != controller
                || !controller
                || !controller->document()
                || controller->document()->sourcePageIndex(view->m_page)
                    != sourcePage) {
                return;
            }
            emit view->choiceFieldFocusFinished(
                sourcePage,
                annotationIndex,
                identityToken,
                focused);
        });
    forceActiveFocus(Qt::MouseFocusReason);
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
    if (designerBlocksPointer(event->position())) {
        m_pointerOverDesignerSource = true;
        sendPointerEvent(
            PDFFormWorker::PointerEventType::Move,
            QPointF(-1.0, -1.0),
            event->modifiers(),
            false);
        updateCursor();
        event->accept();
        return;
    }
    m_pointerOverDesignerSource = false;
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
    const bool pointerOverDesignerSource
        = designerBlocksPointer(event->position());
    if (pointerOverDesignerSource) {
        if (!m_pointerOverDesignerSource) {
            sendPointerEvent(
                PDFFormWorker::PointerEventType::Move,
                QPointF(-1.0, -1.0),
                event->modifiers(),
                false);
        }
        m_pointerOverDesignerSource = true;
        updateCursor();
        event->accept();
        return;
    }
    m_pointerOverDesignerSource = false;
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
    m_pointerOverDesignerSource = false;
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
    m_lastPointerPosition = event->position();
    if (designerBlocksPointer(event->position())) {
        m_pointerOverDesignerSource = true;
        m_designerPointerBlocked = true;
        m_mouseDown = false;
        updateCursor();
        event->accept();
        return;
    }
    m_pointerOverDesignerSource = false;
    m_designerPointerBlocked = false;
    m_mouseDown = true;
    forceActiveFocus(Qt::MouseFocusReason);
    sendPointerEvent(
        PDFFormWorker::PointerEventType::Press,
        event->position(),
        event->modifiers(),
        true);
    event->accept();
}

void PDFFormView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_designerPointerBlocked) {
        m_lastPointerPosition = event->position();
        event->accept();
        return;
    }
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
    if (m_designerPointerBlocked
        && event->button() == Qt::LeftButton) {
        m_lastPointerPosition = event->position();
        m_designerPointerBlocked = false;
        event->accept();
        return;
    }
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
    m_lastPointerPosition = event->position();
    if (designerBlocksPointer(event->position())) {
        m_pointerOverDesignerSource = true;
        m_designerPointerBlocked = false;
        m_mouseDown = false;
        updateCursor();
        event->accept();
        return;
    }
    m_pointerOverDesignerSource = false;
    m_mouseDown = true;
    sendPointerEvent(
        PDFFormWorker::PointerEventType::DoubleClick,
        event->position(),
        event->modifiers(),
        true);
    event->accept();
}

void PDFFormView::mouseUngrabEvent()
{
    if (!m_designerPointerBlocked && m_mouseDown && m_controller) {
        sendPointerEvent(
            PDFFormWorker::PointerEventType::Release,
            QPointF(-1.0, -1.0),
            Qt::NoModifier,
            false);
    }
    m_designerPointerBlocked = false;
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

void PDFFormView::requestChoiceFields()
{
    ++m_choiceFieldsRequestSerial;
    m_choiceFieldsRefreshTimer.stop();
    if (!m_interactive || !m_controller || m_page < 0) {
        clearChoiceFields();
        return;
    }
    m_choiceFieldsRefreshTimer.start();
}

void PDFFormView::loadChoiceFields()
{
    if (!m_interactive || !m_controller || m_page < 0) {
        clearChoiceFields();
        return;
    }
    const quint64 requestSerial = m_choiceFieldsRequestSerial;
    const int requestedPage = m_page;
    QPointer<PDFFormView> view(this);
    QPointer<PDFFormController> controller(m_controller);
    m_controller->requestChoiceFields(
        requestedPage,
        [view,
         controller,
         requestedPage,
         requestSerial](
            QVector<PDFFormWorker::ChoiceField> fields) {
            if (!view
                || view->m_controller != controller
                || view->m_page != requestedPage
                || view->m_choiceFieldsRequestSerial != requestSerial
                || !view->m_interactive) {
                return;
            }

            QVariantList choiceFields;
            choiceFields.reserve(fields.size());
            for (const PDFFormWorker::ChoiceField &field : fields) {
                QVariantList selectedIndices;
                selectedIndices.reserve(field.selectedIndices.size());
                for (int selectedIndex : field.selectedIndices) {
                    selectedIndices.append(selectedIndex);
                }

                const QRectF &rect = field.normalizedRect;
                QVariantMap choiceField;
                choiceField.insert(
                    QStringLiteral("annotationIndex"),
                    field.annotationIndex);
                choiceField.insert(
                    QStringLiteral("sourcePage"),
                    field.sourcePage);
                choiceField.insert(
                    QStringLiteral("comboBox"),
                    field.fieldType == FPDF_FORMFIELD_COMBOBOX);
                choiceField.insert(QStringLiteral("x"), rect.x());
                choiceField.insert(QStringLiteral("y"), rect.y());
                choiceField.insert(QStringLiteral("width"), rect.width());
                choiceField.insert(QStringLiteral("height"), rect.height());
                choiceField.insert(
                    QStringLiteral("accessibleName"),
                    field.accessibleName);
                choiceField.insert(
                    QStringLiteral("identityToken"),
                    field.identityToken);
                choiceField.insert(
                    QStringLiteral("options"),
                    field.options);
                choiceField.insert(
                    QStringLiteral("selectedIndices"),
                    selectedIndices);
                choiceField.insert(
                    QStringLiteral("readOnly"),
                    field.readOnly);
                choiceField.insert(
                    QStringLiteral("multiSelect"),
                    field.multiSelect);
                choiceFields.append(choiceField);
            }
            if (view->m_choiceFields == choiceFields) {
                return;
            }
            view->m_choiceFields = std::move(choiceFields);
            emit view->choiceFieldsChanged();
        });
}

void PDFFormView::clearChoiceFields()
{
    if (m_choiceFields.isEmpty()) {
        return;
    }
    m_choiceFields.clear();
    emit choiceFieldsChanged();
}

bool PDFFormView::designerBlocksPointer(const QPointF &position) const
{
    if (!m_formDesignMode
        || !m_formDesignerController
        || m_page < 0
        || width() <= 0.0
        || height() <= 0.0) {
        return false;
    }

    return m_formDesignerController->sourceFieldContainsPoint(
        m_page,
        QPointF(position.x() / width(), position.y() / height()));
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
        && !m_pointerOverDesignerSource
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
