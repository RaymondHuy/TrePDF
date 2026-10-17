#pragma once

#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFRasterBudget.h"
#include "PDFRasterPlan.h"

#include <atomic>
#include <memory>
#include <functional>

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QSGNode;

/**
 * @brief Asynchronous PDF page raster presenter, exposed to QML as PDFPageView.
 *
 * Uses document/controller snapshots to request PDFium rendering on the form
 * worker, then uploads accepted images into Qt Quick scene-graph textures.
 * Small pages use a full-page raster; large pages use a bounded preview plus
 * visible/preload tiles selected by PDFRasterPlan. This item presents pixels;
 * form input, text selection, and draft editing controls live in other overlays.
 *
 * Property changes, request planning, and worker-result acceptance normally run
 * on the GUI thread. updatePaintNode() runs during scene-graph synchronization
 * and owns node/texture changes there. Worker work captures immutable request
 * data and never renders from the GUI document's PDFium handle. Request serials,
 * object identities, revisions, raster dimensions, and device-pixel ratio keep
 * obsolete work from replacing the current page after scrolling or delegate reuse.
 *
 * Document/controller pointers are observed, not owned. Raster reservations are
 * shared with scene-graph nodes, so memory remains charged until the last image
 * or texture reference is gone. Uploaded QImages are released instead of being
 * retained as a second full CPU cache. Texture destruction belongs to the scene
 * graph, not to GUI-side invalidation or arbitrary worker callbacks.
 *
 * Public page indices refer to the document's zero-based display order. Visible
 * rectangles use item-local logical units; raster jobs use physical pixel grids.
 * Zoom changes implicit layout size, while actual item size and window DPR drive
 * raster resolution. An enclosing page surface applies additional view rotation.
 */
class PDFPageItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PDFPageView)
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(PDFEditController *editController READ editController WRITE setEditController NOTIFY editControllerChanged FINAL)
    Q_PROPERTY(PDFFormController *formController READ formController WRITE setFormController NOTIFY formControllerChanged FINAL)
    Q_PROPERTY(PDFFormDesignerController *formDesignerController
                   READ formDesignerController
                   WRITE setFormDesignerController
                   NOTIFY formDesignerControllerChanged FINAL)
    Q_PROPERTY(bool formDesignMode READ formDesignMode WRITE setFormDesignMode
                   NOTIFY formDesignModeChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(QRectF visiblePageRect READ visiblePageRect WRITE setVisiblePageRect
                   NOTIFY visiblePageRectChanged FINAL)
    Q_PROPERTY(QColor pageColor READ pageColor WRITE setPageColor NOTIFY pageColorChanged FINAL)
    Q_PROPERTY(bool draftTextureCurrent READ draftTextureCurrent NOTIFY draftTextureCurrentChanged FINAL)
    Q_PROPERTY(bool draftTextureReady READ draftTextureReady NOTIFY draftTextureReadyChanged FINAL)
    Q_PROPERTY(QStringList displayedSuppressedCommentObjectIds
                   READ displayedSuppressedCommentObjectIds
                   NOTIFY displayedSuppressedCommentObjectIdsChanged FINAL)

public:
    /**
     * @brief Creates an initially detached page presenter and registers it for raster budgeting.
     * @param parent Optional Qt Quick parent item.
     *
     * Marks the item as having scene-graph content, disables item antialiasing,
     * and configures single-shot draft and zoom settling timers. Draft refreshes
     * use a 250 ms debounce unless requested immediately. Installs the shared
     * budget-release callback once so freed reservations can trigger GUI-thread
     * retries across live page items. Construction does not open or rasterize a PDF.
     */
    explicit PDFPageItem(QQuickItem *parent = nullptr);
    /**
     * @brief Unregisters this presenter and cancels its outstanding raster work.
     *
     * Removes the item from the global GUI-side budget registry before cancelling
     * queued work and signalling cancellation to active work. Does not delete the
     * observed document/controllers or directly destroy scene-graph textures;
     * Qt owns the node lifecycle and reservations follow their remaining owners.
     */
    ~PDFPageItem() override;

    /**
     * @brief Returns the non-owned document supplying page metrics and source identity.
     * @return Observed document, or nullptr when detached/destroyed.
     *
     * A non-null value does not imply a loaded source or a valid page index.
     * Rendering requires a loaded source; invalid display-page mappings produce
     * no raster and may be rejected by the worker after dispatch.
     */
    PDFDocument *document() const;
    /**
     * @brief Returns the optional non-owned controller supplying page-content draft state.
     *
     * Draft-aware rendering requires this controller to refer to the same document
     * as the item. Without a matching controller, rendering uses saved content
     * rather than applying another session's draft suppression or transforms.
     */
    PDFEditController *editController() const;
    /**
     * @brief Returns the optional non-owned controller for live PDFium form state.
     *
     * A matching controller provides the form session key and per-page form
     * revision. Attaching it enables rendering current native form values; this
     * item itself does not route keyboard or pointer events to form widgets.
     */
    PDFFormController *formController() const;
    /**
     * @brief Returns the optional designer used to identify source widget appearances to hide.
     *
     * The pointer is observed via QPointer. It supplies managed source-field IDs
     * for suppression in design mode so separate designer overlays can represent
     * field drafts without leaving duplicate original widget appearances behind.
     */
    PDFFormDesignerController *formDesignerController() const;
    /**
     * @brief Returns whether designer-managed source widgets should be suppressed.
     *
     * This local rendering mode does not create fields, enable native form input,
     * or modify a saved PDF. Actual suppression also requires an attached designer
     * and eligible field IDs for the current page.
     */
    bool formDesignMode() const;
    /**
     * @brief Returns the zero-based display page represented by this item, initially zero.
     *
     * The value is stored as supplied and is not a permanent source-page identity.
     * Rendering resolves the current source mapping, including inserted blank
     * pages, and separately rejects a page outside the document's current range.
     */
    int page() const;
    /**
     * @brief Returns the layout zoom multiplier, normally in the range 0.10 through 5.0.
     *
     * Multiplies document page metrics to produce implicit item size. It is not
     * an independent content-cache key; explicit item geometry and window DPR
     * determine requested raster dimensions.
     */
    qreal zoom() const;
    /**
     * @brief Returns the logical page region supplied by the enclosing viewport.
     *
     * Before any explicit assignment, returns boundingRect(), treating the whole
     * page as visible. After assignment, returns the stored rectangle unchanged,
     * including empty or partly out-of-bounds rectangles. Planning clips and
     * normalizes it through normalizedVisibleRect(); this getter does not do so.
     */
    QRectF visiblePageRect() const;
    /**
     * @brief Returns the background color used to initialize page rasters, initially white.
     *
     * This is part of the rendered page appearance, not merely an outer QML
     * background rectangle. Changing it invalidates the raster content cache.
     */
    QColor pageColor() const;
    /**
     * @brief Returns whether scene-graph synchronization acknowledged current page content.
     *
     * Becomes true after successful texture synchronization is acknowledged back
     * on the GUI thread and its captured content identities/revisions still match.
     * It does not mean that every high-resolution detail tile is resident or that
     * the raster has the latest zoom dimensions. Geometry-only zoom changes may
     * keep this true while scaling the existing texture. Content invalidation
     * clears it even when an older texture remains visible as temporary coverage.
     */
    bool draftTextureCurrent() const;
    /**
     * @brief Returns whether current texture content is safe for normal draft-overlay handoff.
     *
     * Requires the current-generation scene-graph acknowledgment and successful
     * requested preview changes: source-object hiding/moves, annotation removal,
     * and designer-widget suppression, together with the controller's indication
     * that all relevant draft objects can be suppressed. A current fallback raster
     * can therefore have draftTextureCurrent() == true and this property false.
     * This is not a signal that all tiles finished or a frame reached the display.
     */
    bool draftTextureReady() const;
    /**
     * @brief Returns the comment/link identities associated with accepted base-raster suppression.
     *
     * Published when the GUI accepts a new base raster, before texture upload,
     * so matching QML overlays and page pixels can enter the next scene-graph
     * frame together. Describes that accepted snapshot rather than whatever
     * suppression the edit controller currently requests. May include removed
     * web-link IDs encoded as "annotation:<index>".
     *
     * Retained through draft/zoom invalidation until replacement, and cleared by
     * full cache invalidation. It must not be treated as proof that a GPU upload
     * already completed. The returned list is a value copy, not mutable cache access.
     */
    QStringList displayedSuppressedCommentObjectIds() const;

    /**
     * @brief Rebinds the page presenter to a non-owned document.
     * @param document New source document, or nullptr to detach a pooled/unused item.
     *
     * Disconnects the previous document and observes load state, page count, and
     * destruction on the replacement. Refreshes implicit metrics and fully
     * invalidates cached/pending rendering before emitting documentChanged().
     * Reassigning the same pointer is a no-op. Does not reassign the optional
     * edit/form controllers; their document association is checked independently.
     */
    void setDocument(PDFDocument *document);
    /**
     * @brief Replaces the optional controller used to preview content drafts.
     * @param editController Non-owned controller, or nullptr for saved-content rendering.
     *
     * Observes per-page draft-render changes, controller document changes, and
     * destruction. Clears old subscriptions and invalidates cached/pending
     * rendering on replacement or destruction, then emits editControllerChanged().
     * An identical pointer is ignored; this method does not commit or discard drafts.
     */
    void setEditController(PDFEditController *editController);
    /**
     * @brief Replaces the optional controller whose worker session supplies live forms.
     * @param formController Non-owned controller, or nullptr to omit that association.
     *
     * Observes form-render invalidations, controller document changes, and
     * destruction. Rebinding invalidates rendering and emits formControllerChanged().
     * The controller must refer to this item's document to contribute its session
     * and revision. An identical pointer is a no-op; form values are not changed here.
     */
    void setFormController(PDFFormController *formController);
    /**
     * @brief Replaces the designer used to suppress original managed widget appearances.
     * @param formDesignerController Non-owned designer, or nullptr to detach.
     *
     * Reconnects field-change/destruction notifications and refreshes the current
     * page's sorted suppression IDs. A relevant design-mode field update can also
     * request immediate rerendering when IDs stay unchanged, because a moved
     * field must no longer appear at its original source rectangle. Emits
     * formDesignerControllerChanged(); identical pointer assignments do nothing.
     */
    void setFormDesignerController(
        PDFFormDesignerController *formDesignerController);
    /**
     * @brief Switches source-widget suppression between ordinary and designer presentation.
     * @param formDesignMode Whether managed source widgets belong to designer overlays.
     *
     * Recomputes suppression IDs, invalidating cached rendering if that set
     * changes, then emits formDesignModeChanged(). Does not alter PDF field
     * definitions or values. Reassigning the same flag is a no-op.
     */
    void setFormDesignMode(bool formDesignMode);
    /**
     * @brief Retargets this presenter to a display page, including during delegate reuse.
     * @param page Zero-based display-page index; stored without clamping.
     *
     * Refreshes designer suppression, updates implicit metrics, and invalidates
     * old page rasters/requests before emitting pageChanged(). Invalid indices
     * cannot render, but this setter does not navigate the document or reject
     * them synchronously. An unchanged value is a no-op.
     */
    void setPage(int page);
    /**
     * @brief Changes the zoom multiplier used for implicit layout dimensions.
     * @param zoom Requested multiplier, bounded with qBound to 0.10 through 5.0.
     *
     * Uses a fuzzy equality check to ignore equivalent values, recomputes implicit
     * page size, and emits zoomChanged(). Does not directly invalidate page content.
     * Resulting actual-size changes use geometryChange() and the raster settling
     * path, allowing the old texture to scale while new zoom input is arriving.
     */
    void setZoom(qreal zoom);
    /**
     * @brief Updates visible tile coverage from an item-local logical rectangle.
     * @param rect Page-space viewport rectangle; empty explicitly means no visible region.
     *
     * Marks visibility as explicitly supplied, stores the rectangle without
     * clamping, clears the budget-blocked flag, and trims cached offscreen tiles.
     * With an attached window, compares the desired plan against current resident
     * and queued coverage. Newly uncovered tiles or an empty visible intersection
     * cancel obsolete work and advance the request serial; movement within covered
     * tiles need not trigger another PDFium render.
     *
     * Emits visiblePageRectChanged() and schedules a planning pass. Equal repeated
     * explicit assignments are ignored. Calling this with an empty rectangle does
     * not restore the initial implicit whole-page-visible behavior.
     */
    void setVisiblePageRect(const QRectF &rect);
    /**
     * @brief Changes the raster background and invalidates existing page content.
     * @param pageColor Color used when filling the worker's page image.
     *
     * Stores the QColor as supplied, performs a full cache invalidation, and emits
     * pageColorChanged(). Equal colors are ignored. This is a presentation setting;
     * it does not write a PDF background object or change the source file.
     */
    void setPageColor(const QColor &pageColor);

signals:
    /**
     * @brief Notifies a document-pointer assignment after metrics/cache refresh.
     *
     * Source load/count/destruction notifications also refresh rendering, but
     * this signal is emitted by setDocument(), not for every source-state change.
     */
    void documentChanged();
    /** @brief Notifies that the observed content-edit controller changed or was destroyed. */
    void editControllerChanged();
    /** @brief Notifies that the observed native-form controller changed or was destroyed. */
    void formControllerChanged();
    /** @brief Notifies that the designer supplying source-widget IDs changed or was destroyed. */
    void formDesignerControllerChanged();
    /** @brief Notifies a change to the local designer-suppression mode. */
    void formDesignModeChanged();
    /** @brief Notifies a change to the stored display-page index after refreshing its state. */
    void pageChanged();
    /** @brief Notifies a changed bounded zoom multiplier after implicit metrics are updated. */
    void zoomChanged();
    /**
     * @brief Notifies an explicit visible-rectangle assignment.
     *
     * The getter's implicit whole-page fallback also follows geometry before its
     * first explicit assignment; this signal is not emitted for every size change.
     */
    void visiblePageRectChanged();
    /** @brief Notifies a changed raster background color after content invalidation. */
    void pageColorChanged();
    /**
     * @brief Notifies a change to the current-content texture acknowledgment flag.
     *
     * Emitted by GUI-side state setters, never directly from updatePaintNode().
     * Read draftTextureCurrent() for the new value; it does not certify complete
     * high-resolution tile coverage or physical presentation of a frame.
     */
    void draftTextureCurrentChanged();
    /**
     * @brief Notifies a change to successful current-content preview/suppression readiness.
     *
     * Overlays use this handoff state to avoid duplicating unsuppressed source
     * objects. A false value may accompany a usable fallback page texture rather
     * than a completely missing render. Emitted on the GUI thread.
     */
    void draftTextureReadyChanged();
    /**
     * @brief Notifies that the accepted base-raster suppression snapshot changed.
     *
     * Allows QML overlay changes to synchronize with the pending base texture in
     * the next frame. This can precede scene-graph upload and is separate from
     * draftTextureCurrentChanged() and draftTextureReadyChanged().
     */
    void displayedSuppressedCommentObjectIdsChanged();

protected:
    /**
     * @brief Schedules initial raster planning after QML component construction completes.
     *
     * Calls the base hook first, then uses the same queued/coalesced render path
     * as later updates. Does not synchronously render the document or create GPU
     * textures while the component is completing.
     */
    void componentComplete() override;
    /**
     * @brief Responds to layout size changes while preserving reusable page content.
     * @param newGeometry New item geometry in the parent's logical coordinate space.
     * @param oldGeometry Previous item geometry in that same space.
     *
     * Calls QQuickItem's hook first. Only a changed size invokes
     * invalidateRasterCache(); pure position changes require no raster replacement.
     * The geometry-only invalidation retains accepted content/suppression state
     * so the scene graph can scale old coverage while zoom input settles, or reuse
     * an already matching request/cache. These rectangles are not PDF page boxes.
     */
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    /**
     * @brief Handles visibility, window membership, and DPR changes affecting rendering.
     * @param change Qt Quick item-change category.
     * @param data Category-specific payload: boolValue for visibility, window for scene changes.
     *
     * Calls the base hook first. Becoming visible or joining a window schedules
     * rendering; becoming hidden or leaving a window fully invalidates the cache
     * to retire unused textures/work. A device-pixel-ratio change also fully
     * invalidates rendering. Other change categories have no additional behavior.
     */
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    /**
     * @brief Synchronizes accepted rasters with Qt Quick's owned texture-node subtree.
     * @param oldNode Previously returned root, or nullptr after initial creation/node loss.
     * @param data Qt synchronization metadata; unused by this implementation.
     * @return Reused/new root for Qt to own, or nullptr after discarding unusable content.
     *
     * Runs in Qt's scene-graph synchronization context: normally the render thread
     * while the GUI thread is blocked, or the GUI thread under a basic render loop.
     * Performs no PDFium work. Uses an identity QSGTransformNode root with raster
     * children matched by entry ID. New images become opaque textures; each child
     * owns its QSGSimpleTextureNode texture with linear filtering and retains the
     * shared raster-budget lease. Stale children are removed and destroyed here.
     *
     * Clears each transfer QImage after its upload attempt, including failures,
     * avoiding a persistent duplicate CPU raster cache. Core rectangles are scaled
     * from their job's full-page pixel grid to current logical item dimensions;
     * source texture rectangles exclude tile gutters without rewriting pixels.
     * Texture destruction precedes release of the node's lease, so GUI metadata
     * eviction alone cannot prematurely free the budget for a live GPU texture.
     *
     * Explicit clearing, nonpositive geometry, missing reusable nodes, or failed
     * texture creation discard the subtree/accepted raster state and queue guarded
     * recovery. After successful synchronization of dirty uploads, queues
     * markTextureReady() with captured generation tokens. Property notifications
     * therefore run on the GUI thread, not from this callback. Acknowledgment
     * confirms synchronization, not completion of a physical display swap.
     */
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    // Tests observe request generations without adding a QML diagnostics API.
    friend class PDFEditControllerTest;

    /**
     * @brief Geometry for one full-page, preview, or detail-tile render operation.
     *
     * All rectangles use the job's complete-page physical-pixel grid, not item
     * logical coordinates. A preview base can have a smaller grid than detail
     * tiles belonging to the same target page generation.
     */
    struct RasterJob {
        /// Whole-page grid: preview dimensions for a preview base, target raster size for details.
        QSize rasterSize;
        /// Core coverage in that grid; detail cores are adjacent without overlapping gutters.
        QRect rect;
        /// Actual rendered region, including clipped tile gutters; its size determines image allocation.
        QRect renderRect;
        /// True for whole-page coverage, either a full-resolution raster or a reduced preview base.
        bool base = false;
    };
    /**
     * @brief Accepted raster metadata plus its transient CPU-to-scene-graph transfer payload.
     *
     * After upload, geometry/identity/lease remain available for cache reuse and
     * node synchronization, while image no longer retains a second pixel copy.
     */
    struct RasterEntry {
        /// Per-item increasing ID used to associate this accepted entry with its texture node.
        quint64 id = 0;
        /// Geometry and base/detail role captured when the raster was accepted.
        RasterJob job;
        /// Transfer payload only; release after the texture upload attempt, including failed uploads.
        QImage image;
        /// Shared accounting lease; scene-graph owners keep it alive until their texture is destroyed.
        PDFRasterBudget::Lease reservation;
        /// Global monotonic use sequence for LRU decisions; this is not a clock timestamp.
        quint64 lastUsed = 0;
    };

    /**
     * @brief Recomputes page layout metrics and fully invalidates old raster content.
     *
     * Used when the document/load/page context changes, including document
     * destruction. Calls updateImplicitPageSize() followed by invalidateCache().
     * Unlike a zoom-only size update, this path intentionally retires previous
     * page coverage and readiness rather than keeping it for scaling.
     */
    void refreshMetrics();
    /**
     * @brief Updates implicit logical dimensions from display-page metrics and zoom.
     *
     * Reads the page size only from a loaded document; absent/unavailable metrics
     * produce an empty size. Sets nonnegative implicit width/height to page size
     * multiplied by m_zoom, without overriding explicitly assigned QML dimensions.
     * Does not itself invalidate cache or queue work; resulting actual geometry
     * changes can trigger geometryChange().
     */
    void updateImplicitPageSize();
    /**
     * @brief Resets accepted/requested raster content for a document or presentation change.
     *
     * Stops both debounce timers, cancels work, advances the request serial, and
     * clears raster entries, generation identities, tiling/budget fallback state,
     * pending upload tokens, and displayed comment-suppression metadata. Marks the
     * content cache dirty and both readiness properties false, requests repaint,
     * and schedules replacement rendering when the item is eligible.
     *
     * Requests scene-graph texture clearing on the next synchronization; it never
     * directly destroys GPU objects on the GUI thread. Leases held by existing
     * nodes remain charged until those textures are actually retired.
     */
    void invalidateCache();
    /**
     * @brief Invalidates changed draft/form content while retaining coherent previous coverage.
     * @param immediate True queues a render now; false restarts the 250 ms draft debounce.
     *
     * Cancels work, advances the request serial, clears requested-generation
     * identities and budget blockage, stops zoom settling, marks the cache dirty,
     * and clears current/ready flags. Accepted rasters, any accepted pending
     * upload, and their suppression snapshot remain intact until replacement so
     * the old texture and QML overlay do not momentarily describe different states.
     *
     * Immediate requests stop the draft timer and schedule rendering; delayed
     * requests wait for its next timeout. Does not directly call update() or clear
     * scene-graph textures. Replacement base acceptance performs the later handoff.
     */
    void invalidateDraftCache(bool immediate);
    /**
     * @brief Replans raster resolution after geometry changes without changing content identity.
     *
     * Stops draft debounce and clears the budget-blocked flag. Retains an existing
     * in-flight request if it already matches current size/DPR/content; otherwise
     * cancels it, advances the request serial, and clears requested identities.
     * Accepted rasters, content-dirty state, readiness, and suppression metadata
     * remain available for immediate scene-graph scaling.
     *
     * When accepted content already matches the new target dimensions, schedules
     * planning to fill missing tiles without a zoom delay. Otherwise restarts the
     * zoom settle timer. Requests repaint in either case, avoiding repeated PDFium
     * rasterization for every intermediate zoom geometry update.
     */
    void invalidateRasterCache();
    /**
     * @brief Cancels this item's queued/active worker render continuation without waiting.
     *
     * Removes queued owner work through PDFFormWorker, sets the shared atomic
     * cancellation token, and drops the local token, pending flag, remaining job
     * list, and dispatch closure. Active work cooperatively observes cancellation;
     * this method neither joins the worker nor guarantees it already stopped.
     *
     * Does not advance request serials, stop timers, clear accepted identities, or
     * discard textures. Callers separately perform the level of invalidation
     * needed for their state transition.
     */
    void cancelPendingRender();
    /**
     * @brief Handles a content-edit preview invalidation for this display page.
     * @param page Exact zero-based display page; negative values are not a broadcast here.
     * @param revision New page draft revision, which must equal currentDraftRevision().
     * @param immediate Whether to avoid the draft debounce delay.
     *
     * Ignores other pages or obsolete revisions. Delayed notifications always
     * restart the draft debounce. Immediate notifications stop that timer but
     * avoid duplicate work when a matching request, pending texture upload, or
     * clean current texture already represents the revision. Otherwise calls
     * invalidateDraftCache(true), retaining old coherent coverage until replacement.
     */
    void handleDraftRenderStateChanged(int page, quint64 revision, bool immediate);
    /**
     * @brief Handles native form-render invalidation for one display page or every page.
     * @param page Zero-based display page, or a negative all-pages notification.
     * @param revision Revision that must match currentFormRevision() for this item.
     *
     * Ignores unrelated/stale events. Reuses a matching pending request, pending
     * texture upload, or clean accepted cache instead of dispatching duplicate
     * work; unlike draft-change reuse, the cache check does not require the
     * current-texture acknowledgment flag. Otherwise invalidates draft cache
     * immediately because native form appearance updates should not wait for typing debounce.
     */
    void handleFormRenderStateChanged(int page, quint64 revision);
    /**
     * @brief Rebuilds the source-widget suppression IDs for the current designer page.
     * @return True if the sorted unique ID list changed and caused full cache invalidation.
     *
     * With design mode, an attached designer, and a nonnegative display page,
     * collects sourceFieldIdsForPage(), including current and removed source-backed
     * fields. Otherwise uses an empty list. Sorting/deduplication makes equivalent
     * sets stable; an unchanged set returns false without invalidation. This
     * helper does not separately verify the designer's document association.
     */
    bool refreshSuppressedFormWidgetIds();
    /**
     * @brief Returns the current display page's draft-render revision for a matching controller.
     * @return Controller page revision, or zero if no edit controller matches this document.
     *
     * Used in request and cache identity checks. Zero can also be a valid page
     * revision when no draft entry exists; it is not by itself an error indicator.
     */
    quint64 currentDraftRevision() const;
    /**
     * @brief Returns the current display page's live-form revision for a matching controller.
     * @return Controller page/global form revision, or zero for no matching form controller.
     *
     * Delegates to formRevisionForPage(), including its all-pages fallback token.
     * Does not query PDFium synchronously or advance any revision itself.
     */
    quint64 currentFormRevision() const;
    /**
     * @brief Checks whether accepted raster metadata describes the current target generation.
     * @param pixelSize Complete target page raster dimensions in physical pixels.
     * @param devicePixelRatio Window DPR used to derive that target size.
     * @return True for a clean, nonzero accepted request with matching identities and geometry.
     *
     * Compares page, document pointer/render revision, edit/form controller pointers
     * and their page revisions, exact target dimensions, and fuzzy-equal DPR.
     * Does not independently establish complete tile coverage, matching tiling
     * mode, visibility, texture upload success, or successful preview suppression.
     * Callers must inspect those additional conditions when they matter.
     */
    bool cachedRasterMatchesCurrentState(const QSize &pixelSize,
                                         qreal devicePixelRatio) const;
    /**
     * @brief Checks whether an active render request already targets the current state.
     * @param pixelSize Complete target page raster dimensions in physical pixels.
     * @param devicePixelRatio Window DPR accompanying that size.
     * @return True when rendering is pending and its captured state still matches.
     *
     * Compares requested document/render revision, page, controller identities,
     * current draft/form revisions, dimensions, and fuzzy-equal DPR. This is a
     * generation check, not a guarantee that every newly visible tile is queued
     * or that any texture has reached the scene graph.
     */
    bool pendingRenderMatchesCurrentState(const QSize &pixelSize,
                                          qreal devicePixelRatio) const;
    /**
     * @brief Queues GUI-side recovery after scene-graph texture/node loss.
     *
     * May be called from updatePaintNode(); captures a QPointer and uses a queued
     * invocation so property signals and scheduling happen on the owning thread.
     * The callback exits if the item died, a replacement established a nonzero
     * cached request ID, or the cache is no longer dirty. Otherwise clears texture
     * readiness, cancels work, advances the serial, clears budget blockage, and
     * schedules rendering. Does not synchronously render or clear suppression IDs.
     */
    void queueTextureLossRecovery();
    /**
     * @brief Stores the content-generation texture acknowledgment and notifies on change.
     * @param current New cached draftTextureCurrent value.
     *
     * GUI-thread helper only; callers must validate freshness before setting true.
     * An unchanged value emits nothing. Does not schedule work or inspect textures.
     */
    void setDraftTextureCurrent(bool current);
    /**
     * @brief Stores current-content preview/suppression readiness and notifies on change.
     * @param ready New cached draftTextureReady value.
     *
     * GUI-thread helper with no independent generation or suppression validation.
     * An unchanged value is ignored. Readiness does not imply full tile completion.
     */
    void setDraftTextureReady(bool ready);
    /**
     * @brief Replaces the accepted base-raster suppression list on the GUI thread.
     * @param objectIds Snapshot list passed by value and moved into storage if changed.
     *
     * Compares the complete ordered list and emits the matching property signal
     * only on inequality. Does not sort, deduplicate, validate IDs, suppress PDF
     * objects, or independently synchronize scene-graph nodes.
     */
    void setDisplayedSuppressedCommentObjectIds(QStringList objectIds);
    /**
     * @brief Accepts a queued acknowledgment that the scene graph synchronized current rasters.
     * @param requestId Accepted raster request whose upload/synchronization completed.
     * @param draftRevision Draft revision captured with the pending upload.
     * @param formRevision Form revision captured with the pending upload.
     * @param suppressionApplied Whether the raster's requested preview changes were successful.
     *
     * Runs on the GUI thread after updatePaintNode(). Rejects dirty cache, stale
     * accepted request, missing/replaced document or controllers, changed document
     * revision/page, and mismatched cached/current draft/form revisions. Accepted
     * acknowledgment sets current=true and ready=suppressionApplied.
     *
     * Deliberately does not independently compare current item size or DPR: a
     * valid content texture may remain useful while scaled through zoom. This
     * confirms scene-graph synchronization, not a completed physical display frame.
     */
    void markTextureReady(quint64 requestId,
                          quint64 draftRevision,
                          quint64 formRevision,
                          bool suppressionApplied);
    /**
     * @brief Coalesces GUI requests into one later render-planning pass.
     *
     * Does nothing before component completion or while a planning callback is
     * already queued. The callback clears the scheduled flag and returns if draft
     * or zoom debounce is still active; the timer timeout will schedule again.
     * Otherwise calls requestRenderForCurrentSize(). Performs no PDFium rendering,
     * pixel allocation, or blocking wait in the scheduling call itself.
     */
    void scheduleRender();
    /**
     * @brief Derives a physical target raster from the current eligible item/window geometry.
     *
     * Requires a visible item, attached window, loaded document, and positive
     * logical dimensions. PDFRasterPlan::rasterSize() converts logical size with
     * window DPR, rounding upward and rejecting invalid/nonfinite/oversized input.
     * Empty target size, empty clipped visible region, or active budget blockage
     * prevents dispatch. Otherwise forwards the computed full-page size and DPR
     * to requestRender(); this method does not render synchronously.
     */
    void requestRenderForCurrentSize();
    /**
     * @brief Plans a raster generation and captures the source state required by its worker jobs.
     * @param pixelSize Complete page target dimensions in physical pixels, not tile size or PDF points.
     * @param devicePixelRatio Window DPR associated with those dimensions and snapshot guards.
     *
     * Requires a loaded document, nonempty dimensions, and a render source path.
     * Reuses an identical pending generation. Builds a full-page or hybrid
     * preview/detail plan using normalized visibility, tiled-mode hysteresis, and
     * memory-pressure fallback. Existing entries are reused only when accepted
     * content/geometry identities and tiling mode match. Otherwise queues a base
     * first, followed by missing visible/preload tiles. No jobs means no dispatch.
     *
     * For a new generation, cancels old work and snapshots document/display/source
     * page identity, render revision, password/form-session key, transient-blank
     * state, controller pointers/revisions, background, and requested preview
     * modifications. Matching edit/form controllers contribute their own state;
     * source-widget suppression uses the designer ID snapshot already held here.
     * No worker may dereference the GUI document's PDFium handle.
     *
     * Creates a shared cancellation token and a continuation that submits one
     * RasterJob at a time through PDFFormWorker. Before allocating pixels, the
     * worker requires budget capacity for CPU/texture overlap plus applicable
     * replacement headroom and acquires a shared reservation. It renders under
     * the PDFium synchronization rules, checks cancellation around work, and
     * queues completion back to the GUI through a weak item reference. Later
     * jobs are dispatched after result acceptance, leaving queue opportunities
     * for native form input and other visible pages between tile renders.
     */
    void requestRender(const QSize &pixelSize, qreal devicePixelRatio);
    /**
     * @brief Submits the first remaining job through the current render continuation.
     *
     * The front job stays in the list until its accepted completion removes it;
     * dispatch does not optimistically mark coverage resident. When no job or
     * continuation remains, clears the pending flag/closure and releases the
     * local cancellation-token reference. Does not block for worker completion.
     */
    void dispatchNextRaster();
    /**
     * @brief Clips visible logical page geometry and converts it to unit-page coordinates.
     * @return Top-left normalized rectangle, or empty for nonpositive dimensions/no intersection.
     *
     * Intersects visiblePageRect() with boundingRect(), then divides horizontal
     * components by width and vertical components by height. Without an explicit
     * visible assignment, the getter supplies the whole item. The result is
     * independent of DPR and PDF-point units and is used for tile visibility/LRU.
     */
    QRectF normalizedVisibleRect() const;
    /**
     * @brief Tests whether accepted entries contain a detail tile with this exact core rectangle.
     * @param rect Tile core coverage in the full target page's physical-pixel grid.
     * @return True for an equal non-base job rectangle; full-page/preview bases do not count.
     *
     * Performs only a geometry lookup. Does not verify document/revision/DPR,
     * target size, texture upload, or suppression state. Callers establish the
     * accepted generation's validity separately before using this as coverage.
     */
    bool hasRasterTile(const QRect &rect) const;
    /**
     * @brief Retains visible coverage and bounds this item's offscreen detail cache.
     *
     * Maps entries through their own raster grids to normalized page coordinates
     * and refreshes lastUsed for entries intersecting the visible region. Keeps
     * base coverage and evicts least-recently-used offscreen detail entries until
     * their reservation bytes fit the retained-tile allowance. Requests a scene
     * graph update when removing entries.
     *
     * This is GUI metadata eviction: actual global capacity may not return until
     * scene-graph nodes release their shared leases after texture destruction.
     * It does not remove visible detail solely to satisfy the local retention cap.
     */
    void trimRasterCache();
    /**
     * @brief Requests global LRU eviction of replaceable coverage to make budget capacity available.
     * @param bytesNeeded Desired free capacity in bytes, not an unconditional number to remove.
     *
     * GUI-thread operation over registered live page items. Computes the shortage
     * against current free capacity, then considers entries outside each item's
     * visible region and detail tiles whose content/resolution is obsolete.
     * Protects visible base coverage. Removes candidates in global usage order;
     * removing an offscreen base fully invalidates its owning item's cache, while
     * detail removal updates only that coverage and requests scene-graph sync.
     *
     * Counts planned removals toward the shortage, but shared GPU leases can delay
     * actual release. Does not synchronously destroy textures, reserve memory for
     * a caller, or guarantee immediate success of the next worker reservation.
     */
    static void evictUnusedRasters(qint64 bytesNeeded);
    /**
     * @brief Wakes visible renderers whose remembered budget requirement may now fit.
     *
     * Called on the GUI thread, including through the queued budget-release hook.
     * Reads current free capacity and visits blocked registered items with a
     * nonempty visible region. Eligible items have their blocked flag cleared and
     * planning scheduled. Does not reserve bytes or promise all awakened items can
     * proceed; each worker still checks capacity when dispatched. Release-driven
     * wakeups avoid a polling loop while live textures retain the needed memory.
     */
    static void retryBudgetBlockedRenders();
    /**
     * @brief Validates and accepts one worker raster, or handles its allocation/render failure.
     * @param requestId Captured request serial; must still identify this item's current generation.
     * @param document Non-owning source identity snapshot, compared with the current guarded pointer.
     * @param editController Non-owning content-controller identity captured at dispatch.
     * @param formController Non-owning live-form-controller identity captured at dispatch.
     * @param revision Source document's render revision at dispatch.
     * @param page Zero-based display-page index; source mapping occurred before worker execution.
     * @param devicePixelRatio Captured window DPR.
     * @param pixelSize Complete target page raster size in physical pixels, distinct from job size.
     * @param draftRevision Captured page draft-render revision.
     * @param formRevision Captured page form-render revision.
     * @param suppressionApplied Combined success of requested preview modifications and
     *                           eligibility of all relevant draft objects for suppression.
     * @param suppressedCommentObjectIds Comment/link suppression identities for this snapshot.
     * @param job Geometry and base/detail role of the rendered region.
     * @param image Rendered region payload; null indicates failure or budget refusal.
     * @param reservation Shared lease moved into an accepted raster entry to account for its pixels.
     * @param budgetDenied Distinguishes allocation-budget refusal from ordinary rendering failure.
     *
     * Runs on the GUI thread. Before accepting anything, compares request serial,
     * window presence, document/controller identities, document/page revisions,
     * current/requested raster size and DPR, and current/requested draft/form
     * revisions. Stale results cannot update presentation; payload reservations
     * are released when their remaining owners drop them.
     *
     * A null image cancels remaining jobs. Budget denial for a non-tiled base
     * switches to a smaller hybrid preview/detail plan. Other budget denials
     * remember the required overlap/headroom capacity, mark the item blocked,
     * request global unused-raster eviction, and retry only when capacity allows.
     * Ordinary render failure does not start an automatic busy retry loop.
     *
     * A successful base replaces old entries and publishes its suppression/tiling
     * snapshot before detail refinement. A detail with suppression behavior that
     * differs from the accepted base cancels the generation, avoiding mixed
     * source/draft appearances on one page. Accepted rasters receive an entry ID,
     * LRU sequence, and lease; update cached identity metadata and pending-upload
     * tokens; remove the finished job; trim unused details; request scene-graph
     * synchronization; and dispatch the next job. Acceptance alone does not set
     * texture readiness: updatePaintNode()/markTextureReady() complete that handoff.
     */
    void handleRenderResult(quint64 requestId,
                            PDFDocument *document,
                            PDFEditController *editController,
                            PDFFormController *formController,
                            int revision,
                            int page,
                            qreal devicePixelRatio,
                            const QSize &pixelSize,
                            quint64 draftRevision,
                            quint64 formRevision,
                            bool suppressionApplied,
                            const QStringList &suppressedCommentObjectIds,
                            const RasterJob &job,
                            const QImage &image,
                            PDFRasterBudget::Lease reservation,
                            bool budgetDenied);

    QPointer<PDFDocument> m_document;
    QPointer<PDFEditController> m_editController;
    QPointer<PDFFormController> m_formController;
    QPointer<PDFFormDesignerController> m_formDesignerController;
    bool m_formDesignMode = false;
    QStringList m_suppressedFormWidgetIds;
    int m_page = 0;
    qreal m_zoom = 1.0;
    QRectF m_visiblePageRect;
    bool m_hasExplicitVisibleRect = false;
    QColor m_pageColor = Qt::white;
    bool m_cacheDirty = true;
    QVector<RasterEntry> m_rasters;
    QVector<RasterJob> m_renderJobs;
    std::function<void(const RasterJob &)> m_renderWork;
    quint64 m_nextRasterId = 0;
    bool m_cachedTiled = false;
    bool m_requestedTiled = false;
    bool m_forceTiledForBudget = false;
    bool m_budgetBlocked = false;
    qint64 m_budgetRetryBytes = 0;
    bool m_cachedSuppressionApplied = false;
    bool m_textureDirty = false;
    bool m_clearTexture = true;
    bool m_draftTextureCurrent = false;
    bool m_draftTextureReady = false;
    QStringList m_displayedSuppressedCommentObjectIds;
    bool m_renderScheduled = false;
    QTimer m_draftRenderTimer;
    QTimer m_zoomRenderTimer;
    quint64 m_cachedRequestId = 0;
    QSize m_cachedPixelSize;
    int m_cachedPage = -1;
    qreal m_cachedDevicePixelRatio = 0.0;
    int m_cachedRevision = -1;
    PDFDocument *m_cachedDocument = nullptr;
    quint64 m_cachedDraftRevision = 0;
    PDFEditController *m_cachedEditController = nullptr;
    quint64 m_cachedFormRevision = 0;
    PDFFormController *m_cachedFormController = nullptr;
    bool m_renderPending = false;
    QSize m_requestedPixelSize;
    int m_requestedPage = -1;
    qreal m_requestedDevicePixelRatio = 0.0;
    int m_requestedRevision = -1;
    PDFDocument *m_requestedDocument = nullptr;
    quint64 m_requestedDraftRevision = 0;
    PDFEditController *m_requestedEditController = nullptr;
    quint64 m_requestedFormRevision = 0;
    PDFFormController *m_requestedFormController = nullptr;
    quint64 m_pendingTextureRequestId = 0;
    quint64 m_pendingTextureDraftRevision = 0;
    quint64 m_pendingTextureFormRevision = 0;
    bool m_pendingSuppressionApplied = false;
    std::shared_ptr<std::atomic_bool> m_renderCancellation;
    quint64 m_requestSerial = 0;
};
