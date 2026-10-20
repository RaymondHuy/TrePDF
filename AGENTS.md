# TrePDF Agent Guide

## Communication

- Reply to the user in Vietnamese by default.
- Keep explanations practical and concise. The user prefers implementation over long proposals.
- Preserve user changes in the working tree. Do not reset, revert, or reformat unrelated code.

## Project Summary

TrePDF is a macOS desktop PDF reader built with Qt 6, Qt Quick Controls, QML,
C++23, and PDFium. The current scope includes PDF viewing, sandboxed AcroForm
interaction, draft editing for supported text, images, and page operations,
and Save/Save As persistence.

Current features:

- The Information tab after Settings shows the running application version from
  `Qt.application.version` (`TREPDF_VERSION`) and the contact email
  `trebase177@gmail.com`. `InformationRibbon.qml` is loaded on demand and works
  without an open PDF. The email is read-only, selectable and copyable; it does
  not send messages. Normal ribbon switching and Header/Footer exit guards apply.

- Settings > Font opens a lazy modal, including without an open PDF, for a
  family name and optional Regular/Bold/Italic/Bold Italic TTF/OTF files.
  `UserFontManager` validates local snapshots and `ApplicationFonts` installs
  each batch atomically in `AppLocalDataLocation/user-font`. Imports share
  the existing exact-face registry, picker, edit preflight and Save pipeline,
  and reload on startup. An editable Preview with Bold/Italic toggles uses
  temporary private aliases of the selected files, without installing them;
  missing or mismatched styles remain unavailable instead of being synthesized.
  User fonts and downloaded fonts share a 128-file /
  256 MiB storage bound. See `docs/font-providers.md` for paths and limits.

- Entering Header requires a clean active PDF. Main commits pending inline,
  panel and form input before checking all session drafts. Unsaved changes keep
  the previous ribbon tab and show a friendly Save/Got it notice; save or undo
  all changes, then enter Header again. Edits within Header remain usable.
  Leaving Header for any other ribbon tab also requires Save or Undo, including
  Find navigation and unapplied text/image/page-number placement. The exit
  notice preserves Header UI; its Save action submits pending placement with
  the chosen page range before saving. Invalid input stays editable and blocks
  Save. The clean-entry boundary reuses the per-session command history.
  Header's Save button has its own File ribbon group after the Header tools,
  matching View, and shares the exit notice's save flow, submitting pending
  Header placement with its selected page range.
  Switching to a dirty PDF while Header is active returns to View with the
  same notice. Other dirty PDF tabs do not block a clean active PDF.

- Header > Select enables synchronization by default in a right-side panel when
  an eligible object is selected; users can uncheck it for that selection. It matches
  the selected text/image's pre-edit snapshot against other 100-point headers,
  then Save, Cmd/Ctrl+S or Save As commits input, waits for the current scan and
  applies content, style, geometry or deletion to all matches or a custom range
  before writing. The synchronization panel has no separate Apply changes button.
  Invalid or empty custom ranges and scan/apply failures stop saving and retain
  the edits; disabling synchronization saves only the selected object's change.
  A dirty enabled anchor survives blank deselection until another object, tool
  or history change replaces its matching context. Synchronization remains per
  selection. Text content/style/resize changes copy the edited frame's physical
  size so narrower group members keep the same wrapping. Resized text is reflowed
  before validating its content bounds, including after movement near page edges.
  Moving managed Header groups copies each changed physical coordinate instead
  of accumulating earlier per-page offsets; unmarked headers keep relative movement.
  Header text also persists its
  logical paragraph/frame through Save/reopen; bounded grouping recovers older
  marked glyph fragments without joining ambiguous overprints. Newly created Header batches retain a UUID/role in bounded
  `TrePDFHeader` object marks through Save/reopen. Unmarked PDF headers require
  matching text/style/physical geometry or decoded image pixels; ambiguous
  pages are skipped. Page-number formatting/movement retains per-page values.
  `PDFEditControllerHeaderSync.cpp` owns bounded cancellable scanning and atomic
  `HeaderDraftBatch` history, merging consecutive anchor edits into one Undo.
  See `docs/header-text-pipeline.md` for matching and lifecycle details.

- Header > Select activates on entry and reuses Edit selection and property
  panels for objects entirely inside the first 100 PDF points. The transient
  `headerSelectionOnly` controller scope filters source/draft pointer hits,
  direct selection, keyboard traversal and history restoration. Move/resize
  and image rotation stay inside the header; text retains Edit reflow behavior
  and leaves Header selection if committed content overflows. Losing focus retains
  pending Header/Footer text and images. Select or a different placement tool
  submits valid pending content to undoable, unsaved drafts using the chosen page
  range; invalid input blocks the tool change and stays editable. Cancel/Escape
  explicitly discards pending placement. Edits affect one page object and use normal
  Undo/Redo and Save; enabled synchronization propagates them to matching headers
  during Save. See `docs/header-text-pipeline.md`.

- Header > Page numbers places 12-point black numbers by clicking the gray
  100-point header, then opens a right panel for inclusive PDF start/stop pages
  and optional Roman/Arabic starts. Both sequences begin at one; Arabic resets
  to 1 after Roman. Roman supports i–mmmcmxcix; a batch supports 10,000 pages.
  `HeaderPageNumberPlacement.qml` owns pending state independently of pooled
  delegates; previews share `headerPageNumberText()` with the controller.
  `addHeaderPageNumbers()` uses the atomic added-text batch pipeline for
  validation, Undo/Redo and Save/Save As. Numbers become ordinary page text;
  later page operations do not renumber them automatically. See
  `docs/header-text-pipeline.md` for lifecycle and range semantics.

- Header > Image arms placement in the gray 100-point header, opens the local
  image picker after a click, then shows a temporary image and a right panel.
  `HeaderImagePlacement.qml` owns the pending snapshot geometry independently of
  pooled delegates; `HeaderImageOverlay.qml` supports drag/eight-handle resize,
  and `HeaderImagePanel.qml` offers aspect lock, quarter turns and page ranges.
  `prepareHeaderImage()` validates/snapshots pixels; `addHeaderImage()` in
  `PDFEditControllerHeaderText.cpp` validates every target frame before one
  undoable added-image batch. Save uses the existing image writer.
  Cancel, session/history/structure changes abandon pending input. Select/tool
  changes and Save submit it first; ribbon tab guards preserve pending content.

- Header > Add Text arms click placement inside the gray 100-point header.
  `HeaderTextOverlay.qml` shows an inline textbox at the clicked point, then
  `HeaderTextPanel.qml` opens for font family/size/style/color and all-pages/custom
  page-range selection. Font family changes apply to selected characters or new
  text at the caret; size/style update the whole pending textbox. Color changes
  apply to new text at the caret. Per-character fonts and colors persist through
  the existing added-text writer and survive delegate pooling.
  Edit > Add Text shares Header's initial family selection via
  `headerTextFontFamily()`, including available system/downloaded Arial when
  no Arial font is bundled.
  `HeaderTextPlacement.qml` owns temporary input independently of pooled delegates. `PDFEditControllerHeaderText.cpp` validates the complete
  batch before creating ordinary added-text drafts inside the first 100 PDF
  points. Apply is one undoable action; Save/Save As uses the existing text
  writer, and reopened headers are ordinary independently editable page text.
  The gray 100-point guide is presentation-only. Footer shares this pipeline in
  the last 100 PDF points, with its own tools, messages, synchronization scope
  and Save/Undo tab guards. See `docs/header-text-pipeline.md` for limits and
  lifecycle behavior.

- Protect > Watermark adds, updates and removes managed text/image watermarks
  across selected source pages. `PDFWatermark.h/.cpp` shares bounded versioned
  metadata, vector text/image snapshots, placement and PDFium mutation between
  scanning, preview and Save. `PDFEditControllerWatermarks.cpp` owns per-session
  groups and undoable changes; `WatermarkDialog.qml` is a lazy session-bound UI.
  Watermarks persist as marked page objects, survive reopen and stay excluded
  from ordinary Edit candidates. Other page drafts must be saved/discarded
  before managing watermarks; pending watermarks block page-content/page-structure
  edits until Save/Discard to keep preview layer order consistent. Password
  changes may be saved together. See `docs/watermark-pipeline.md` for limits,
  fingerprint validation, source protection, and preview-document ownership.

