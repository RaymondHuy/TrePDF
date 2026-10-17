#pragma once

#include "PDFFormController.h"
#include "PDFFormDesignerController.h"

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

/**
 * @brief Per-page Qt Quick input bridge for the document's native PDF form session.
 *
 * Translates mouse, keyboard, focus, and committed input-method events into
 * PDFFormController requests. PDFium work remains in the controller/worker
 * pipeline; this item neither owns a PDFium document nor renders the page image.
 * It also exposes safe choice-field snapshots to the QML choice overlay and
 * prevents native interaction from competing with designer-managed fields.
 *
 * All methods and event handlers belong on the Qt Quick GUI thread. Controller
 * pointers are non-owning QPointers. Async choice results capture view/controller
 * identity, page, and request serial so a recycled page delegate cannot display
 * another page's fields. View interactivity is separate from the controller's
 * document-permission gate; callers must configure it for the active UI mode.
 *
 * page() uses zero-based display-page indices. Source-page and annotation indices
 * in choice snapshots identify widgets in the saved source document. Pointer
 * positions use item-local, top-left logical coordinates; choice rectangles are
 * normalized to the displayed page and must be scaled by the overlay's size.
 */
class PDFFormView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFFormController *controller READ controller WRITE setController
                   NOTIFY controllerChanged FINAL)
    Q_PROPERTY(PDFFormDesignerController *formDesignerController
                   READ formDesignerController
                   WRITE setFormDesignerController
                   NOTIFY formDesignerControllerChanged FINAL)
    Q_PROPERTY(bool formDesignMode READ formDesignMode WRITE setFormDesignMode
                   NOTIFY formDesignModeChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive
                   NOTIFY interactiveChanged FINAL)
    Q_PROPERTY(QVariantList choiceFields READ choiceFields
                   NOTIFY choiceFieldsChanged FINAL)

