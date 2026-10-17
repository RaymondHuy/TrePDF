#pragma once

#include "PDFDocument.h"
#include "PDFFormWorker.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <functional>

/**
 * @brief GUI-thread bridge between a document session and worker-owned PDF forms.
 *
 * Exposes cached form input/history state to QML and forwards input, inspection,
 * scripting, and snapshot requests to PDFFormWorker. The worker owns the PDFium
 * form document and serializes its operations; this object does not expose or
 * directly manipulate those PDFium handles. Form filling is distinct from the
 * page-content draft history and from PDFFormDesignerController's field design.
 *
 * Call this object's methods on its owning GUI thread. Normal worker results are
 * delivered through the application's event loop after releasing the PDFium
 * lock. Generation numbers and document keys prevent results for an old source
 * or reset session from updating current state. Callback clients must still
 * guard their own lifetimes: some callbacks report failure after this controller
 * has gone away, while queued work can also be dropped during session teardown.
 *
 * Unless a parameter explicitly says sourcePage, page arguments identify
 * zero-based pages in the document's current display order. Source-page and
 * annotation identities must be retained when acting on an inspected widget.
 */
class PDFFormController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged FINAL)
    Q_PROPERTY(bool interactive READ interactive NOTIFY interactiveChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool hasFocus READ hasFocus NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(bool acceptsTextInput READ acceptsTextInput
                   NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(int focusedPage READ focusedPage
                   NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(int focusedAnnotationIndex READ focusedAnnotationIndex
                   NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(int cursorType READ cursorType NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(bool javascriptEnabled READ javascriptEnabled
                   WRITE setJavaScriptEnabled
                   NOTIFY javascriptEnabledChanged FINAL)
    Q_PROPERTY(bool javascriptTransitionPending
                   READ javascriptTransitionPending
                   NOTIFY javascriptTransitionPendingChanged FINAL)

public:
    /**
     * @brief Receives (succeeded, temporaryPdfPath, diagnosticError).
     *
     * A successful snapshot belongs to the caller, which must eventually remove
     * the temporary file. A reported failure supplies an empty path; the
     * controller removes its failed/stale snapshot. References are valid only
     * during the callback, so copy values needed by later asynchronous work.
     * See exportSnapshot() for immediate failure and cancellation semantics.
     */
    using SnapshotCompletion
        = std::function<void(bool, const QString &, const QString &)>;
    /**
     * @brief Receives an owned vector of supported choice-widget snapshots.
     *
     * An empty vector can mean no eligible fields, an invalid request, a failed
     * inspection, or a stale result. This callback has no separate error flag.
     */
    using ChoiceFieldsCompletion
        = std::function<void(QVector<PDFFormWorker::ChoiceField>)>;
    /**
     * @brief Receives (succeeded, fields) from managed source-field discovery.
     *
     * A successful empty vector means no supported TrePDF fields were found;
     * false with an empty vector represents failure, cancellation, or a stale
     * request. The vector is passed by value and may be moved by the receiver.
     */
    using TrePDFTextFieldsCompletion = std::function<void(
        bool,
        QVector<PDFFormWorker::TrePDFTextField>)>;
    /**
     * @brief Reports whether the requested choice widget acquired PDFium focus.
     *
     * The result is checked against the requested document, display page, and
     * annotation index. It does not describe Qt Quick keyboard focus or imply
     * that a choice value changed. See focusChoiceField() for delivery timing.
     */
    using ChoiceFocusCompletion = std::function<void(bool)>;

    /**
     * @brief Creates an initially detached controller with a unique form session ID.
     * @param parent Optional QObject owner, normally the PDFDocumentSession.
     *
     * Construction does not open a PDF. Input is initially inactive, there are
     * no drafts or focused fields, and the cursor defaults to PDFium's arrow.
     * setDocument() establishes source tracking and worker-session state.
     */
    explicit PDFFormController(QObject *parent = nullptr);
    /**
     * @brief Invalidates callbacks and requests destruction of the worker session.
     *
     * PDFium cleanup is dispatched to the worker; it is not performed directly
     * by this destructor. The associated PDFDocument is not owned or deleted by
     * this controller. No snapshot is saved implicitly during destruction.
     */
    ~PDFFormController() override;

    /**
     * @brief Returns the currently observed document, or nullptr when detached.
     *
     * The pointer is non-owning and does not imply that the document is loaded
     * or permits form filling. QPointer clears it when the document is destroyed.
     */
    PDFDocument *document() const;
    /**
     * @brief Attaches to a document and rebuilds the controller's session state.
     * @param document Non-owned source document; nullptr detaches the controller.
     *
     * Disconnects the old document, observes source/load/current-page/structure
     * changes on the new one, and handles its destruction. Refreshing resets
     * pending input, runtime form edits, and cached history; callers must save
     * needed changes before replacing the document. Reassigning the same
     * pointer is a no-op. Emits documentChanged() after refreshing state.
     */
    void setDocument(PDFDocument *document);

    /**
     * @brief Returns whether the loaded document currently permits form filling.
     *
     * This is the controller's document/permission gate, not the active ribbon
     * tool or an individual widget's editability. Views add their own mode gates;
     * the worker validates the particular field when processing each operation.
     */
    bool interactive() const;
    /**
     * @brief Returns whether form changes exist or a potential mutation is pending.
     *
     * Includes unacknowledged input so Save/Close guards cannot overlook an edit
     * that is still queued. A pending operation can later prove to be a no-op;
     * this flag is therefore more conservative than the last worker dirty flag.
     */
    bool hasDrafts() const;
    /**
     * @brief Returns the last accepted worker report of PDFium widget focus.
     *
     * This differs from Qt item/window focus and is updated asynchronously.
     * Use focusRequestPending() when input may still be acquiring form focus.
     */
    bool hasFocus() const;
    /**
     * @brief Returns the cached worker indication that the focused form accepts text.
     *
     * The view uses this to expose input-method support. It does not grant
     * editability to every widget or represent an in-progress IME composition.
     */
    bool acceptsTextInput() const;
    /**
     * @brief Returns the focused widget's last reported zero-based display page.
     * @return A display-page index, or -1 when no focused page is reported.
     *
     * The value follows accepted worker input state and may lag a queued focus
     * request. It is not a persistent source-page identity across page edits.
     */
    int focusedPage() const;
    /**
     * @brief Returns the focused widget's source-page annotation index, or -1.
     *
     * Interpret this index together with focusedPage() and its source mapping.
     * Annotation indices alone are not stable identities across source reloads.
     */
    int focusedAnnotationIndex() const;
    /**
     * @brief Returns the last worker-reported availability of native form Undo.
     *
     * This is PDFium's focused-field history, not the document's page-content
     * history. A form can be dirty even when PDFium offers no undoable operation.
     */
    bool canUndo() const;
    /**
     * @brief Returns the last worker-reported availability of native form Redo.
     *
     * Describes the focused form editor only and is refreshed after worker
     * interaction; it does not describe PDFEditController's command stack.
     */
    bool canRedo() const;
    /**
     * @brief Returns whether dispatched pointer/choice requests may acquire focus.
     *
     * Tracks outstanding focus-affecting requests until their current callbacks
     * arrive or session state resets. This lets the view route input while
     * hasFocus() still reflects the preceding worker state.
     */
    bool focusRequestPending() const;
    /**
     * @brief Returns the last PDFium cursor identifier, initially FXCT_ARROW.
     *
     * The value is an FXCT_* form cursor code, not a Qt::CursorShape. The view
     * translates it to a Qt cursor when presenting the form interaction state.
     */
    int cursorType() const;
    /**
     * @brief Returns the published JavaScript-enabled state for this form session.
     *
     * During a transition this reflects the requested state; completion may
     * reconcile it with the worker's actual state. Check
     * javascriptTransitionPending() before treating it as settled readiness.
     */
    bool javascriptEnabled() const;
    /**
     * @brief Requests a worker-side form JavaScript state change.
     * @param enabled Whether form scripting should be enabled for this session.
     *
     * Enabling requires a valid document key and a build with scripting support;
     * unsupported enable requests emit javascriptEnableFailed(). An unchanged
     * state is a no-op. Disabling also cancels any pending external-link prompt.
     * Publishes the requested flag immediately, then queues the actual change.
     * Worker failure may restore the previous effective state and active page.
     */
    void setJavaScriptEnabled(bool enabled);
    /**
     * @brief Returns whether a JavaScript transition has not yet settled.
     *
     * Includes the deferred automatic enable scheduled after document loading,
     * not just commands already executing on the worker. Observers receive
     * javascriptTransitionPendingChanged() when this cached flag changes.
     */
    bool javascriptTransitionPending() const;
    /**
     * @brief Returns the form-render invalidation token for a display page.
     * @param page Zero-based display-page index used by a page renderer.
     *
     * Returns the page-specific revision when present, otherwise the last
     * all-pages revision. This is a cache token, not the PDFDocument revision,
     * a page-count check, or a saved-file version.
     */
    quint64 formRevisionForPage(int page) const;
    /**
     * @brief Returns zero-based display pages whose runtime form contents changed.
     *
     * The set is controller-owned and changes as worker results arrive or the
     * session resets. Copy it when taking a save request snapshot, and also
     * inspect allPagesChanged() because the set alone may not cover the changes.
     */
    const QSet<int> &changedPages() const;
    /**
     * @brief Returns whether form changes require treating every page as dirty.
     *
     * This accompanies changedPages() when a worker result cannot restrict the
     * content changes to a page set. It is separate from an all-pages render
     * invalidation, which may occur even when no form value has changed.
     */
    bool allPagesChanged() const;
    /**
     * @brief Builds the current worker identity without opening a PDF.
     * @return Session ID plus render-file path, render revision, and password.
     *
     * An unloaded/detached document contributes only the session ID, yielding
     * an invalid key for document work. The password is an in-memory credential;
     * do not log or expose this key through user-visible diagnostics.
     */
    PDFFormWorker::DocumentKey documentKey() const;

    /**
     * @brief Queues a pointer interaction against the page's current source mapping.
     * @param page Zero-based display page; inserted pages without a source are ignored.
     * @param type Move, press, release, or double-click operation understood by PDFium.
     * @param modifiers PDFium FWL_EVENTFLAG_* bits, already translated from Qt.
     * @param position Point in the page view's top-left device coordinate space.
     * @param deviceSize Positive page-view dimensions in the same units as position.
     *
     * The worker maps these coordinates to PDF page space. PDFFormView normally
     * supplies logical item coordinates and rounded item dimensions, rather than
     * multiplying only one of them by device-pixel ratio. An outside-page point
     * can clear hover state. Inactive/unloaded/unmapped requests are ignored.
     * Non-move events temporarily count as potential mutations; presses also
     * count as pending focus requests. Source links masked by setRemovedWebLinks()
     * are included in the worker request so their original actions stay suppressed.
     */
    void pointerEvent(int page,
                      PDFFormWorker::PointerEventType type,
                      int modifiers,
                      const QPointF &position,
                      const QSize &deviceSize);
    /**
     * @brief Queues a keyboard interaction for the worker's active form field.
     * @param virtualKey PDFium FWL_VKEY_* key code, not an untranslated Qt key.
     * @param modifiers PDFium FWL_EVENTFLAG_* modifier/repeat bits.
     * @param text Associated committed text; callers remove shortcut-only text.
     *
     * Ignored when interaction is disabled. Marks a potential mutation before
     * dispatch so draft guards cover the pending command, even if processing
     * ultimately only moves the caret or focus. This does not expose IME preedit.
     */
    void keyEvent(int virtualKey,
                  int modifiers,
                  const QString &text);
    /**
     * @brief Selects text in the active native form editor through the worker.
     *
     * Applies to the current field, not all page text or every form field.
     * Ignored when inactive; selection changes arrive through input state.
     */
    void selectAll();
    /**
     * @brief Requests Copy or Cut for the active form selection.
     * @param cut True removes the copied selection when the worker permits it;
     *            false requests Copy without deletion.
     *
     * Non-empty returned text is placed on the GUI clipboard after accepting
     * the result. The worker enforces applicable copy/field restrictions. Cut
     * counts as a pending mutation; Copy does not. Inactive calls are ignored.
     */
    void copySelection(bool cut);
    /**
     * @brief Queues replacement of the focused form selection with committed text.
     * @param text Text to pass to the worker's Paste command, including IME commits.
     *
     * This method does not read the system clipboard itself. Native field rules
     * determine whether the replacement is accepted; the controller tracks a
     * pending mutation until the result arrives. Inactive calls are ignored.
     */
    void replaceSelection(const QString &text);
    /**
     * @brief Queues Undo in PDFium's active form editor.
     *
     * Does not undo page-content or form-designer commands. The worker determines
     * whether an operation is available; dispatch is ignored when inactive and
     * otherwise participates in the pending-mutation guard.
     */
    void undo();
    /**
     * @brief Queues Redo in PDFium's active form editor.
     *
     * Uses the same asynchronous mutation tracking as undo(). It cannot restore
     * operations absent from PDFium's native field history.
     */
    void redo();
    /**
     * @brief Asks the worker to commit/leave the currently focused form widget.
     *
     * This is asynchronous and can invoke field validation/actions. The returned
     * interaction determines the resulting focus and value state; calling this
     * method alone does not prove that validation succeeded. Ignored when inactive.
     */
    void killFocus();
    /**
     * @brief Discards runtime form edits by resetting the worker's form session.
     *
     * Invalidates old callbacks and pending link actions, clears cached draft,
     * focus, cursor, and native history state, and invalidates all form rasters.
     * Enabled scripting is requeued for the rebuilt session. Does nothing when
     * there are no drafts, focus, or native Undo/Redo state to discard. The source
     * PDF and the independent page-content draft history are not written here.
     */
    void discardChanges();
    /**
     * @brief Asynchronously serializes current form values to a temporary PDF.
     * @param completion Receiver of success, owned temporary path, and diagnostics.
     *
     * Creates a unique temporary file, then queues export after preceding worker
     * commands. The worker commits the focused field and runs enabled will-save
     * actions before serialization; their resulting state is applied on delivery.
     * Failure to commit the field can prevent export. This is staging for Save,
     * not a write to the user's destination, and does not clear draft history.
     *
     * Path-creation failure calls completion immediately. Delivered worker results
     * run on the application thread; stale/destroyed-controller results remove
     * the temporary file and report failure. Session teardown can purge queued
     * work without a callback. Supply a receiver that takes ownership of a
     * successful file and guards its own lifetime; do not fire-and-forget export
     * with an empty completion, which cannot hand that successful file to a caller.
     */
    void exportSnapshot(const SnapshotCompletion &completion);
    /**
     * @brief Exports the same form snapshot while synchronously waiting for the worker.
     * @param[out] filePath Temporary PDF path on success; empty on reported failure.
     * @param[out] error Diagnostic on failure; its previous value is not cleared on success.
     * @return True only when the worker successfully writes the snapshot.
     *
     * Applies the worker's interaction state before returning and removes a
     * failed temporary file. The caller owns successful output and its cleanup.
     * This blocks the calling thread without a nested event loop: prefer the
     * asynchronous API for UI saves, and never wait while holding the PDFium
     * process lock or executing on the form worker itself.
     */
    bool exportSnapshotBlocking(QString &filePath, QString &error);
    /**
     * @brief Asynchronously inspects the supported choice widgets on one page.
     * @param page Zero-based display page, mapped to a saved source page at dispatch.
     * @param completion Receives field snapshots by value; an empty callback is ignored.
     *
     * Supplies safe fixed-choice combo/list fields for the Qt overlay, including
     * supported read-only fields identified as such. Editable combos, hidden or
     * action-bearing widgets, commit-on-selection behavior, and unsafe geometry
     * or option data remain outside this overlay path. Each snapshot carries its
     * source page, source annotation index, options, selected indices, and an
     * identity token to reuse when requesting focus or changing an option.
     * Geometry is normalized to the displayed page with a top-left origin.
     *
     * An invalid/unloaded/inactive/unmapped request completes immediately with
     * an empty vector. Delivered worker results also produce an empty vector on
     * failure or changed generation, document key, or display-to-source mapping.
     * Empty results therefore do not distinguish failure from no eligible fields.
     * Worker completion is queued to the application thread; session teardown
     * can discard pending work, so receivers must not rely on unconditional delivery.
     */
    void requestChoiceFields(int page,
                             const ChoiceFieldsCompletion &completion);
    /**
     * @brief Discovers supported TrePDF-managed widgets across the source document.
     * @param completion Receives success plus owned field snapshots; empty means no request.
     *
     * Despite the historical name, discovery includes managed text/comb fields,
     * check boxes, radio buttons, dropdowns, and list boxes. Records retain source
     * page/annotation identities, live values, and metadata used by the designer.
     * This does not require interactive() and does not create designer drafts.
     *
     * Initial dispatch is deferred to the controller's next event turn so source
     * state can settle. A newer call supersedes older requests. The worker scans
     * source pages incrementally, allowing input/render work between scan steps.
     * Valid ownership markers and supported geometry/data are required; document
     * inspection failure or duplicate managed IDs rejects the complete result.
     *
     * Success may return an empty vector. Failed, superseded, or stale delivered
     * results return (false, {}), never a partial inventory. The initial deferred
     * call can be removed if this object is destroyed before it runs; callbacks
     * must guard their receiver and not assume delivery during application teardown.
     */
    void requestTrePDFTextFields(
        const TrePDFTextFieldsCompletion &completion);
    /**
     * @brief Queues focus and an option-selection change for a supported choice field.
     * @param page Zero-based display page currently showing the source widget.
     * @param annotationIndex Zero-based index in that source page's annotation array.
     * @param optionIndex Zero-based option index in the inspected choice field.
     * @param selected Desired state for a multi-select list when toggle is false.
     * @param expectedSourcePage Expected source-page identity; a negative value omits
     *                           this controller-side mapping comparison.
     * @param toggle For multi-select lists, invert the worker's current option state
     *               instead of relying on a potentially stale selected snapshot.
     * @param expectedIdentityToken Snapshot fingerprint from requestChoiceFields();
     *                              empty omits comparison with a caller-supplied token.
     *
     * Combo boxes and single-select lists always select the requested option;
     * selected/toggle do not deselect their sole choice. Invalid indices, stale
     * expected page mappings, unloaded documents, and inactive sessions are ignored.
     * The worker rechecks that the field is visible, writable, supported, and
     * action-free, and that its identity remains valid across focus changes.
     * Omitting the expected token does not disable that worker-side validation.
     *
     * Dispatch immediately tracks both a potential mutation and pending focus.
     * Accepted results update native focus/history, dirty state, and rendering;
     * no synchronous success value or independent designer Undo entry is produced.
     */
    void selectChoiceOption(int page,
                            int annotationIndex,
                            int optionIndex,
                            bool selected,
                            int expectedSourcePage = -1,
                            bool toggle = false,
                            const QString &expectedIdentityToken = {});
    /**
     * @brief Requests PDFium focus on a supported choice widget without selecting an option.
     * @param page Zero-based display page.
     * @param annotationIndex Zero-based annotation index within its source page.
     * @param expectedSourcePage Expected source-page index, or a negative value to
     *                           skip the explicit controller-side mapping comparison.
     * @param expectedIdentityToken Optional inspected identity token, revalidated by
     *                              the worker when supplied.
     * @param completion Optional result receiver; true means the returned focus
     *                   matches this document, display page, and annotation.
     *
     * Uses the same eligibility/mapping guards as choice selection. Leaving a
     * previous field can commit its value, so this request counts as both pending
     * focus and a potential mutation even though it does not select an option.
     *
     * Early rejection invokes completion(false) synchronously. A delivered worker
     * result is applied before invoking completion on the application thread.
     * A stale delivered result reports false; work purged by a session reset may
     * have no callback. The receiver must protect its own lifetime and should
     * not interpret true as a form-value change or a Qt Quick focus guarantee.
     */
    void focusChoiceField(int page,
                          int annotationIndex,
                          int expectedSourcePage = -1,
                          const QString &expectedIdentityToken = {},
                          const ChoiceFocusCompletion &completion = {});
    /**
     * @brief Consumes a current confirmation request and asks the OS to open its URL.
     * @param requestId Nonzero token previously emitted by externalLinkRequested().
     * @return QDesktopServices::openUrl()'s result, or false for an invalid/stale token.
     *
     * Rechecks the captured generation and full document key. A stale matching
     * request is cancelled with notification; zero or unrelated IDs are ignored.
     * A valid request is cleared before launching, without a cancellation signal,
     * and stays consumed even if launching fails. The return value confirms only
     * the OS launch request, not successful loading of a remote page. Must run
     * outside the PDFium process lock.
     */
    Q_INVOKABLE bool confirmExternalLink(quint64 requestId);
    /**
     * @brief Routes an HTTP/HTTPS URL through the existing confirmation flow.
     * @param url URL text to validate and present; this method performs no network access.
     * @return The newly created pending request ID, or zero if rejected or consumed
     *         by a synchronous signal handler.
     *
     * Requires a loaded document, a permitted URL, and no existing pending prompt;
     * form-filling interactivity is not required. Emits externalLinkRequested()
     * synchronously when accepted, with the current source identity captured for
     * later confirmation. Signal handlers may consume/cancel the request before
     * this function returns. The browser opens only through confirmExternalLink().
     */
    quint64 requestExternalWebLink(const QString &url);
    /**
     * @brief Replaces the set of source links suppressed by unsaved link edits.
     * @param removedWebLinks Map from zero-based source-page indices to source
     *                        annotation indices whose original actions are hidden.
     *
     * The mask affects ordinary link hit testing, hover cursor, and external or
     * internal navigation actions. It does not remove annotations from the PDF
     * or modify form values. An identical map is a no-op. Changing it cancels a
     * pending external-link prompt and, when needed, changes the cached hand
     * cursor back to arrow and emits inputStateChanged().
     */
    void setRemovedWebLinks(const QHash<int, QSet<int>> &removedWebLinks);
    /**
     * @brief Cancels the matching external-link confirmation without opening it.
     * @param requestId Nonzero ID of the current pending request.
     *
     * Zero, old, or unrelated IDs are ignored so a stale dialog cannot cancel a
     * newer prompt. A match clears the captured URL/key and emits
     * externalLinkRequestCancelled() for that request.
     */
    Q_INVOKABLE void cancelExternalLink(quint64 requestId);

signals:
    /**
     * @brief Notifies that the observed document pointer changed or was destroyed.
     *
     * Session state has already been refreshed when emitted. This signal does
     * not stand for every source-content change on an unchanged document object.
     */
    void documentChanged();
    /**
     * @brief Notifies a change to the loaded-document/form-permission interaction gate.
     *
     * Read interactive() to decide whether views may send form input. Individual
     * widget restrictions and the view's current editing mode still apply.
     */
    void interactiveChanged();
    /**
     * @brief Requests refresh of hasDrafts(), canUndo(), and canRedo() bindings.
     *
     * Covers both accepted worker history changes and changes in aggregate
     * pending-mutation state. It is not proof that a PDF value was actually edited.
     */
    void historyStateChanged();
    /**
     * @brief Requests refresh of focus, text-input, widget identity, and cursor state.
     *
     * May also be emitted for a page-associated interaction whose cached values
     * are unchanged, allowing views to refresh their input-method presentation.
     */
    void inputStateChanged();
    /**
     * @brief Invalidates cached form rendering for one display page or all pages.
     * @param page Zero-based display-page index, or a negative value for every page.
     * @param revision Newly allocated controller-local form-render token.
     *
     * Renderers use the token alongside document revision and geometry to reject
     * stale images. This signal also occurs on resets/focus changes and does not
     * by itself mean that saved PDF content or form values changed.
     */
    void formRenderStateChanged(int page, quint64 revision);
    /**
     * @brief Notifies a change in the published requested/reconciled scripting state.
     *
     * Read javascriptEnabled() together with javascriptTransitionPending(); this
     * notification alone does not mean the worker has finished switching modes.
     */
    void javascriptEnabledChanged();
    /** @brief Notifies that a deferred/in-flight scripting transition started or settled. */
    void javascriptTransitionPendingChanged();
    /**
     * @brief Reports unsupported scripting enable or a failed/mismatched worker transition.
     *
     * Despite its name, this can also report a failed disable request. Inspect
     * the reconciled JavaScript state rather than assuming it is now disabled.
     */
    void javascriptEnableFailed();
    /**
     * @brief Asks the UI to restore a display page after a rejected form transition.
     * @param page Valid zero-based display-page index accepted by the worker.
     *
     * Emitted under the rollback guard before a fallback model update. The UI
     * can discard queued navigation and align its viewport without re-triggering
     * the rejected transition; this is not an Undo of document page structure.
     */
    void pageRollbackRequested(int page);
    /**
     * @brief Requests navigation to a valid internal link/form-action destination.
     * @param page Zero-based display index resolved from the saved source destination.
     *
     * Deleted/unmapped destinations are ignored. Emitted before a fallback
     * PDFDocument current-page update; destination zoom/position is not applied here.
     */
    void pageNavigationRequested(int page);
    /**
     * @brief Presents a validated external URL for explicit user confirmation.
     * @param requestId Nonzero token to pass to confirmExternalLink() or cancelExternalLink().
     * @param authority Fully encoded URL authority, suitable for identifying the host.
     * @param displayUrl Complete fully encoded URL for display in the confirmation UI.
     * @param insecureHttp True for HTTP; false for HTTPS.
     *
     * Only one request is pending at a time. The controller captures its document
     * key/generation and never launches the URL merely by emitting this signal.
     * Emission occurs outside the PDFium process lock and can invoke reentrant
     * GUI handlers, so receivers must retain and use the exact request token.
     */
    void externalLinkRequested(quint64 requestId,
                               const QString &authority,
                               const QString &displayUrl,
                               bool insecureHttp);
    /**
     * @brief Notifies the UI that a particular pending confirmation is no longer valid.
     * @param requestId ID cleared by cancellation, source invalidation, or link-mask changes.
     *
     * Receivers should dismiss only a dialog carrying this ID. Normal consumption
     * by confirmExternalLink() intentionally does not emit a cancellation signal.
     */
    void externalLinkRequestCancelled(quint64 requestId);

private:
    /**
     * @brief Reinitializes form state after document assignment, reload, or destruction.
     *
     * Advances callback/navigation/request identities, cancels external prompts,
     * clears link suppression, and resets the worker session. Recomputes the
     * document permission gate, clears dirty/focus/history/pending input state,
     * and invalidates every form render. Property notifications are emitted when
     * the corresponding cached state changes.
     *
     * For a valid source in a scripting-capable build, schedules automatic
     * JavaScript enable on the next event turn, guarded by generation/key. This
     * lets sourceChanged/loadedChanged settle before document-open actions run.
     */
    void refreshDocumentState();
    /**
     * @brief Reconciles view-side state after display-page structure changes.
     * @param currentPageMappingChanged Whether the current display page now resolves
     *                                  to a different source-page occurrence.
     *
     * Invalidates navigation actions/prompts and all form renders while retaining
     * the worker session and existing form edits. Only a changed current-page
     * mapping triggers submitCurrentPageChange(); this is not a full source reset.
     */
    void handlePageStructureChange(bool currentPageMappingChanged);
    /**
     * @brief Dispatches an active-field editing command with common callback guards.
     * @param command Native form operation to enqueue.
     * @param text Replacement payload used by Paste; normally empty for other commands.
     * @param copyResultToClipboard Whether non-empty returned selection text should
     *                              be copied to the GUI clipboard after acceptance.
     *
     * Ignores inactive sessions and captures the current document key/generation.
     * Cut, Paste, Undo, and Redo increment the potential-mutation count before
     * dispatch. Other commands still apply their returned focus/history/render
     * state but do not increment that count here.
     */
    void submitEditCommand(PDFFormWorker::EditCommand command,
                           const QString &text = {},
                           bool copyResultToClipboard = false);
    /**
     * @brief Creates the guarded GUI-side receiver for an asynchronous interaction.
     * @param key Exact worker document identity captured when dispatching.
     * @param generation Controller generation captured with that identity.
     * @param copyResultToClipboard Whether to apply non-empty selectedText to the clipboard.
     * @param pendingMutation Whether this completion balances a previously counted mutation.
     * @param pendingFocusRequest Whether it balances a previously counted focus request.
     * @return Callback safe to retain after this controller's destruction via QPointer.
     *
     * A stale controller/key/generation result is ignored entirely. A current
     * result decrements matching pending counters, merges interaction state, and
     * processes host actions only if its captured navigation epoch is still current.
     * Thus ordinary form state can remain relevant even when navigation effects
     * became obsolete. Notifies history bindings if aggregate hasDrafts() changes
     * as a pending operation settles. Reset paths clear counters for discarded work.
     */
    PDFFormWorker::InteractionCallback interactionCallback(
        const PDFFormWorker::DocumentKey &key,
        quint64 generation,
        bool copyResultToClipboard = false,
        bool pendingMutation = false,
        bool pendingFocusRequest = false);
    /**
     * @brief Counts a dispatched operation that might mutate form state.
     *
     * Makes hasDrafts() true before asynchronous processing can finish and emits
     * historyStateChanged() only when that aggregate flag changes. The matching
     * interaction callback must be marked pendingMutation to decrement the count;
     * source/session reset paths clear all remaining counts together.
     */
    void beginPendingMutation();
    /**
     * @brief Queues and reconciles the worker's form JavaScript transition.
     * @param enabled Requested worker scripting state.
     *
     * Captures current display/source pages, document key, generation, and request
     * serials; supersedes older transition results and page-rollback decisions.
     * A valid request marks the transition pending. Its latest valid completion
     * clears pending state and applies returned form interaction before comparing
     * requested and actual modes.
     *
     * Failed/mismatched transitions reconcile the exposed flag, may restore the
     * worker's active display page if no newer page request exists, and emit
     * javascriptEnableFailed(). Invalid keys do not dispatch; an enabled flag is
     * cleared in that case. This path does not process returned host navigation
     * or external-link actions through processHostActions().
     */
    void queueJavaScriptState(bool enabled);
    /**
     * @brief Synchronizes the document's current display/source page with form scripting.
     *
     * Consumes a rollback-generated current-page notification without redispatch.
     * Ordinary calls advance the navigation epoch, invalidating earlier host
     * actions even when scripting is off. A loaded scripting-enabled document
     * queues the page transition with its current source mapping.
     *
     * Valid worker interaction state is applied even if a newer page request has
     * superseded this one; only the newest request may roll back navigation on
     * failure. Results for a replaced document/session are discarded entirely.
     */
    void submitCurrentPageChange();
    /**
     * @brief Restores a valid display page after worker-side transition rejection.
     * @param page Zero-based display page; negative/out-of-range values are ignored.
     *
     * Emits pageRollbackRequested() while suppressing the next resulting page
     * callback, allowing the UI to cancel deferred navigation. If the UI has not
     * already restored the page, updates PDFDocument directly, then clears the
     * guard. This avoids recursively resubmitting the rejected transition.
     */
    void rollbackCurrentPage(int page);
    /**
     * @brief Allocates a form-render revision and notifies affected page renderers.
     * @param page Zero-based display page, or any negative value to invalidate all pages.
     *
     * A single-page invalidation updates that page's token. An all-pages
     * invalidation replaces the global fallback token and clears page overrides.
     * Always emits formRenderStateChanged(page, revision); it does not itself
     * mark form values dirty or change the source PDF's render revision.
     */
    void invalidateFormPages(int page);
    /**
     * @brief Handles current, permitted host actions reported by a worker interaction.
     * @param result Interaction payload, including optional external URI/internal GoTo.
     * @param navigationEpoch Epoch captured with the original input request.
     *
     * Ignores obsolete epochs, unloaded documents, and actions from masked source
     * links. A valid HTTP/HTTPS action creates one confirmation request with its
     * generation/key captured; it never opens a browser here. After emitting the
     * prompt, rechecks state to account for reentrant GUI handlers.
     *
     * Internal GoTo source-page destinations are mapped back to current display
     * indices. Valid destinations emit pageNavigationRequested(), followed by a
     * model update if needed. Deleted destinations and GoTo zoom/position details
     * are not applied. Host-facing signals/actions run outside the PDFium lock.
     */
    void processHostActions(
        const PDFFormWorker::InteractionResult &result,
        quint64 navigationEpoch);
    /**
     * @brief Clears the pending external request and its captured source credentials.
     * @param notify Whether to emit externalLinkRequestCancelled() for a nonzero old ID.
     *
     * Resets ID, generation, document key, and URL before notification, so a
     * reentrant signal handler cannot confirm the request being cancelled.
     * Passing false is used when consuming a confirmed request normally.
     */
    void clearPendingExternalLink(bool notify);
    /**
     * @brief Merges an already-validated worker result into GUI-observable state.
     * @param result Current interaction state; page/focusedPage use display indices.
     * @param copyResultToClipboard Copy selectedText only if non-empty and a GUI
     *                              clipboard exists; otherwise preserve its contents.
     *
     * Accumulates dirty state and changed display pages until reset/discard. A
     * changed result with no specific page marks all pages changed. Native
     * Undo/Redo, focus, text-input capability, widget identity, and cursor are
     * refreshed from the result; masked ordinary links cannot retain a hand cursor.
     *
     * Value changes invalidate all form pages because calculations can affect
     * other fields. Otherwise explicit invalidation or a focus-state change
     * invalidates the reported page. Emits history/input notifications as needed;
     * a page-associated result requests an input refresh even if values match.
     * This method does not validate request freshness or process host actions;
     * callers must perform the required generation/key checks first.
     */
    void applyInteractionResult(
        const PDFFormWorker::InteractionResult &result,
        bool copyResultToClipboard = false);
    /**
     * @brief Parses an external URL against the controller's HTTP/HTTPS allowlist.
     * @param encodedUrl Encoded URL bytes supplied by the worker or a link request.
     * @return Strictly parsed absolute URL with a host, or an empty QUrl on rejection.
     *
     * Rejects empty input, raw ASCII controls/space/DEL, invalid or relative URLs,
     * non-HTTP(S) schemes, user information in the authority, and input or encoded
     * output exceeding 8,192 bytes. Scheme matching is case-insensitive. This is
     * syntactic/scheme validation only: it does not contact the host, establish
     * trust, check reachability, or replace the confirmation step.
     */
    static QUrl validatedExternalUrl(const QByteArray &encodedUrl);
    /**
     * @brief Compares every field used to identify a particular worker document.
     * @param left First captured/current identity.
     * @param right Second identity to compare.
     * @return True when session ID, file path, revision, and password all match.
     *
     * Uses exact stored values, not path canonicalization or isValid(). Two
     * equally invalid keys can compare equal; callers needing a usable source
     * must check validity separately. Password comparison stays in memory.
     */
    static bool sameDocumentKey(const PDFFormWorker::DocumentKey &left,
                                const PDFFormWorker::DocumentKey &right);
    /**
     * @brief Reserves a unique temporary PDF path for a forthcoming form snapshot.
     * @param[out] error Receives a diagnostic on failure; is not cleared on success.
     * @return Closed temporary-file path, or an empty string if no valid source or
     *         the temporary file cannot be created.
     *
     * Uses QTemporaryFile in the system temporary directory with auto-removal
     * disabled. The returned file exists but does not yet contain a serialized
     * PDF. The export path must remove failed/stale output or transfer successful
     * file cleanup to its caller; destruction of the local temporary-file object
     * deliberately does not delete it.
     */
    QString createSnapshotPath(QString &error) const;

    QPointer<PDFDocument> m_document;
    const quint64 m_sessionId;
    quint64 m_generation = 0;
    quint64 m_formRevisionSerial = 0;
    quint64 m_allPagesFormRevision = 0;
    QHash<int, quint64> m_pageFormRevisions;
    bool m_interactive = false;
    bool m_hasDrafts = false;
    bool m_hasFocus = false;
    bool m_acceptsTextInput = false;
    int m_focusedPage = -1;
    int m_focusedAnnotationIndex = -1;
    bool m_canUndo = false;
    bool m_canRedo = false;
    int m_pendingFocusRequestCount = 0;
    int m_cursorType = FXCT_ARROW;
    int m_pendingMutationCount = 0;
    QSet<int> m_changedPages;
    bool m_allPagesChanged = false;
    quint64 m_javascriptRequestSerial = 0;
    quint64 m_pageChangeRequestSerial = 0;
    quint64 m_trePDFTextFieldsRequestSerial = 0;
    quint64 m_navigationEpoch = 0;
    quint64 m_nextExternalLinkRequestId = 0;
    quint64 m_pendingExternalLinkRequestId = 0;
    quint64 m_pendingExternalLinkGeneration = 0;
    PDFFormWorker::DocumentKey m_pendingExternalLinkKey;
    QUrl m_pendingExternalLinkUrl;
    QHash<int, QSet<int>> m_removedWebLinks;
    bool m_ignoreNextCurrentPageChange = false;
    bool m_javascriptEnabled = false;
    bool m_javascriptTransitionPending = false;
};