- The Protect tab, between Sign and Convert, sets an opening password on an
  unencrypted PDF or changes it on an unlocked PDF without permission
  restrictions. `SetPasswordDialog.qml` provides masked new/confirmation inputs
  and a show-password toggle; the session supplies the authenticated source
  password. Apply stores a per-session pending change; successful Save/Save As
  applies it, Discard retains the original password, and content undo retains
  the pending change. A failed save keeps the pending password and destination
  intact. Password-only changes pass the source directly through qpdf to
  preserve content, annotations and forms. When content/form changes are also
  pending, `FPDF_REMOVE_SECURITY` creates plaintext staging for canonicalization
  before `PDFPasswordWriter` applies AES-256 revision 6 through bundled qpdf
  12.4.1. PDFium validates the password, encryption and page count and rejects
  passwordless opening before atomic `QSaveFile` commit. Passwords stay in
  memory, never in settings, logs or CLI arguments. Password changes on
  permission-restricted PDFs, password removal and permission controls are
  unsupported. Ordinary encrypted saves without a pending replacement keep
  their existing annotation/AcroForm canonicalizer limitations.

- The Sign tab creates visible signatures/initials from bounded freehand strokes,
  typed text, or a local image with optional white removal; it also places dates
  and check/cross marks. `SignatureDialog.qml` prepares the preview through the
  GUI-thread `SignatureLibrary`, which stores up to 32 named signature/initial
  PNG snapshots atomically in `AppDataLocation/signatures.json`. The library is
  independent of PDF draft history. `Main.qml` binds pending placement to the
  active session and cancels it on tab/session/document/save/history/tool changes
  or Escape. Placement uses `PDFEditController::addImageData()` and the existing
  added-image pipeline for move/resize/rotate/delete, undo/redo/discard and
  Save/Save As; reopened items are ordinary PDF images. Sign enables the shared
  image properties panel. This does not create certificate signatures, verify
  existing signatures, or send online signing requests.

- Edit > Link > Auto-create links from URLs scans saved source text on a separate,
  bounded single worker through `PDFWebLinkScanner`. Only HTTP/HTTPS URLs are
  accepted, including PDFium's `www` detection; existing Link regions are skipped.
  Other unsaved edits must be saved first. The batch is held separately from
  comments/text in `PDFEditControllerWebLinks.cpp` and participates in
  undo/redo/discard and atomic Save/Save As. Source-page identities survive page
  insertion/deletion. `WebLinkDraftLayer.qml` shows Edit frames and routes Browse
  clicks through the existing external-link confirmation. Save adds borderless
  `/Link` URI annotations, one per detected line rectangle, without regenerating
  page content. Remove web links scans saved HTTP/HTTPS Link annotations,
  skips protected or structurally unsafe annotations, and clears pending added
  links in one undoable batch. Internal destinations, other URI schemes, and
  comments remain unchanged. Draft removals suppress source appearances and
  Browse clicks; Save revalidates source fingerprints before deleting links.
  Add or edit a link uses `LinkTool`, `ManualLinkLayer.qml`, and
  `LinkPropertiesDialog.qml` for rectangle placement, selection, move/resize,
  deletion, HTTP/HTTPS destinations, and internal page destinations. Manual
  drafts live in `m_manualLinks`, share link history, suppress edited source
  appearances/clicks, and persist via marker-based canonicalizer patches.
  Source page identities preserve destinations through page operations; a
  deleted destination prevents Save. Newly inserted pages need Save before
  becoming link sources/destinations. Protected or unsupported source links
  remain read-only. New edits cancel
  in-flight detection; completed links retain independent spatial geometry.

- Convert > Recognize Text (OCR) opens a lazy panel and a language modal explaining
  why the user selects the languages in the scan. English/Vietnamese are bundled
  and initially selected; up to four catalog languages can be combined.
  `OCRLanguageManager` verifies existing models asynchronously and downloads
  missing models from a pinned official `tessdata_fast` revision, with progress,
  cancellation, retry and persistent offline reuse. Documents are never uploaded.
  Applying languages does not start OCR; Cancel keeps the previous selection.
  Recognition supports all, current or selected PDF pages. A clean saved source is
  required. `PDFOCRController` owns a bounded, cancellable worker per job;
  `PDFOCRPipeline` renders through private PDFium under the process-wide lock,
  snapshots the selected model files and PDF font into a private directory with
  bounded copies and pinned SHA-256 validation, recognizes with Tesseract/Leptonica
  outside that lock, and adds an
  invisible text-only layer through qpdf without replacing source page images.
  Pages with existing text are skipped. The validated result is atomically saved
  as a separate PDF and opens in a new tab. OCR is an export, not a content draft;
  source changes/close cancel work. Desktop macOS, Windows and Linux share the
  implementation; iOS hides the unavailable feature. See `docs/ocr-pipeline.md`
  for limits, packaging, geometry, encryption and destination safety.

- The Convert tab writes detected PDF tables to Excel, one worksheet per table.
  Export requires saved source content and text-based ruled grids with closed
  perimeters; rectangular merged cells are preserved. Borderless/scan tables
  and malformed or nonrectangular merged regions remain unsupported. Values remain
  literal text. `PDFExcelExportController` owns per-session state and uses a
  separate single-worker PDFium document, bounded `PDFTableExtractor` geometry,
  cancellation, staging, and atomic `QSaveFile` commit. `SpreadsheetWorkbook`
  is the replaceable writer interface; only `LibxlsxwriterWorkbook.cpp` includes
  libxlsxwriter. See `docs/excel-export-pipeline.md` for the complete contract.

- Open multiple local PDF files in independent tabs and close them separately.
- Edit > Pages > Merge PDF opens a lazy file-list dialog even without an open
  document. Users add local PDFs, inspect page counts, reorder them by dragging
  or using Up/Down, remove files, and save the merged pages as a separate PDF.
  `PDFMergeController` owns bounded asynchronous inspection and qpdf-based
  merging, cancellation, source validation and atomic output. Main checks open
  input drafts and rejects destinations used by open tabs; success opens the
  result in a new tab. See `docs/pdf-merge-pipeline.md` for supported features
  and limits.
- Edit > Pages > Split PDF sits beside Merge PDF and opens a lazy modal with
  one-file-per-page, every-N-pages and custom page-group modes. The active PDF
  is offered as the source, or users can choose another local PDF.
  `PDFSplitController` validates saved input, copies page content through qpdf
  and publishes the complete batch in a new uniquely named subfolder.
  Existing files and the source remain untouched; cancellation before
  publication leaves no partial output. Open source drafts must be saved first.
  Internal links are remapped within each output; links to omitted pages are
  disabled while retaining their annotations. AcroForms, encrypted PDFs and
  unsupported interactive structures are rejected. See `docs/pdf-split-pipeline.md` for range syntax, limits,
  preservation rules and cancellation behavior.
- Open local PDFs through Finder/Open With and the Dock on macOS, positional
  command-line paths/file URLs, or window drag-and-drop. `PDFFileOpenHandler`
  buffers native requests before QML loads, filters local PDF files and deduplicates
  canonical paths. `Main.qml` queues opens during saves/modal dialogs and reuses
  the existing tab/password flow. Cmd/Ctrl+O opens the picker; Cmd/Ctrl+F commits
  active edits to drafts, switches to View, reveals Search and selects its query.
  The macOS bundle declares PDF Editor support with Alternate handler rank;
  it does not change the default PDF application.
- Insert a blank page immediately after the current page or delete the current
  page, with undo/redo/discard and Save/Save As persistence.