public:
    /**
     * @brief Creates an initially inactive input item for display page zero.
     * @param parent Optional Qt Quick parent item.
     *
     * Enables input-method support, starts with no accepted mouse buttons and an
     * arrow cursor, and configures a single-shot 40 ms choice-refresh timer.
     * No document is opened or inspected here. Attach a controller and enable
     * interactivity before expecting native form input or choice-field discovery.
     */
    explicit PDFFormView(QQuickItem *parent = nullptr);

    /**
     * @brief Returns the observed form-interaction controller, or nullptr.
     *
     * The pointer is not owned by the view and is cleared when its QObject is
     * destroyed. A non-null pointer does not prove its document is loaded or
     * interactive; those checks remain part of the controller's request handling.
     */
    PDFFormController *controller() const;
    /**
     * @brief Returns the optional controller used to identify designer-managed fields.
     *
     * The view observes this object without owning it. When formDesignMode() is
     * active, it uses the designer to filter duplicate choice overlays and to
     * protect managed source widgets from native pointer handling.
     */
    PDFFormDesignerController *formDesignerController() const;
    /**
     * @brief Returns whether this view applies form-designer interaction filtering.
     *
     * This is a local presentation/routing flag. It does not create fields, switch
     * a ribbon tab, or disable all native form interaction by itself.
     */
    bool formDesignMode() const;
    /**
     * @brief Returns this delegate's zero-based display-page index, initially zero.
     *
     * This is not a saved source-page identity. Resolve the current document's
     * source mapping before acting on a widget snapshot, especially after page
     * insertion/deletion or delegate reuse. The stored value is not range-validated.
     */
    int page() const;
    /**
     * @brief Returns whether the view is configured to accept native form input.
     *
     * This local flag controls mouse/hover routing and choice discovery. It is
     * independent of PDFFormController::interactive(), which applies document
     * permissions; true here does not override controller or field restrictions.
     */
    bool interactive() const;
    /**
     * @brief Returns the current QML-facing choice-widget snapshots by value.
     *
     * In design mode with a designer attached, filters out source choice fields
     * managed by that designer so its overlay retains responsibility. Otherwise
     * returns the cached inspection list. Filtering does not mutate the cache.
     * Both current and removed designer source fields participate in matching,
     * preventing an old native overlay from reappearing after a draft move/delete.
     * An empty list can indicate no supported fields, cleared/inactive state, or
     * inspection failure; this property has no separate loading/error status.
     *
     * Each QVariantMap contains:
     * - sourcePage and annotationIndex: zero-based saved-source widget identity.
     * - identityToken: fingerprint to return unchanged with focus/selection requests.
     * - comboBox: true for a combo box, false for a supported list box.
     * - x, y, width, height: normalized displayed-page rectangle, top-left origin.
     * - accessibleName: worker-supplied name for the overlay's accessible label.
     * - options: display strings in option order; selectedIndices: zero-based choices.
     * - readOnly and multiSelect: restrictions the overlay must honor.
     *
     * Rectangle orientation includes the PDF's intrinsic page rotation; an
     * additional QML view rotation is applied by the enclosing page surface.
     * These values are snapshots, not live PDF handles. Controller/worker identity
     * checks still apply when a user later interacts with the corresponding field.
     */
    QVariantList choiceFields() const;

    /**
     * @brief Replaces the observed native form controller and refreshes view state.
     * @param controller Non-owned controller, or nullptr to detach.
     *
     * An identical pointer is a no-op. Disconnects old subscriptions, clears
     * cached choices, then observes input state, relevant form-render changes,
     * controller interactivity, and document changes on the replacement. Schedules
     * choice refresh and updates IME/cursor presentation before controllerChanged().
     *
     * Controller destruction also clears pointer/drag state, invalidates choice
     * requests, and stops the refresh timer. This method does not assign the
     * controller's document or take ownership of its lifetime.
     */
    void setController(PDFFormController *controller);
    /**
     * @brief Replaces the optional designer used for source-widget arbitration.
     * @param formDesignerController Non-owned designer, or nullptr to detach.
     *
     * Disconnects the previous designer and observes relevant field changes and
     * destruction on the replacement. Resets designer-pointer suppression,
     * refreshes the cursor, and emits choiceFieldsChanged() because the filtered
     * getter can change even when the underlying snapshot cache does not.
     * Emits formDesignerControllerChanged(); identical assignments do nothing.
     */
    void setFormDesignerController(
        PDFFormDesignerController *formDesignerController);
    /**
     * @brief Enables or disables designer filtering for this page's native input.
     * @param formDesignMode Whether managed source widgets belong to designer UI.
     *
     * Resets designer hover/press-block state, refreshes the cursor, and notifies
     * choiceFieldsChanged() and formDesignModeChanged(). Does not rescan PDF fields
     * or change the controller's form values. Identical assignments are ignored.
     */
    void setFormDesignMode(bool formDesignMode);
    /**
     * @brief Retargets the item to a display page, including when a delegate is reused.
     * @param page Zero-based display-page index; stored as supplied without clamping.
     *
     * Clears designer-pointer suppression and old choices, invalidates outstanding
     * choice inspection through the refresh serial, and requests new choices when
     * eligible. Negative pages suppress choice discovery. Emits pageChanged() only
     * for a changed value; does not navigate PDFDocument or select a source widget.
     */
    void setPage(int page);
    /**
     * @brief Configures native event acceptance and refreshes dependent UI state.
     * @param interactive True accepts left-button/hover input; false disables both.
     *
     * Disabling clears pointer/drag/designer-block state and releases Qt focus
     * if active, allowing focus-out handling to ask the controller to commit/leave
     * its field. Both directions refresh choice discovery, IME state, and cursor,
     * then emit interactiveChanged(). Does not alter document permissions or
     * implicitly save form values. An unchanged flag is a no-op.
     */
    void setInteractive(bool interactive);
    /**
     * @brief Forwards an overlay option change after checking its source-page identity.
     * @param sourcePage Zero-based source page captured in the choice snapshot.
     * @param annotationIndex Zero-based source annotation index for that widget.
     * @param optionIndex Zero-based option index to select or toggle.
     * @param selected Desired multi-select state when toggle is false.
     * @param toggle Whether the worker should invert its current multi-select state.
     * @param identityToken Snapshot fingerprint, forwarded unchanged for revalidation.
     *
     * Requires an interactive view, controller/document, and a sourcePage matching
     * the document's current mapping of page(). Otherwise requests a refresh,
     * or clears choices when inspection is unavailable, and returns. The
     * controller/worker handles detailed eligibility,
     * index/token validation, and focus/selection changes asynchronously. Combo
     * boxes and single-select lists always select the option rather than deselect it.
     * After forwarding, the view immediately requests Qt mouse focus; that does
     * not prove the native selection was accepted. There is no completion signal
     * for this operation; subsequent controller state/inspection reflects results.
     */
    Q_INVOKABLE void selectChoiceOption(int sourcePage,
                                        int annotationIndex,
                                        int optionIndex,
                                        bool selected,
                                        bool toggle,
                                        const QString &identityToken);
    /**
     * @brief Asks the native form session to focus a choice widget before overlay input.
     * @param sourcePage Zero-based saved-source page from the inspected snapshot.
     * @param annotationIndex Zero-based source annotation index.
     * @param identityToken Exact snapshot fingerprint to revalidate at the worker.
     *
     * A failed local interactivity/controller/source-mapping check requests a
     * refresh (or clears unavailable choices) and emits
     * choiceFieldFocusFinished(..., false) immediately. Forwarded requests normally
     * complete asynchronously, but controller-side early rejection can also notify
     * synchronously. The callback retains weak view/controller pointers
     * and checks their association and current source mapping before notifying QML.
     * Stale/destroyed receivers can suppress notification entirely. The result
     * describes native widget focus, not selection of an option or changed values.
     * After forwarding, the view immediately requests Qt mouse focus. Controller
     * validation can still fail synchronously before that focus request. The view's
     * completion guard checks receiver/controller/source mapping, not a separate
     * focus-request serial or the current design-mode flag.
     */
    Q_INVOKABLE void focusChoiceField(int sourcePage,
                                      int annotationIndex,
                                      const QString &identityToken);