- Add Sticky Note, Free Text, Highlight, Squiggly, Underline, Strikeout, Stamp,
  Square, Circle, Line, Polygon, PolyLine, and Ink annotations from the Comment
  tab; Ink is drawn with the Pencil tool. The Comment tab has its own Select
  button and activates it by default on entry. This `CommentSelectTool` shares
  the normal selection mechanics but only accepts comment candidates through
  hit testing, direct selection, keyboard traversal, and history restoration;
  page text, images, Edit shapes, and form objects remain excluded. New comments
  are undoable session drafts; supported source comments can be indexed,
  selected, edited, and deleted with undo/redo/discard. Save/Save As persists
  additions, content edits, and removals without rewriting page content streams
  on annotation-only pages.
  Square, Circle, Line, Polygon, and PolyLine are drawing-comment drafts with
  shared stroke/interior/opacity/border/effect/line-ending properties. Ink
  shares the stroke/opacity/border pipeline without interior, effect, or line
  endings; all six use subtype-safe vector `/AP`. A Pencil drag creates one Ink
  annotation; the model and source rehydration preserve multiple strokes as
  `/InkList`.
  QML samples points at least one logical pixel apart, keeps at most 4,096 raw
  points, and continues updating the final endpoint after reaching that cap.
  The controller validates and deduplicates the input, applies
  Ramer-Douglas-Peucker simplification at about 0.75 PDF point, and caps the
  result at 1,024 points. Source Ink scanning is bounded to 64 strokes, 4,096
  points per stroke, and 32,768 points total; malformed or oversized Ink stays
  available as a generic read-only comment. Stamp provides 24 presets in
  Dynamic (5), Standard (12), Sign Here (5), and Additional (2) groups; the latter
  retains For Comment and Not Approved. Dynamic Approved, Revised, Reviewed,
  Received, and Confidential capture the current author and date/time at
  placement and retain that snapshot through contents edits, history, and
  Save/reopen. `PDFStampPresets.h` shares the colored background and white
  icon/text vector artwork between QML and `/AP`, including Unicode outlines.
  Added Stamp
  drafts support click/drag placement, move, eight-handle resize, delete,
  undo/redo/discard, and Save/Save As as `/Stamp` with `/Name`, `/Contents`,
  Print, and vector `/AP`. Eligible source Stamp supports drag/arrow-key move,
  eight-handle resize, contents edits, replies, deletion, undo/redo/discard,
  and Save/Save As. Source geometry Save patches `/Rect` after serialization
  while preserving the original `/AP`, its `/BBox`, and unrelated dictionary
  entries. Caret, FileAttachment,
  Sound, Redact, and PDF 2.0 Projection remain indexed read-only for panel
  navigation.
  Added and valid source Square, Circle, Line, Polygon, PolyLine, and Ink can be
  moved by dragging the selected bounding box or using arrow keys, and resized
  with eight side/corner handles. Resize applies the frame's horizontal and
  vertical scale to every vertex or Ink stroke; each gesture is one undoable
  command. Saving source drawing geometry patches `/Rect` plus `/L`,
  `/Vertices`, or `/InkList` after serialization without allowing PDFium to
  mutate the source appearance `/BBox`. Pure movement and external/custom
  appearances preserve `/AP`; a resized TrePDF-managed drawing comment receives
  a matching canonical `/AP` when its captured properties are fully supported.
  Selecting a drawing comment while the Edit tab is active opens a dedicated
  Drawing Comment Properties panel with stroke color, optional interior color,
  and opacity controls. The Comments panel remains dedicated to comment/thread
  content and no longer hosts drawing appearance controls. Each accepted
  change is one undoable command and previews live. Added drawing comments
  support these controls; all six reopened TrePDF-managed drawing subtypes
  support them only when
  provenance and captured appearance properties are safe. External/custom
  appearances stay property-read-only. Source style Save patches `/C`, `/IC`,
  and `/CA`, regenerates the matching `/AP`, preserves `/BS`, `/BE`, `/LE`, and
  unrelated dictionary entries, and revalidates the indexed appearance
  fingerprint before committing.
  Highlight, Underline, Squiggly, and Strikeout accept only non-whitespace
  source-text glyphs intersected by the drag gesture, snap the result into
  per-line rectangles/QuadPoints, and reject whitespace-only or no-text
  selections. Nearly horizontal gestures use a three-pixel hit-test band. If
  glyph geometry is still loading at release, preserve the pending
  revision/page/selection/tool/color, prioritize that page's candidate scan,
  and materialize the draft automatically; Discard must cancel pending text
  markup creation. Invisible OCR text is eligible
  for hit testing but never exposed as a selectable/editable text candidate.
  Scans without a usable text layer do not support glyph-aware text markup. Current
  axis-aligned geometry accepts upright horizontal glyphs only; rotated/vertical
  glyphs require oriented QuadPoints and are ignored. Pages above the 50,000
  text-character safety cap do not cache partial glyph geometry and therefore
  do not support these four text-markup tools.
  Free Text accepts a click or drag to place a bounded text box. Its contents
  are edited through the comments panel and remain semantic `/Contents` text;
  Save writes a standard `/FreeText` annotation with a wrapped Unicode-safe
  appearance instead of converting it into page-content text.
  The comments panel shows each annotation's author and modification date,
  falling back to its creation date. Root comments accept replies; the panel
  flattens imported reply chains to one display level, saves new replies with
  standard `/IRT`, and cascades root deletion across the thread. PDF annotation
  state transitions with `/State` and `/StateModel /Review` or `/Marked` stay in
  the internal thread graph for safe deletion/flag checks but are excluded from
  the comment list and selection APIs. Each root and reply comment card has a lazily
  loaded three-dot menu with a cascading Set Status submenu and a Mark with
  Checkmark item. Status and checkmark changes are independent undoable drafts;
  Save/Save As appends author-specific `/Text` state transitions with `/IRT`,
  `/State`, and `/StateModel`, chains later transitions to the previous state
  for the same author/model when its `/NM` is safe, and restores the terminal
  state independently for its root or reply target after reopen. Reader-specific
  case variants of the current reviewer reuse the exact spelling established by
  the document, including for replies without prior state. The canonical
  exact-case history is preferred; `/IRT` depth is authoritative within it,
  while `/M` and annotation order are deterministic tie-breakers for forks.
  Reply
  annotation appearances stay hidden from the page viewer while the replies
  remain available and editable in the panel. New Sticky Notes and replies have
  vector normal appearances, and comment line endings are saved as PDF CR while
  the editor uses LF. Comments and replies are limited
  to 700 UTF-16 code units. Read-only and LockedContents source annotations
  remain protected.
- Draw rectangles, ovals, lines, arrows, triangles, and diamonds from the
  Shapes dropdown. Added shapes are vector session drafts that can be selected,
  moved, resized with eight side/corner handles, deleted, undone/redone, and
  persisted as PDF path objects. Selecting an Edit shape opens Shape Properties
  for stroke color, fill/no fill on closed shapes, and opacity; each accepted
  change is undoable. Semantic style lives in `Draft.commentProperties` with
  opacity independent from stroke/interior colors.
  New shapes use a black outline with transparent fill; near-axis lines and
  arrows snap horizontally or vertically. Newly saved top-level paths marked
  `TrePDFShape` rehydrate as `sourceShape` only after bounded canonical geometry
  and style validation. Unmarked top-level paths can also rehydrate when their
  geometry and style match a canonical rectangle, triangle, or diamond. They
  retain style/move/resize/delete, history/discard, and Save/reopen support; the first
  edited Save adds `TrePDFShape` metadata. Arbitrary and nested paths remain
  excluded, and invalid marked paths never fall back to unmarked recognition.
  These Edit shapes are page content, separate from comment annotations.
- Render every page through PDFium.
- Virtualized continuous scrolling with reusable QML delegates.
- `PageThumbnailPanel.qml` provides a collapsible, virtualized left thumbnail
  sidebar. `PDFDocumentSession::thumbnailsVisible` retains visibility per tab.
  Clicks commit panel/editor/form input and navigate the active split pane.
  Miniatures reuse `PDFPageView` at bounded dimensions with the shared raster
  budget and form worker; pooled delegates detach document/controller, and
  collapsing destroys the list. Thumbnails intentionally show saved page content
  without an Edit controller or interaction overlays; content drafts refresh
  after Save, while page structure, view rotation and runtime form values update
  live. A visible reminder explains pending content.
- Per-tab Page Display modes for Single Page, Continuous, Facing, and Continuous
  Facing, exposed through a compact dropdown. Single Page and Facing keep
  exactly one active page/spread delegate; both Continuous modes virtualize
  stacked rows and select the current page by dominant visible area. Facing
  modes alone can keep the cover page separate. A nested Split menu selects
  Vertical or Horizontal. Both Split modes create two independent
  scroll/zoom/page viewports over the same document. Edit/form interaction
  remains in the primary pane.
- Asynchronous page rendering on a worker thread.
- Page navigation, zoom, and fit-to-width controls.
- The View tab searches source-page text across the current document on a
  dedicated bounded worker. Search state is per tab; newer queries cancel older
  scans, Previous/Next wrap through matches and center the active match in the
  viewport, and visible-page matches use normalized highlight geometry with the
  active result distinguished in orange. Pending text and worker dispatch are
  owned per session, and background completion does not navigate outside View.
  Document, page-structure, or draft
  history changes clear the query/results because search reads saved source
  text. Queries are capped at 256 UTF-16 code units, retained highlight geometry
  is bounded, and incomplete scans surface a limited state.
- View supports single-page drag text selection, double-click word selection,
  Cmd/Ctrl+C, a lazy right-click Copy menu, Cmd/Ctrl+A for the focused page, and
  Escape to clear. Each viewport owns `PDFTextSelectionController` independently.
  `PDFTextSelectionItem` reads saved source Unicode and normalized character
  geometry on a bounded single worker, maps crop/intrinsic rotation through
  PDFium, and renders highlights with scene-graph geometry. Visible source
  Widgets and Links retain pointer priority. Pooling, source/page changes, leaving
  View, and edit history changes cancel stale work and clear selection. Selection
  requires saved content and PDF copy permission; scans need a text layer.
  Pages exceeding 50,000 characters are rejected without partial extraction.
  Selection does not create drafts or modify forms.
- Per-tab left/right view rotation in 90-degree steps. Rotation is a scene-graph
  view transform only: it rotates the page texture and edit overlay together
  without changing PDF page geometry, drafts, or saved content.
- Standard AcroForm widgets are interactive in Browse mode. Eligible
  fixed-choice combo boxes and list boxes use a Qt Quick overlay synchronized
  with the worker-owned PDFium form session; choice fields with editable text,
  commit-on-selection behavior, field actions, or unsafe display geometry stay
  on the native PDFium interaction path. Form values participate in the
  per-tab draft guard and Save/Save As, while PDFium does not expose undo/redo
  history for non-editable choice fields.
- The Forms tab can place Text Field, Check Box, Radio Button, fixed
  single-select Dropdown, multi-select List Box, and Comb Field widgets. Check boxes and
  radio buttons use a square click/drag tool; dropdowns and list boxes use a
  rectangular tool. Dropdowns start with three localized options, while List
  Boxes start taller with five localized options. All six support
  selection, movement, resizing, and
  transient deletion; check boxes and radio buttons can be toggled, and
  dropdown values and multiple list-box values can be selected in the designer. A selected Text Field has a
  right-side panel for enabling multi-line input and an optional maximum
  character count; both persist through Save/Save As and rehydrate after reopen.
  A selected Radio Button has a right-side panel for changing its Group ID and
  deleting the member. Radio Buttons with the same Group ID persist as one
  AcroForm radio field with child Widgets and mutually exclusive selection.
  TrePDF rehydrates radio groups, state, choice options, and choice selection
  after reopen. A TrePDF-created Text Field, Dropdown, List Box, or Radio Button can also
  be deleted after rehydrate: deletion remains a Forms-mode draft until
  Save/Save As, while Discard restores it. Text Field, Dropdown, and List Box removal is
  limited to top-level, single-widget TrePDF fields; Radio removal rebuilds the
  remaining TrePDF-created group. Source-backed Check Box and arbitrary
  AcroForm structures remain protected.
- Horizontal fill-only text inside a top-level Form with at most 16 single-child
  Form levels is editable when every node has finite nonsingular geometry, no
  marks, transparency or clipping, and safe inherited Form paint.
  `PDFSourceTextGeometry.h` validates the same structure in scan, preview and
  Save. Candidates retain leaf font/text/composed geometry but own the outer
  occurrence: move translates its matrix, delete removes it, and content/style/
  frame changes replace it at the same z-order with page-level text. Never
  mutate the shared Form stream. Other nested text remains read-only.
- Text draft editing with selection, undo, redo, discard, Save, and Save As.
  Drafts stay session-only until a save succeeds.
- Add Image uses the same placement cursor and draft history as Add Text. After
  the user clicks a page and chooses a supported local image, TrePDF snapshots
  the decoded pixels, shows a resizable draft that preserves the initial aspect
  ratio, and writes it as a PDF image object on Save/Save As.
- Top-level PDF image objects can be selected and deleted with Backspace,
  Delete, or the contextual Delete action. Deletion remains an undoable session
  draft until Save/Save As removes the source object. A top-level Form XObject
  containing only one image through at most 16 single-child Forms is also an
  image candidate: edits target the outer occurrence, never the shared stream.
  Mixed-content, marked, externally clipped or transparent outer Forms remain
  protected, as do outer Forms with non-default fill (stencil paint safety).
  Internal image masks and Form clipping stay intact.
- Added images and editable top-level source images can be dragged within the
  page. A move preserves image size and creates one undoable command. Added
  images and eligible source images also have eight side/corner resize handles
  and a Lock aspect ratio checkbox in a dedicated right-side Image Properties
  panel. The panel appears only when an image is selected in Edit and expands
  when activated, replacing the text-formatting panel. Its checkbox is disabled
  for unsupported images or while saving. Edit and Header image selection popups
  offer Copy beside Delete. Copy and Cmd/Ctrl+C put the visible image on the
  system clipboard; Cmd/Ctrl+V adds an independent image draft on the current
  primary-pane page. Text inputs keep their own clipboard shortcuts. TrePDF
  clipboard metadata preserves physical size. Images copied in Header retain
  their original position in PDF points when pasted, including across different
  page sizes; images copied in Edit use a small offset. Both are clamped to the
  destination page or the 100-point Header. Oversized Header pastes
  are proportionally reduced. Paste uses normal Undo/Redo and Save/Save As.
  Added images receive their decoded-pixel fingerprint immediately. Header
  copies of upright, unclipped top-level images preserve intrinsic pixels using
  the same bounded decoder as Header matching, so a pasted logo can match its
  source before Save and after reopening without resampling differences.
  `PDFEditControllerClipboard.cpp` snapshots eligible source images, including
  image-only Forms, on a private worker document and respects PDF copy permission.
  The lock defaults to enabled and is transient per-tab controller state; toggling it creates no
  draft/history entry and writes no PDF property. Each resize is one undoable
  geometry command. Source resize requires a finite, nonsingular image matrix,
  no custom clipping, and the entire image inside the visible page. Unsupported
  top-level images keep their existing move/delete support; images in unsupported
  Form structures remain protected. Image-only Form occurrences use the same
  move/resize/rotate/delete history and preserve other uses on any page.
  Source preview transforms run on the render worker, and
  Save/Save As composes the original matrix with the source-to-target frame
  mapping so pixels, original rotation/reflection, and z-order remain intact.
  The panel also provides Rotate left/right 90° for
  added images and eligible source images. Rotation preserves physical size,
  swaps frame width/height, keeps the center when possible, and shifts the frame
  within the page. Disable quarter turns when the rotated frame cannot fit
  without scaling. `Draft.imageQuarterTurns` tracks clockwise rotation relative
  to the source; each click participates in undo/redo/discard and Save/Save As.
  The worker and writer share the source-matrix rotation mapping, while added
  image preview rotates its snapshotted pixels through the painter. Rotation
  requires the same source eligibility as resizing, even for square/180° cases.
- Added text and editable top-level source text can be dragged by the selected
  bounding box or arrow keys. A move is one undoable geometry command. When
  source text is only moved, preview and Save/Save As translate every original
  text-object matrix so its font, glyph encoding, paint, and z-order remain
  intact; a content, format, or frame-size change still uses text reflow and
  source-object replacement.
- Drafts use a rich-text model with UTF-16 text ranges, character spans,
  paragraph formats, a layout frame, and content bounds. The inline `TextEdit`
  supports multiple lines, wraps/reflows inside the layout frame, renders every
  character span, and applies formatting to the selected text range.
- Editable geometry separates immutable source bounds, the draft layout frame,
  and laid-out content bounds. The frame and content bounds participate in
  draft comparison and undo/redo. Existing PDF text seeds the initial frame
  from the Qt width of its longest original line so each line fits before
  reflow. On entering text edit, a transient frame extends from the existing
  left edge to the next same-row text boundary or the inferred right page margin. It is persisted
  only by a real text edit, a font-size or paragraph-format change, or explicit
  resize, so a no-op editor session does not create a draft. Four edge resize handles update
  the normalized frame, reflow text live, and commit geometry through the same
  draft undo/redo snapshots as text and formatting.
- Nearby same-line text objects are grouped across font families, bold, italic,
  underline, and calibrated stretch differences. Comparable size and identical
  paint are still required; every source character span retains its own format.
  Same-line groups are bounded to 1,024 parts and 32,768 UTF-16 code units and
  cannot bridge indexed source obstacles. Rich-text saves persist validated
  `TrePDFParagraphLayout` metadata so mixed faces reopen as one editable group
  without inferring joins or repeatedly recalibrating their stretch.
  Matching nearby lines become one paragraph draft when font family/size,
  paint, left origin (within
  0.4 em), baseline gaps (0.8–1.6 em), and regular leading agree. Bold, italic,
  underline, and font-stretch differences of at most two percentage points
  (or both stretches within 98–102%) are preserved in source character spans.
  A first-line inset of 0.75–2.5 em can join body lines at a common left margin;
  the opposite transition starts a separate paragraph. Short inset headings,
  excessive offsets, overlapping text and source obstacles remain excluded.
  `firstLineIndentRatio` retains that inset through editor, preview, history,
  Justify and Save/reopen; the initial frame includes its width.
  Restored single-line paragraph metadata can join adjacent lines only with
  default left-aligned paragraph formatting and all source parts on one baseline.
  Wrapped or explicitly formatted restored paragraphs remain separate.
  Source text with overlapping bounds stays separate; grouping must not cross
  columns or source obstacles.
  Paragraphs retain `\n`, the body left margin and first line's baseline, and every flattened
  source part for rendering suppression and Save. Each paragraph is bounded to
  64 lines, 1,024 parts, and 32,768 UTF-16 code units; overlap and paragraph-link
  checks share a 200,000-comparison budget with source/line fallback. Content
  edits use the existing Qt reflow; move-only drafts translate every original
  text matrix and preserve the source line layout.
  Justify joins tracked source line breaks into spaces within the selected
  logical paragraph so Qt can wrap and justify it. Explicit user-entered
  paragraph breaks remain separate. Source-break positions follow text edits
  and draft snapshots. Saving justified lines positions words independently
  to retain their spacing without stretching glyphs; the final line stays left aligned.
  Eligible horizontal Latin/Common source runs also retain per-cluster advance
  corrections when uniform character/word spacing cannot represent their PDF
  positions. Precomposed Vietnamese can qualify; combining marks, multi-character
  ligatures, complex shaping, and mismatched Qt cluster boundaries use the
  existing layout fallback. Calibration is bounded to 1,024 positions, three
  passes, finite corrections within 512 PDF points, and 0.08-point origin error.
  Keep `sourceAdvanceRatio` separate from user character/word spacing; editor,
  preview, and writer use the same effective advances. Text edits clear it for
  changed clusters and the preceding boundary while retaining unaffected spans.
  Font family/size, bold/italic, and spacing changes clear it in the changed
  range; color/underline retain it. QTextDocument custom properties preserve
  it through local undo/redo. `TrePDFTextSpacing` version 2 persists the
  correction, reads version 1 as zero, and combines with paragraph metadata to
  avoid recalibration after reopen. Retained Latin/Common paragraph layouts
  position whole glyph/grapheme clusters even at zero tracking to preserve Qt
  kerning and font advances. Simple same-face characters share one text object
  with `FPDFText_SetPositions()` to preserve extraction without singleton-object
  line-break heuristics; limits remain 1,024 clusters per line and 32,768 parts per
  draft. A failed save retains both the destination and drafts. Joined source
  objects keep their original origins, or remain separate if validation fails.
  This is bounded source-layout preservation,
  not a general PDF glyph-positioning engine. See `docs/text-editing-pipeline.md`.