signals:
    /** @brief Notifies that the observed native form controller changed or was destroyed. */
    void controllerChanged();
    /** @brief Notifies that the optional designer controller changed or was destroyed. */
    void formDesignerControllerChanged();
    /** @brief Notifies a change to the view's designer filtering/routing mode. */
    void formDesignModeChanged();
    /** @brief Notifies that this delegate now represents a different display-page index. */
    void pageChanged();
    /** @brief Notifies a change to local mouse/hover and form-input interactivity. */
    void interactiveChanged();
    /**
     * @brief Requests reevaluation of the QML choiceFields property.
     *
     * Emitted when the cached snapshot list changes or clears, and when designer
     * filtering may change its visible subset. It does not imply a PDF form-value
     * mutation or guarantee that a fresh worker scan has just completed.
     */
    void choiceFieldsChanged();
    /**
     * @brief Reports a still-relevant overlay request to acquire native widget focus.
     * @param sourcePage Original zero-based source page used to identify the request.
     * @param annotationIndex Original zero-based source annotation index.
     * @param identityToken Original field fingerprint, allowing QML to match its snapshot.
     * @param focused Whether the controller reported focus on that requested widget.
     *
     * Local or controller-side early rejection emits false synchronously. Worker results are
     * delivered later only while the view/controller/source mapping still match;
     * consumers must tolerate requests disappearing on reset or delegate teardown.
     * Match the full identity before opening or updating an overlay control.
     */
    void choiceFieldFocusFinished(int sourcePage,
                                  int annotationIndex,
                                  const QString &identityToken,
                                  bool focused);