- Draft fonts resolve through embedded PDF faces, system fonts, the private app
  cache, Google Fonts, then the packaged CTAN JSON catalogue. Fonts under
  `src/assets/fonts` remain available as local fallback. Opening the inline
  editor first checks every selected text span's family/style/glyph coverage.
  Missing faces use a single consent popup with Cancel, Use replacement font,
  and OK to download before resuming the same selection. The replacement option
  names a verified local family and changes only missing faces for both editing
  and Save, with normal Escape/Undo semantics and no network request. If no
  local family supports all required styles/glyphs, replacement is unavailable.
  Cancellation or session changes abandon the pending editor without creating
  drafts. Usable source faces remain preferred,
  while substitute previews require an exact local face. Save retains its font
  recovery for newly entered glyphs/styles. Download failures open a modal notice
  naming the font and failed Regular/Bold/Italic/Bold Italic face. Validated
  provider metadata can identify an unavailable style and list available styles;
  lookup, network and glyph/validation failures retain their distinct reasons.
  Failed preparation leaves text unchanged; failed Save retains drafts and the
  destination. See `docs/font-providers.md`.
- Contextual Edit/Delete actions when selecting editable text; deletion remains
  an undoable draft until Save writes it into the PDF.
- Transactional PDF persistence through a separate PDFium document handle.
  Save overwrites atomically through `QSaveFile`; Save As leaves the source
  untouched. Drafts and history are cleared only after the saved file reopens.
- An edit-only, collapsible right-side text-formatting panel provides UI for
  font family, size, color, bold, italic, and underline. Materialized character
  formatting participates in draft undo/redo. Font size accepts integral PDF
  points from 1 to 512, formats the selected object or active text range, and
  sets a transient typing size at a collapsed caret. Mixed sizes show a blank
  panel value. Store semantic size ratios in the live QTextDocument so local
  undo/redo restores exact source sizes independently of zoom; merge size and
  calibrated stretch without replacing the editor document. Inline text and
  draft previews lay out in PDF points, then scale as a whole with page zoom,
  preserving wrap, baseline, and font-size behavior at fractional zoom levels.
  Commit the active editor before changing primary-pane zoom. During inline
  editing, font family, color and bold/italic/underline apply to the
  selected range; with only a caret they become the typing format for
  subsequently inserted text without changing existing characters. Invoking
  font family or a bold/italic/underline control on a selected text object opens its inline editor first
  and leaves the caret at the end, so it never restyles the whole object. The
  editor merges range formatting directly into its text document and syncs
  color, B/I/U and size back after local undo/redo without replacing the rich-text document.
  The font picker filters system faces by the active bold/italic style using a
  lightweight save-compatibility probe; filtered PDF source fonts remain
  visible as the current value only.
  Its single dropdown has Vietnamese, English, Chinese, Japanese, Korean, and
  Symbol filters, also available for Header text. Symbol includes symbol-only
  writing systems and named symbol/dingbat/math/icon/ornament families, including
  Wingdings and Webdings, without requiring Latin coverage. All groups narrow
  the same save-compatible exact-face list,
  intersecting every regular/bold/italic face used by a mixed-style selection.
- ID-based localization with Vietnamese as the default UI language.
- A restrained orange UI inspired by Foxit Reader.

## Repository Layout

- `CMakeLists.txt`: application target, QML module, translation pipeline,
  Qt/PDFium discovery, and macOS PDFium bundling.
- `cmake/FindPDFium.cmake`: creates the `PDFium::PDFium` imported target.
- `cmake/FindQPDF.cmake`: creates the `QPDF::QPDF` imported target from the
  prebuilt package, with an optional `QPDF_ROOT` override.
- `i18n/qml_vi.ts`: Vietnamese ID-based translation catalog.
- `src/main.cpp`: Qt application startup, default UI language, and QML engine
  loading.
- `src/ApplicationFonts.h/.cpp`: bundled-font registration and the separate
  bundled/system family catalogs used by draft font resolution.
- `src/DraftImageItem.h/.cpp`: QML item that draws the snapshotted pixels of an
  inserted-image draft without reopening the selected file path.
- `src/SignatureLibrary.h/.cpp`: bounded transparent signature/initial/date/mark
  snapshots, local atomic library storage, and placement through added-image drafts.
- `src/Components/SignatureDialog.qml`: draw/type/import/saved-library signature
  creation plus date and check/cross previews; `SignRibbon.qml` exposes these actions.
- `src/Components/ProtectRibbon.qml` and `SetPasswordDialog.qml`: document
  protection state, pending password entry and Save/Save As actions.
- `src/Main.qml`: multi-tab coordination, menus, toolbar, and active workspace.
- `src/PDFTabModel.h/.cpp`: per-tab document/edit sessions, active-tab state,
  duplicate-path protection, and cross-tab save guards.
- `src/PDFDocument.h/.cpp`: main-thread PDFium document, metadata, page sizes,
  navigation state, and source file path.
- `src/PDFAnnotationUtils.h/.cpp`: shared bounded annotation-string access,
  comment newline normalization, markup classification, and reply-relation
  predicates used by indexing, rendering, and saving.
- `src/PDFEditController.h`: the shared class declaration, Qt/QML API and state
  for editable-content discovery, selection, rich-text drafts, history and save.
  Its implementation is split by responsibility while retaining one controller
  and the same ownership and behavior:
  `PDFEditController.cpp` (lifecycle, session connections, tools and selection),
  `PDFEditControllerCandidates.cpp` (candidate cache/async scan, source text and
  fonts), `PDFEditControllerAnnotationScan.cpp` (source annotation indexing and
  thread state), `PDFEditControllerText.cpp` (inline editor and rich-text
  formatting), `PDFEditControllerComments.cpp` (comment creation, markup,
  replies, state and style), `PDFEditControllerGeometry.cpp` (adding
  text/image/shape, page operations and move/resize),
  `PDFEditControllerDrafts.cpp` (history, snapshots and draft presentation/render),
  and `PDFEditControllerSave.cpp` (sync/async save orchestration).
  `PDFEditControllerSupport.cpp` and private `PDFEditController_p.h` hold shared
  implementation helpers.
- `src/PDFSearchController.h/.cpp`: asynchronous whole-document text search,
  cancellation/stale-result validation, per-page normalized match geometry, and
  cyclic result navigation.
- `src/PDFFormController.h/.cpp`: per-tab AcroForm state, asynchronous form
  interaction, host actions, focus state, draft guards, and choice-field
  requests.
- `src/PDFFormWorker.h/.cpp`: serialized worker-owned PDFium form sessions,
  native widget events, choice-field inspection/mutation, form rendering, and
  save snapshots.
- `src/PDFFormView.h/.cpp`: QQuickItem bridge for native pointer, keyboard, and
  IME events plus the asynchronous choice-field model exposed to QML.
- `src/PDFSaveWriter.h/.cpp`: resolves source object paths, reflows rich-text
  runs, embeds/reuses fonts, creates added image objects, regenerates changed
  page content, and writes the PDF transactionally.
- `src/PDFPasswordWriter.h/.cpp`: C++23 adapter to qpdf's C API; encrypts a
  completed plaintext staging PDF or replaces an unrestricted source PDF's
  opening password using its authenticated session password. The prebuilt qpdf
  dylib uses native crypto with OS secure randomness and contains its static
  JPEG dependency; its runtime dependencies are system libraries only. No qpdf
  executable is invoked.
- `src/PDFSourceImageGeometry.h`: shared source-image resize eligibility and
  affine frame mapping for the candidate scanner, render preview, and writer.
- `src/PDFImageFormResources.h/.cpp`: bounded qpdf resource restoration after
  image-only Form edits, retaining inherited resource names pruned by PDFium
  without overwriting new resources or changing shared page dictionaries.
- `src/PDFStampPresets.h`: shared 24-preset catalogue, dynamic author/date
  formatting, and vector artwork for QML Stamp previews and saved appearances.
- `src/PDFCommentAnnotation.h`: shared drawing-comment geometry/property
  contract used by controller snapshots, writer, and the `/AP` canonicalizer.
- `src/PDFShapeUtils.h`: shared canonical Edit-shape path builder and bounded
  `TrePDFShape` metadata/geometry/style validator used by scanning and saving.
- `src/PDFPageItem.h/.cpp`: asynchronous rendering and scene graph texture
  upload for one page delegate.
- `src/Components/FormChoiceOverlay.qml`: custom combo-box and list-box
  presentation for the safe subset of AcroForm choice fields.
- `src/Components/SearchRibbonControl.qml`: View-tab query, status, previous,
  next, and clear controls.
- `src/Components/SearchHighlightLayer.qml`: non-interactive per-page search
  highlights inside the shared rotated page surface.
- `src/Components/DrawingCommentPropertiesPanel.qml`: dedicated Edit-tab panel
  for stroke, optional interior color, and opacity of the selected drawing
  comment or Edit shape through the shared `selectedDrawing*` API, with distinct
  panel titles; comment/thread content remains in `CommentPanel.qml`.
- `src/Components/ImagePropertiesPanel.qml`: dedicated Edit-tab panel shown for
  a selected source or added image, with the transient Lock aspect ratio option.
- `src/Components/ShapeDraftView.qml`: shared Qt Quick Shapes preview used by
  the ribbon palette, drag preview, and added/source-shape draft delegates.
- `src/Components/CommentDrawingView.qml`: vector preview shared by Square,
  Circle, Line, Polygon, and PolyLine comment drafts and gestures.
- `src/Components/InkDrawingView.qml`: Qt Quick Shapes freehand preview shared
  by the active Pencil gesture and Ink draft delegates.
- `src/Components/SquigglyLine.qml`: point-scaled Squiggly renderer shared by
  the drag preview and draft delegates, matching PDFium's saved appearance.
- `src/Components`: ribbon controls, document view, page delegates, and the
  draft editing overlay.
- `third_party/pdfium`: prebuilt arm64 PDFium headers and library. Treat this as
  third-party code and do not modify it for normal feature work.
- `third_party/qpdf`: prebuilt Release macOS arm64 qpdf 12.4.1 dylib, public
  headers, version, checksums, and upstream notices; requires macOS 13 or newer.
  Normal builds use this package without compiling or downloading qpdf sources.
- `scripts/build-qpdf-macos.sh`: separate dependency rebuild script for upgrades;
  accepts a Qt prefix and optional output directory. This dependency build requires
  at least C++20 and the Qt SDK's static JPEG library. See the package README for tool
  overrides and verified offline source archives.

## Local Toolchain

This project is configured for the following local SDK:

- Qt prefix: `/Users/luongvanhuy/Qt/6.12.0/macos`
- `Qt6_DIR`: `/Users/luongvanhuy/Qt/6.12.0/macos/lib/cmake/Qt6`
- `qmake`: `/Users/luongvanhuy/Qt/6.12.0/macos/bin/qmake`
- PDFium root: `<repo>/third_party/pdfium`
- qpdf root: `<repo>/third_party/qpdf` (override with `QPDF_ROOT`)
- CLion: `/Applications/CLion.app`
- Default build directory: `<repo>/cmake-build-debug`

Application and test targets require C++23 with compiler extensions disabled.
The iOS Objective-C++ bridge uses the same standard. Use a compiler and
standard library that support the C++23 features used by the project; CMake
must fail rather than silently select an older language standard.

There is intentionally no `CMakePresets.json`. Do not add one unless the user
explicitly requests it. `CMakeLists.txt` already contains local fallback paths
for Qt, PDFium, and qpdf.

## CLion

Open this directory in CLion:

`/Users/luongvanhuy/Documents/TreOffice/TrePDF`

Use the default `Debug` CMake profile and run the `trepdf` target. The bundled
CLion CMake and Ninja work with this project without a preset.

Equivalent configure command:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_MAKE_PROGRAM=/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja \
  -G Ninja \
  -S /Users/luongvanhuy/Documents/TreOffice/TrePDF \
  -B /Users/luongvanhuy/Documents/TreOffice/TrePDF/cmake-build-debug
```

Build command:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build /Users/luongvanhuy/Documents/TreOffice/TrePDF/cmake-build-debug \
  --target trepdf -j 8
```

## Localization

- Use Qt's ID-based translation system exclusively. Do not introduce new
  `qsTr()` or `QObject::tr()` calls.
- Every user-visible QML string uses `qsTrId("PascalCaseKey")`; every
  user-visible C++ string uses `qtTrId("PascalCaseKey")`.
- Translation IDs are English, globally unique, and PascalCase. Treat an ID as
  a stable API: changing it creates a new translation entry.
- Put an English engineering source comment (`//% "Source text"`) immediately
  before every `qsTrId()` or `qtTrId()` call so `lupdate` can preserve readable
  source text.
- Translation functions take literal IDs only. Do not pass runtime variables to
  `qsTrId()` or `qtTrId()` unless the literal ID was separately marked with
  `QT_TRID_NOOP`.
- Use `%1`, `%2`, or `%n` placeholders instead of concatenating translated
  fragments. Preserve every placeholder in all translations.
- Vietnamese is currently the default language. `src/main.cpp` sets the QML
  engine UI language to `vi`, and the catalog lives at `i18n/qml_vi.ts`.
- Translation files must use the `qml_<locale>.ts` naming scheme so
  `QQmlApplicationEngine` can load them automatically.
- Keep the translation resource prefix at `/qt/qml/TrePDF/src/i18n` while
  `Main.qml` is packaged as `src/Main.qml`. The engine searches for `i18n`
  adjacent to the main QML file.
- Add a locale to `I18N_TRANSLATED_LANGUAGES` in `CMakeLists.txt` before adding
  its `i18n/qml_<locale>.ts` catalog.

After changing any user-visible text, update and validate translations:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build /Users/luongvanhuy/Documents/TreOffice/TrePDF/cmake-build-debug \
  --target update_translations -j 8