protected:
    /**
     * @brief Reserves form-editing shortcuts before normal Qt shortcut dispatch.
     * @param event Borrowed Qt event; ownership remains with the event system.
     * @return True for a shortcut override accepted here, otherwise the base
     *         QQuickItem::event() result.
     *
     * When this view is interactive and has active Qt focus, accepts
     * QEvent::ShortcutOverride for standard Copy, Cut, Paste, Select All, Undo,
     * Redo, Tab, and Backtab. This keeps those keys available for the native form
     * editor instead of allowing application-level shortcuts to consume them.
     * It does not execute the command or check controller/field eligibility at
     * this stage; keyPressEvent() and the controller perform subsequent routing.
     * Every other event follows the base item's handling.
     */
    bool event(QEvent *event) override;
    /**
     * @brief Refreshes input-method state after Qt grants this item focus.
     * @param event Borrowed focus-in event, forwarded to the base item first.
     *
     * Calls updateInputMethod() after QQuickItem::focusInEvent(). Acquiring Qt
     * focus alone does not focus a PDFium widget or change its value. The refresh
     * may release Qt focus again if no native focus/request or active mouse
     * gesture justifies retaining it.
     */
    void focusInEvent(QFocusEvent *event) override;
    /**
     * @brief Requests native field commit/focus release when this item loses Qt focus.
     * @param event Borrowed focus-out event.
     *
     * Calls controller->killFocus() when attached, clears the native mouse-down
     * flag, then delegates to the base item and updates input-method state.
     * The controller request is asynchronous and field validation can affect
     * its outcome; losing Qt focus is not proof that native focus was released
     * or that the field's value was accepted. Designer press-block state is not
     * reset by this handler itself.
     */
    void focusOutEvent(QFocusEvent *event) override;
    /**
     * @brief Initializes native hover tracking when the pointer enters the item.
     * @param event Borrowed hover-enter event in item-local logical coordinates.
     *
     * Ignores the event when inactive or detached. Otherwise records pointer
     * presence/position and checks whether the original source widget belongs to
     * designer interaction. A blocked hit sends an outside-page Move at (-1,-1)
     * to clear native hover and restores the appropriate cursor. An ordinary hit
     * forwards the actual Move and temporarily shows an arrow while the worker
     * resolves the native cursor. Accepts handled events without acquiring focus.
     */
    void hoverEnterEvent(QHoverEvent *event) override;
    /**
     * @brief Updates native hover or suppresses it over a designer-owned source widget.
     * @param event Borrowed hover-move event with item-local logical position.
     *
     * Ignores inactive/detached input; otherwise updates pointer presence and the
     * last position used by IME queries. Entering a designer-blocked region sends
     * one outside-page Move to clear native hover. Further moves within that
     * region avoid repeating that request. Leaving it resumes normal Move
     * dispatch. Handled events are accepted; this path does not focus a field.
     */
    void hoverMoveEvent(QHoverEvent *event) override;
    /**
     * @brief Clears page hover state when the pointer leaves the item.
     * @param event Borrowed hover-leave event, accepted even when input is inactive.
     *
     * Clears pointer-inside and designer-hover flags. If interactive and attached,
     * sends an outside-page Move at (-1,-1) with no modifiers or pressed-button
     * flag so PDFium can leave the hovered widget/link. Refreshes the cursor,
     * which falls back to an arrow outside the page; does not end a mouse drag
     * or explicitly release form focus.
     */
    void hoverLeaveEvent(QHoverEvent *event) override;
    /**
     * @brief Starts a left-button native form gesture or consumes a designer-blocked press.
     * @param event Borrowed mouse-press event with item-local logical position.
     *
     * Ignores input unless interactive, attached, and pressed with the left button.
     * Records pointer presence/position. A designer-owned source hit marks the
     * gesture blocked, clears native mouse-down state, refreshes the cursor, and
     * accepts the event without sending it to PDFium or requesting Qt focus.
     *
     * An ordinary press clears designer suppression, marks native mouse-down,
     * requests Qt mouse focus, and forwards Press with the left-button flag set.
     * Acceptance records local routing only; worker focus/value changes arrive
     * later through the controller.
     */
    void mousePressEvent(QMouseEvent *event) override;
    /**
     * @brief Forwards drag motion for an active native left-button gesture.
     * @param event Borrowed mouse-move event with item-local logical position.
     *
     * A gesture blocked at press time remains blocked: only the last pointer
     * position is updated and the event is accepted. Otherwise ignores input
     * unless interactive, attached, and m_mouseDown is true. Valid drag motion
     * updates the position and sends Move with the left-button flag. It does not
     * perform a new designer hit test midway through the native drag.
     */
    void mouseMoveEvent(QMouseEvent *event) override;
    /**
     * @brief Finishes a native drag or consumes the release of a designer-blocked press.
     * @param event Borrowed mouse-release event.
     *
     * A left-button release for a blocked gesture clears that block without a
     * native release; the blocked press already cleared native mouse-down state.
     * Otherwise requires an interactive,
     * attached view with an active native drag and a left-button release; other
     * events are ignored. Updates the final position, sends Release with no
     * left-button-down flag, clears m_mouseDown, and accepts the event. Focus is
     * left to the controller/worker result rather than explicitly killed here.
     */
    void mouseReleaseEvent(QMouseEvent *event) override;
    /**
     * @brief Routes a left-button double-click to PDFium when the source hit is eligible.
     * @param event Borrowed double-click event in item-local logical coordinates.
     *
     * Uses the same interactive/controller/left-button gate as a press. A
     * designer-owned source hit clears native mouse-down and press-block flags,
     * refreshes the cursor, and is accepted without native dispatch. Otherwise
     * marks the gesture down and sends DoubleClick with the left-button flag.
     * This handler does not independently force Qt focus; selection/focus effects
     * are determined by PDFium's handling of the double-click.
     */
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    /**
     * @brief Cleans up native press state when Qt removes the item's mouse grab.
     *
     * If a native gesture is down, a controller exists, and the gesture was not
     * designer-blocked, sends an outside-page Release at (-1,-1) with no modifiers
     * or pressed-button flag. This closes the native gesture even without a
     * normal mouse-release event. Clears press-block and mouse-down state before
     * calling the base implementation. Unlike ordinary mouse handlers, cleanup
     * does not require the local interactive flag to remain true.
     */
    void mouseUngrabEvent() override;
    /**
     * @brief Translates Qt shortcuts and ordinary key input into native form commands.
     * @param event Borrowed Qt key-press event.
     *
     * Ignores the event when inactive or detached. Standard Copy/Cut/Paste,
     * Select All, Undo, and Redo are routed to controller edit commands. Paste
     * reads the GUI clipboard's plain text, or supplies an empty string when no
     * clipboard is available. Field/copy permissions remain worker decisions.
     *
     * Other keys use virtualKey() and eventModifiers(), including auto-repeat.
     * Associated text is removed for Meta or Control-without-Alt shortcuts so
     * command keystrokes do not insert their text; Control+Alt text is retained.
     * Backtab is explicitly sent as PDFium Tab plus Shift and no text. The event
     * is accepted after dispatch even if the native field later ignores it or
     * the key maps to FWL_VKEY_Unknown. IME composition commits use the separate
     * inputMethodEvent() path.
     */
    void keyPressEvent(QKeyEvent *event) override;
    /**
     * @brief Inserts committed IME text into the active native form editor.
     * @param event Borrowed input-method event from the platform composition system.
     *
     * Requires an interactive view, an attached controller, and its cached
     * acceptsTextInput() state. Otherwise ignores the event. A non-empty
     * commitString is passed to replaceSelection(); an empty commit is accepted
     * without dispatching an edit. PDFium exposes no preedit bridge here, so the
     * platform retains composition presentation. This handler does not forward
     * preedit attributes or IME replacement-offset/length metadata separately.
     */
    void inputMethodEvent(QInputMethodEvent *event) override;
    /**
     * @brief Supplies the limited IME context available from this native-form bridge.
     * @param query Requested Qt input-method property.
     * @return The supported property value, or the base item's answer for other queries.
     *
     * ImEnabled combines local interactivity, controller presence, and native text
     * acceptance; it does not additionally test active Qt focus. ImHints returns
     * ImhNone. ImCursorRectangle is an approximate 1-by-18 logical-unit rectangle
     * at the last pointer position, not an exact PDFium caret bound. Surrounding
     * text is empty and cursor/anchor positions are zero because this bridge does
     * not expose native field text offsets. No worker request occurs during a query.
     */
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    /**
     * @brief Converts Qt modifier state to PDFium's FWL_EVENTFLAG_* bit mask.
     * @param modifiers Qt keyboard modifier flags from the input event.
     * @param leftButtonDown Whether to include the native left-button-down flag.
     * @param autoRepeat Whether to mark a repeating key event.
     * @return A possibly zero PDFium mask, not Qt::KeyboardModifiers.
     *
     * Maps Shift, Control, Alt, Meta, and Keypad independently, then adds the two
     * explicit button/repeat flags. Other Qt bits are ignored. This translation
     * is side-effect free and does not infer platform shortcuts or mouse state.
     */
    int eventModifiers(Qt::KeyboardModifiers modifiers,
                       bool leftButtonDown = false,
                       bool autoRepeat = false) const;
    /**
     * @brief Maps a supported Qt logical key to the corresponding PDFium virtual key.
     * @param qtKey Qt::Key value, rather than a platform-native scan code.
     * @return An FWL_VKEY_* value, or FWL_VKEY_Unknown for an unmapped key.
     *
     * Covers digits, A-Z, F1-F24, Backspace, Tab/Backtab, Return/Enter, Escape,
     * Space, page navigation, Home/End, arrows, Insert, and Delete. Backtab maps
     * to Tab here; keyPressEvent() supplies Shift separately. Character text is
     * forwarded independently, so an unknown virtual key does not by itself
     * discard the associated printable text.
     */
    static int virtualKey(int qtKey);
    /**
     * @brief Packages an item-local pointer event for the page's form controller.
     * @param type Worker pointer operation to dispatch.
     * @param position Logical point relative to this item, with a top-left origin;
     *                 (-1,-1) is used by callers to clear hover or finish outside.
     * @param modifiers Qt keyboard flags, converted by eventModifiers().
     * @param leftButtonDown Explicit button state accompanying this operation.
     *
     * Ignores requests without a controller or positive item dimensions. Sends
     * the current display page, position, and rounded width/height (each at least
     * one) in matching logical units. Does not multiply either coordinate by the
     * device-pixel ratio or apply an extra view rotation. The worker performs the
     * final PDF page-space conversion. Local interactivity/designer checks belong
     * to callers; cleanup callers can use this even after normal input is disabled.
     */
    void sendPointerEvent(PDFFormWorker::PointerEventType type,
                          const QPointF &position,
                          Qt::KeyboardModifiers modifiers,
                          bool leftButtonDown);
    /**
     * @brief Invalidates old choice results and schedules a coalesced field inspection.
     *
     * Advances the request serial immediately and stops the current refresh timer.
     * If inactive, detached, or assigned a negative page, clears cached choices
     * instead of dispatching. Otherwise restarts the 40 ms single-shot timer,
     * allowing rapid render/input changes to share one loadChoiceFields() call.
     * Existing choices remain visible until a result replaces them unless another
     * lifecycle path explicitly clears them. This method performs no PDF inspection.
     */
    void requestChoiceFields();
    /**
     * @brief Starts the delayed choice inspection and converts accepted snapshots for QML.
     *
     * Rechecks local interactivity/controller/page eligibility and clears choices
     * on failure. Captures the request serial, display page, and weak view/controller
     * references before asking the controller to inspect the page. The callback
     * ignores results after destruction, controller replacement, page reassignment,
     * serial advancement, or deactivation. The controller separately verifies source
     * identity and display-to-source mapping against its worker result.
     *
     * Converts eligible fields to the QVariantMap schema documented by choiceFields().
     * Replaces the cache and emits choiceFieldsChanged() only if the new list differs;
     * an accepted empty result clears previous entries. Designer filtering is
     * deferred to the getter, so changing design mode does not require a rescan.
     */
    void loadChoiceFields();
    /**
     * @brief Clears the raw choice snapshot cache and notifies observers if necessary.
     *
     * Does nothing when the cache is already empty. This helper does not stop the
     * refresh timer, advance the request serial, or independently cancel a worker
     * inspection. Callers responsible for changing request lifetime must perform
     * that invalidation separately.
     */
    void clearChoiceFields();
    /**
     * @brief Tests whether designer ownership should suppress native pointer handling.
     * @param position Point in this item's logical, top-left coordinate space.
     * @return True when the normalized point hits a managed source field's original
     *         source rectangle on this display page; otherwise false.
     *
     * Requires design mode, a designer controller, a nonnegative page, and positive
     * item dimensions. Divides the position by width/height and delegates the hit
     * test to sourceFieldContainsPoint(). The designer checks current and removed
     * source-backed fields of all supported types, including their original
     * rectangles after movement/deletion; it does not test moved draft geometry
     * here. Out-of-page normalized positions do not match. This helper itself
     * changes no focus, cursor, or event acceptance state.
     */
    bool designerBlocksPointer(const QPointF &position) const;
    /**
     * @brief Synchronizes Qt focus retention and the platform input-method queries.
     *
     * Releases active Qt focus only when interactive and attached but the
     * controller reports neither native focus nor a pending focus request, and
     * no native mouse gesture is down. Keeping focus during pending presses
     * prevents the asynchronous native focus result from arriving too late.
     *
     * When a QInputMethod exists, requests refresh of ImEnabled, ImCursorRectangle,
     * and ImHints, even if no focus was released. Does not open a field, inject
     * text, compute an exact caret rectangle, or explicitly show/hide a keyboard.
     */
    void updateInputMethod();
    /**
     * @brief Maps the accepted PDFium hover cursor to the Qt Quick item cursor.
     *
     * Starts with Arrow and uses controller state only while interactive, pointer
     * inside, and outside a designer-owned source hit. Maps VBEAM to IBeam,
     * HBEAM to SizeVer, HAND to PointingHand, NESW to SizeBDiag, and NWSE to
     * SizeFDiag. Unknown native cursor codes and all ineligible states use Arrow.
     * Only updates this item's cursor; it performs no hit test or worker dispatch.
     */
    void updateCursor();

    QPointer<PDFFormController> m_controller;
    QPointer<PDFFormDesignerController> m_formDesignerController;
    bool m_formDesignMode = false;
    int m_page = 0;
    bool m_interactive = false;
    bool m_pointerInside = false;
    bool m_pointerOverDesignerSource = false;
    bool m_designerPointerBlocked = false;
    bool m_mouseDown = false;
    QPointF m_lastPointerPosition;
    QVariantList m_choiceFields;
    quint64 m_choiceFieldsRequestSerial = 0;
    QTimer m_choiceFieldsRefreshTimer;
};