```

Do not leave new entries as `type="unfinished"`. Build `trepdf`, run the
`trepdf_qmllint` target, and visually check that longer translations are not
clipped or elided unintentionally.

## Rendering Architecture

### PDFDocument

- `PDFDocument` lives on the GUI thread and owns the main `FPDF_DOCUMENT`.
- Open local files with `FPDF_LoadDocument()` using their path.
- Never restore the old `QFile::readAll()` plus `FPDF_LoadMemDocument()` design.
  Large scanned books made that design copy the entire PDF into process memory.
- Do not pass the main `FPDF_DOCUMENT` to a worker thread.
- Page dimensions are cached so hot QML bindings do not repeatedly query
  PDFium.
- Increment the document revision whenever the underlying document changes.
  Async results use this revision to reject stale work.

### Async Worker

- Rendering uses one `QThreadPool` worker to bound decoder memory and avoid
  concurrent access to the same PDFium document.
- Document search uses its own single-thread pool and opens a separate PDFium
  document from `renderFilePath()`. Coalesce queued work per controller so only
  the newest pending snapshot remains. Keep each PDFium transaction under the
  process-wide lock, recheck cancellation after acquiring that lock, and
  validate controller, path, document revision, query, and cancellation token
  before accepting results.
- The rendering worker owns a separate thread-local `FPDF_DOCUMENT` opened from
  the file path. It must never use the GUI thread's document handle.
- Each request has an atomic cancellation flag. Check it before expensive work
  and before posting the rendered image back to the GUI thread.
- Deliver results with a queued invocation and guard the target with
  `QPointer<PDFPageItem>`.
- Treat QML zoom as visual geometry, not a separate render-content key. Preserve
  and scene-graph-scale the accepted texture while zoom input changes, then
  request the final raster after a short settle interval.
- Reject a result when its request id/serial, document pointer, revision, page,
  device pixel ratio, or raster pixel size no longer matches.
- Hybrid rendering uses full-page rasters up to 32 MiB/4096 physical pixels per
  edge, with a 24 MiB return threshold. Larger pages use a full-page preview
  (at most 4 MiB/2048 pixels per edge) and 1024-pixel visible tiles with a
  two-pixel gutter. Plan at most 64 tiles, visible first, with 128-pixel preload.
  Keep the full-page raster transform separate from the tile bitmap size and
  origin, including FPDF_FFLDraw and widget suppression patches.
- Dispatch one preview/tile per queue turn under the same owner/generation;
  do not enqueue each tile as an independent same-owner render or monopolize
  the form worker with a whole-page batch. Accept a new-revision full-page
  preview before detail tiles so source/draft suppression never mixes revisions.
- Keep source-text glyph discovery for text markup inside the asynchronous page
  candidate scan. Return and cache only normalized glyph geometry on the GUI
  side; QML and GUI-thread controller paths must never open or inspect PDFium
  page/text handles for hit testing.
- Preserve invisible OCR text as internal text-markup hit-test geometry without
  surfacing it through normal candidate selection/editing. Cache only
  non-whitespace, upright horizontal glyphs. If a page reports more than 50,000
  text characters, cache no per-glyph text-markup geometry for that page rather
  than keeping a partial result; normal source-candidate scanning remains
  unchanged.

### Scene Graph Texture Ownership

- `PDFPageItem` derives from `QQuickItem`, sets `ItemHasContents`, and uploads
  completed `QImage` data in `updatePaintNode()`.
- Scene graph node and texture operations belong in `updatePaintNode()` on the
  render thread.
- Use `QSGSimpleTextureNode::setOwnsTexture(true)`. With ownership enabled,
  `setTexture()` deletes the previous texture and the node destructor deletes
  the final texture.
- Never call `QSGSimpleTextureNode::setTexture(nullptr)`. Qt 6.11 dereferences
  the argument; this previously caused an `EXC_BAD_ACCESS` crash on
  `QSGRenderThread` with the Metal backend.
- Do not manually delete an owned texture.
- If the node or texture is lost, invalidate the accepted-raster token during
  scene-graph sync, then use a guarded queued callback to clear GUI readiness
  state and schedule recovery; never emit the QML-facing signals directly from
  the render thread.
- Clear the CPU-side pending `QImage` immediately after creating the scene graph
  texture. Keeping both copies defeats the memory optimization.
- The page node contains owned base/tile texture children. Raster-budget leases
  stay charged until actual scene-graph texture destruction, not GUI eviction.
- Use identity `QSGTransformNode` containers for the page and each raster child;
  incremental software-backend updates must retain the inherited page transform.

### QML Virtualization

- The document view is a `ListView` with `reuseItems: true`.
- Keep delegates small and reset pooled state through `ListView.onPooled` and
  `ListView.onReused`.
- Pooled page delegates detach their `PDFDocument`; this cancels stale rendering
  and releases their page texture.
- Keep `cacheBuffer` conservative. Raising it renders and retains more full-page
  textures and can quickly consume hundreds of megabytes on Retina displays.
- `clip: true` is intentional for the scrolling viewport.
- Keep QML imports versionless and use `QtQuick.Controls.Basic` when customizing
  control internals.
- Keep view rotation on the shared page-surface container so `PDFPageView` and
  `EditOverlay` receive the same transform. Do not feed view rotation back into
  `PDFDocument::pageSize()` or save-time normalized coordinates.
- Keep `SearchHighlightLayer` in that same page surface, below the interaction
  coordinator. It is presentation-only, reads normalized rectangles from the
  session search controller, and must not capture pointer or keyboard input.
- Wrap user-visible text in `qsTrId()` using the localization rules above.

### Form Interaction Architecture

- `PDFFormWorker` owns the form-session `FPDF_DOCUMENT` and `FPDF_FORMHANDLE`
  on a serialized single-worker queue. Keep all PDFium form access under the
  process-wide PDFium lock; GUI-thread controllers and QML must never access
  those handles directly.
- `PDFFormView` remains the native pointer, keyboard, and IME bridge. It also
  requests choice-field snapshots asynchronously and refreshes them after form
  render-state, page, document, or interactivity changes. Reject stale results
  using the controller generation, document key, and current source-page
  mapping.
- `FormChoiceOverlay` may replace the presentation only for visible Widget
  annotations that are fixed-choice combo boxes or list boxes with valid
  geometry and bounded option data. Editable combo boxes, fields with
  commit-on-selection, `/A` or `/AA` actions, and controls too small for safe
  custom presentation must remain on PDFium's native path. Read-only fields may
  be shown by the overlay but must remain disabled.
- Before focusing or changing a choice, resolve the current source page and
  annotation again and validate its identity token. Never trust an annotation
  index alone after page insertion, deletion, document reopen, or an async
  refresh. Resolve rapid multi-select toggles against live worker state rather
  than a stale QML snapshot.
- Keep `PDFFormView`, `FormChoiceOverlay`, the page texture, and `EditOverlay`
  inside the same rotated page-surface container. Constrain combo popups to the
  visible `ListView` viewport and clear pending/open popup state when the page,
  document, interaction mode, or delegate lifetime changes.
- Choice mutations use the existing per-tab form draft guard and snapshot-based
  Save/Save As workflow. PDFium does not expose undo/redo for non-editable
  choice fields; do not advertise or synthesize that history without a separate
  design that preserves PDF form focus and action semantics.

### PDF Save Architecture

- Never modify or save the GUI thread's main `FPDF_DOCUMENT` directly. Open a
  separate writer document from `PDFDocument::renderFilePath()`. Snapshot all
  draft/font/geometry data on the GUI thread, then run `PDFSaveWriter::save()`
  on a worker; the worker request must not retain controller/document pointers.
- Resolve every source object path before removing source objects. Only
  top-level page text plus top-level image movement/resizing/rotation/deletion
  are currently saveable, including validated image-only and single-text Form
  occurrences. Revalidate wrappers with `PDFSourceImageGeometry::isImageOnlyForm()`
  or `PDFSourceTextGeometry::textObject()`; text font lookup uses the leaf while
  mutation retains the outer root path. Restore missing inherited page resources
  for retained image/text Forms after PDFium serialization;
  transform/remove only the top-level occurrence, never its child objects or
  shared Form stream. Arbitrary nested Form content remains non-editable.
  Keep moved/resized/rotated source
  images in place and compose their existing matrix with the source-to-target
  normalized frame transform; never decode/recreate them. Use the shared
  `PDFSourceImageGeometry` mapping so CropBox offsets and intrinsic page rotation
  agree between preview and save. Revalidate resize/rotation eligibility in the
  writer:
  custom clipping, singular/nonfinite matrices, and partially off-page source
  images cannot be resized or rotated. Pure movement only translates the original
  matrix.
  Apply the same matrix-only path to editable
  top-level source text when position is its only change; any text, character
  format, paragraph format, or frame-size change must follow the existing
  remove-and-reflow path.
  Remove source objects with `FPDFPage_RemoveObject()` and destroy each object
  only after a successful removal.
- PDFium does not wrap text. Reflow the draft with `QTextLayout`, create text
  objects per line/style run, convert Qt top-left coordinates to PDF bottom-left
  coordinates, and represent underline as a path.
- Preserve the exact source horizontal text scale when a run reuses its original
  PDF font and the user has not reset its stretch. Compute Qt-to-PDFium width
  compensation only for newly embedded or standard fonts; after reopen, reuse
  that saved scale instead of recomputing it so repeated saves cannot compound
  the correction. Treat draft `fontStretch` as the target PDF ratio, not a
  direct `QFont::setStretch()` value: preview base fonts, rich-text spans, and
  save-time `QTextLayout` must share the measured stretch calibration, while
  the PDF matrix continues to use the exact source scale.
- Keep loaded font handles alive until all changed pages have called
  `FPDFPage_GenerateContent()` and the document has been serialized.
- Write through `QSaveFile` and call `FPDF_SaveAsCopy()` with
  `FPDF_NO_INCREMENTAL`. Preserve shaping/kerning tables in newly embedded
  programs: PDFium's subset flag strips these and changes layout after reopen
  with embedded-first resolution. Original PDF font resources stay unchanged.
  Commit only after PDFium succeeds;
  any earlier error must leave the destination and in-memory drafts untouched.
- Keep a pending opening password outside content undo snapshots but inside
  the draft/close guard. Snapshot it into the worker request without exposing a
  password getter to QML. Revalidate that encrypted sources have no permission
  restrictions before replacing their password. Password-only changes use qpdf
  directly on the source with the authenticated session password. When edits
  accompany a replacement password, add `FPDF_REMOVE_SECURITY` only to staging
  serialization, complete plaintext canonicalization, then encrypt a separate
  temporary file. Ordinary encrypted saves must retain source encryption.
  Validate the replacement with PDFium using the new password and expected page
  count, and confirm passwordless opening fails before committing. Reopen with
  the new password; only successful reopen clears pending state.
- Persist Highlight, Squiggly, Underline, Strikeout, Free Text, and Stamp drafts as
  their matching `FPDF_ANNOT_*` annotations; persist Square, Circle, Line,
  Polygon, PolyLine, and Ink through the subtype-safe drawing placeholder
  pipeline. Adding annotations alone must not call
  `FPDFPage_GenerateContent()`; mixed text/image edits still regenerate the
  affected page content normally. Highlight, Squiggly, Underline, and Strikeout
  selections carry snapped rectangle/QuadPoints entries grouped by matched text
  line. Free Text
  must keep `/Contents`, `/DA`, comment metadata, and a wrapped Unicode-capable
  `/AP`; editing a source Free Text annotation must replace its stale
  appearance. New Stamp annotations keep their preset `/Name`, semantic
  `/Contents`, `/C`, Print flag, and vector `/AP` from `PDFStampPresets.h`.
  Dynamic Stamp author/date artwork is fixed at placement, independent of
  `/Contents`; source rehydration derives its detail from `/T` and
  `/CreationDate`. Editing source Stamp contents must preserve its geometry
  and existing `/AP`. Square,
  Circle, Line, Polygon, and PolyLine use `/L` or
  `/Vertices` where applicable plus a common
  `/C`, `/IC`, `/CA`, `/BS`, `/BE`, `/IT`, `/Name`, `/LE`, `/F`, and vector
  `/AP` framework. PDFium creates only a staging placeholder; the canonicalizer
  writes the final subtype, dictionary, and appearance atomically. For Ink, the
  writer maps every normalized stroke through `FPDF_DeviceToPage()` and the
  canonicalizer writes `/Subtype /Ink`, `/InkList`, `/BS`, `/C`, `/CA`, the
  Print flag, and a vector `/AP` with round line caps and joins. All six drawing
  subtypes support source content edits and geometry changes, delete, reply,
  undo/redo/discard, Save/Save As, and reopen without regenerating page content
  on annotation-only pages. Source geometry updates patch `/Rect` plus `/L`,
  `/Vertices`, or `/InkList` after PDFium serialization while preserving
  unrelated dictionary entries. Translation keeps the original `/AP`; resize
  regenerates a matching normal appearance only for a provenance-validated
  TrePDF drawing comment with fully supported captured properties, while
  external/custom appearances remain intact.
- Persist Edit shapes as top-level PDF path objects marked with `TrePDFShape`.
  Use `PDFShapeUtils.h` for the same canonical path and metadata validation in
  scanner and writer. Validated marked paths and canonical rectangle, triangle,
  or diamond paths with no marks can become `sourceShape`, only at the page's
  top level.
  Invalid marked paths must never fall back to unmarked recognition.
  Keep semantic stroke/interior colors and opacity separate in
  `Draft.commentProperties`, combining alpha only for preview and serialization.
  Revalidate the captured source fingerprint, geometry, and style before
  rewriting or deleting; add `TrePDFShape` metadata to edited unmarked shapes;
  rebuild changed source shapes at their original z-order. Convert every path point
  through `FPDF_DeviceToPage()` so CropBox offsets and rotation match the
  editor; keep ovals vector by using cubic Bézier segments and retain
  insertion order with the other added page objects.
- Place replacement text runs with `FPDFPage_InsertObjectAtIndex()` after proven
  interleaved line backgrounds while preserving foreground paint. Keep generated
  underlines with their run and account for other drafts' effective geometry.
  Validate finite bounds and bound comparisons; reject conflicting paint-order
  constraints before commit. Translate original insertion boundaries after all
  source removals so multiple drafts retain their order. Added text and images
  are appended in insertion order. Convert coordinates
  with `FPDF_DeviceToPage()` so CropBox offsets and page rotation follow the
  same mapping as the editor.
- Added images are decoded and size-limited on the GUI thread, then snapshotted
  into the save request. Create a PDFium-owned BGRA bitmap and keep it alive
  through `FPDFPage_GenerateContent()`; never make the save worker reopen the
  selected image path.
- Reopen the saved path through `PDFDocument::open()`. A successful reopen
  changes the document revision and resets drafts/undo/redo through the normal
  document-state signals. If reopen unexpectedly fails after commit, close the
  stale in-memory document because its source object paths no longer match the
  file now on disk.
- Disable document editing, Open, tab switching/closing, and window close while
  an async save is active. Resume a pending tab close or Quit action only after
  the matching tab emits `saveFinished(true)`.
- Exact bundled font faces, reusable original PDF fonts, PDF standard fonts,
  and eligible macOS system TrueType faces are supported. Resolve system faces
  with CoreText on the GUI thread and snapshot their bytes into the save
  request. Materialize TTC faces as standalone sfnt data before giving them to
  PDFium. Accept only static `glyf/loca` faces up to 64 MiB whose OS/2 license
  permits Installable or Editable embedding; reject CFF/CFF2, variable,
  color or bitmap-only, missing-glyph, Restricted, and Preview & Print-only
  faces rather than silently substituting them. Filter the font picker with a
  lightweight exact-face CoreText probe, but keep content-specific glyph and
  full-size validation in the save writer. CoreText coverage checks also accept
  legacy symbol bytes only when the exact face's glyph mapper resolves them.
  `PDFSymbolFont.h` repairs common Unicode bullet mappings missing from bounded
  embedded legacy-symbol cmaps by uniquely matching the PDF outline to the same
  program's glyph. Candidate scan and Save share the repaired snapshot; ambiguous
  or unsupported mappings remain unchanged. Inferred spaces after bullets prefer
  the following text face when the bullet subset lacks a space.

## Memory and Performance Invariants

- Memory must scale with visible and near-visible pages, not total page count.
- Never store rendered images for every page.
- Never queue unlimited obsolete render jobs.
- Do not duplicate a full-page image in both a long-lived CPU cache and a
  `QQuickPaintedItem` backing store.
- Preserve native device-pixel-ratio quality unless the user explicitly accepts
  lower-quality rendering.
- `PDFRasterBudget` caps estimated app-owned raster bytes at 256 MiB, reserving
  twice the image bytes before allocation for CPU/texture overlap. This does
  not cap PDFium decoder or Qt/driver staging memory. Keep 16 MiB headroom for
  replacement previews; full-page items may switch to tiled under pressure.
- Evict nonvisible raster entries by LRU, retaining at most 32 MiB of charged
  nonvisible tile leases per item within the shared cap. Never block or poll
  the form worker for capacity: retain fallback coverage and retry on actual
  release or new input. Finite budgets may temporarily leave visible detail
  at preview resolution when all remaining resident content is pinned.

## Coding Conventions

- Use PascalCase for C++ class and QML component names.
- Use camelCase for methods, properties, and local variables.
- Prefix private C++ data members with `m_`.
- Prefer Qt ownership types and RAII. Document any cross-thread lifetime that is
  not obvious.
- Keep edits scoped. Avoid unrelated refactors and generated-file churn.
- Add new C++ and QML files to `qt_add_qml_module()` in `CMakeLists.txt`.
- Do not edit files under `cmake-build-debug` or other build directories.

## Verification

For every rendering or lifecycle change:

1. Build the `trepdf` target with the command above.
2. Run `git diff --check`.
3. Open a multi-page PDF and verify first-page rendering.
4. Scroll rapidly in both directions to exercise delegate pooling and request
   cancellation.
5. Change zoom repeatedly.
6. Close and reopen the document while rendering is active.
7. Quit normally and confirm there is no render-thread crash.

Software/offscreen tests are useful but do not cover the macOS Metal lifecycle.
Texture ownership changes must also be tested with the native Metal backend.
Do not commit temporary auto-open paths, stress-test `Timer` blocks, or local PDF
paths added only for verification.

## Known Test Result

The current texture ownership fix was tested on the Metal backend with a
231-page PDF, 450 random page jumps, and two close/reopen cycles. The process
exited normally. This is a regression scenario worth repeating after changes to
`PDFPageItem`.

Hybrid rendering was verified on 2026-09-08 with 341 passing offscreen tests and
native Metal tests for tile reuse, zoom settling, budget recovery, and atomic
comment suppression after delete/undo/discard. Manual testing covered a
16-page PDF, scrolling in both directions, 500% zoom, horizontal scrolling,
rotation, independent split panes, close/reopen after a zoom change, and normal
Quit without a new crash report. These are correctness checks, not a throughput
or memory benchmark.

## CodeGraph

The repository has an initialized `.codegraph` index. Use the configured
CodeGraph tools for structural code exploration and impact analysis before
editing. The index watches source changes automatically; treat `.codegraph`
runtime files as local tooling state and do not commit them.
