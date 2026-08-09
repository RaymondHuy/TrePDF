# TrePDF Agent Guide

## Communication

- Reply to the user in Vietnamese by default.
- Keep explanations practical and concise. The user prefers implementation over long proposals.
- Preserve user changes in the working tree. Do not reset, revert, or reformat unrelated code.

## Project Summary

TrePDF is a macOS desktop PDF reader built with Qt 6, Qt Quick Controls, QML,
C++17, and PDFium. The current scope includes PDF viewing, sandboxed AcroForm
interaction, draft editing for supported text, images, and page operations,
and Save/Save As persistence.

Current features:

- Open multiple local PDF files in independent tabs and close them separately.
- Insert a blank page immediately after the current page or delete the current
  page, with undo/redo/discard and Save/Save As persistence.
- Add Sticky Note, Highlight, Underline, and Strikeout annotations from the
  Comment tab. New comments are undoable session drafts; supported source
  comments can be indexed, selected, edited, and deleted with
  undo/redo/discard. Save/Save As persists additions, content edits, and
  removals without rewriting page content streams on annotation-only pages.
  The comments panel shows each annotation's author and modification date,
  falling back to its creation date. Root comments accept replies; the panel
  flattens imported reply chains to one display level, saves new replies with
  standard `/IRT`, and cascades root deletion across the thread. Reply
  annotation appearances stay hidden from the page viewer while the replies
  remain available and editable in the panel. Read-only and LockedContents
  source annotations remain protected.
- Draw rectangles, ovals, lines, arrows, triangles, and diamonds from the
  Shapes dropdown. Added shapes are vector session drafts that can be selected,
  moved, resized, deleted, undone/redone, and persisted as PDF path objects.
  New shapes use a black outline with transparent fill; near-axis lines and
  arrows snap horizontally or vertically.
- Render every page through PDFium.
- Virtualized continuous scrolling with reusable QML delegates.
- Asynchronous page rendering on a worker thread.
- Page navigation, zoom, and fit-to-width controls.
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
- Text draft editing with selection, undo, redo, discard, Save, and Save As.
  Drafts stay session-only until a save succeeds.
- Add Image uses the same placement cursor and draft history as Add Text. After
  the user clicks a page and chooses a supported local image, TrePDF snapshots
  the decoded pixels, shows a resizable draft that preserves the initial aspect
  ratio, and writes it as a PDF image object on Save/Save As.
- Top-level PDF image objects can be selected and deleted with Backspace,
  Delete, or the contextual Delete action. Deletion remains an undoable session
  draft until Save/Save As removes the source object; images nested in Form
  XObjects remain protected.
- Added images and editable top-level source images can be dragged within the
  page. A drag preserves image size and creates one undoable command. Source
  image preview transforms run on the render worker, and Save/Save As translates
  the original image matrix so pixels, orientation, and z-order remain intact.
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
  from its Qt text width so the original line fits before reflow. On entering
  text edit, a transient frame extends from the existing left edge to the next
  same-row text boundary or the inferred right page margin. It is persisted
  only by a real text edit, a paragraph-format change, or explicit resize, so a
  no-op editor session does not create a draft. Four edge resize handles update
  the normalized frame, reflow text live, and commit geometry through the same
  draft undo/redo snapshots as text and formatting.
- Compatible same-line text objects are grouped into one draft edit while each
  source object path remains available for rendering suppression.
- Fonts under `src/assets/fonts` are bundled and preferred over matching system
  families when resolving draft text fonts.
- Contextual Edit/Delete actions when selecting editable text; deletion remains
  an undoable draft until Save writes it into the PDF.
- Transactional PDF persistence through a separate PDFium document handle.
  Save overwrites atomically through `QSaveFile`; Save As leaves the source
  untouched. Drafts and history are cleared only after the saved file reopens.
- An edit-only, collapsible right-side text-formatting panel provides UI for
  font family, size, color, bold, italic, and underline. Materialized character
  formatting participates in draft undo/redo; size is not connected to draft
  formatting yet. During inline editing, bold/italic/underline apply to the
  selected range; with only a caret they become the typing format for
  subsequently inserted text without changing existing characters. Invoking
  one of those controls on a selected text object opens its inline editor first
  and leaves the caret at the end, so it never restyles the whole object. The
  editor merges range formatting directly into its text document and syncs
  B/I/U back after local undo/redo without replacing the rich-text document.
  The font picker filters system faces by the active bold/italic style using a
  lightweight save-compatibility probe; filtered PDF source fonts remain
  visible as the current value only.
  Its single dropdown has Vietnamese, English, Chinese, Japanese, and Korean
  filters. Language groups narrow the same save-compatible exact-face list,
  intersecting every regular/bold/italic face used by a mixed-style selection.
- ID-based localization with Vietnamese as the default UI language.
- A restrained orange UI inspired by Foxit Reader.

## Repository Layout

- `CMakeLists.txt`: application target, QML module, translation pipeline,
  Qt/PDFium discovery, and macOS PDFium bundling.
- `cmake/FindPDFium.cmake`: creates the `PDFium::PDFium` imported target.
- `i18n/qml_vi.ts`: Vietnamese ID-based translation catalog.
- `src/main.cpp`: Qt application startup, default UI language, and QML engine
  loading.
- `src/ApplicationFonts.h/.cpp`: bundled-font registration and the separate
  bundled/system family catalogs used by draft font resolution.
- `src/DraftImageItem.h/.cpp`: QML item that draws the snapshotted pixels of an
  inserted-image draft without reopening the selected file path.
- `src/Main.qml`: multi-tab coordination, menus, toolbar, and active workspace.
- `src/PDFTabModel.h/.cpp`: per-tab document/edit sessions, active-tab state,
  duplicate-path protection, and cross-tab save guards.
- `src/PDFDocument.h/.cpp`: main-thread PDFium document, metadata, page sizes,
  navigation state, and source file path.
- `src/PDFEditController.h/.cpp`: editable-content discovery, selection,
  rich-text draft state, snapshot-based undo/redo history, and save commands.
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
- `src/PDFPageItem.h/.cpp`: asynchronous rendering and scene graph texture
  upload for one page delegate.
- `src/Components/FormChoiceOverlay.qml`: custom combo-box and list-box
  presentation for the safe subset of AcroForm choice fields.
- `src/Components/ShapeDraftView.qml`: shared Qt Quick Shapes preview used by
  the ribbon palette, drag preview, and added-shape draft delegates.
- `src/Components`: ribbon controls, document view, page delegates, and the
  draft editing overlay.
- `third_party/pdfium`: prebuilt arm64 PDFium headers and library. Treat this as
  third-party code and do not modify it for normal feature work.

## Local Toolchain

This project is configured for the following local SDK:

- Qt prefix: `/Users/luongvanhuy/Qt/6.11.0/macos`
- `Qt6_DIR`: `/Users/luongvanhuy/Qt/6.11.0/macos/lib/cmake/Qt6`
- `qmake`: `/Users/luongvanhuy/Qt/6.11.0/macos/bin/qmake`
- PDFium root: `<repo>/third_party/pdfium`
- CLion: `/Applications/CLion.app`
- Default build directory: `<repo>/cmake-build-debug`

There is intentionally no `CMakePresets.json`. Do not add one unless the user
explicitly requests it. `CMakeLists.txt` already contains local fallback paths
for Qt and PDFium.

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
- The worker owns a separate thread-local `FPDF_DOCUMENT` opened from the file
  path. It must never use the GUI thread's document handle.
- Each request has an atomic cancellation flag. Check it before expensive work
  and before posting the rendered image back to the GUI thread.
- Deliver results with a queued invocation and guard the target with
  `QPointer<PDFPageItem>`.
- Reject a result when its request id, document pointer, revision, page, zoom,
  device pixel ratio, or pixel size no longer matches.

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
- Clear the CPU-side pending `QImage` immediately after creating the scene graph
  texture. Keeping both copies defeats the memory optimization.

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
  top-level page text plus top-level image movement/deletion are currently
  saveable; nested Form XObject content must remain non-editable until its
  transform/clip/resource scope can be preserved. Keep moved source images in
  place and translate their existing matrix; this preserves their resource,
  orientation, size, and z-order. Apply the same matrix-only path to editable
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
  `FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS`. The subset flag trims newly
  embedded font programs to glyphs used by new text objects while leaving
  original PDF font resources unchanged. Commit only after PDFium succeeds;
  any earlier error must leave the destination and in-memory drafts untouched.
- Persist highlight drafts as `FPDF_ANNOT_HIGHLIGHT` annotations with explicit
  quad points, bounding rectangles, color/opacity, and the Print flag. Adding
  annotations alone must not call `FPDFPage_GenerateContent()`; mixed
  text/image edits still regenerate the affected page content normally.
- Persist added shapes as top-level PDF path objects. Convert every path point
  through `FPDF_DeviceToPage()` so CropBox offsets and rotation match the
  editor; keep ovals vector by using cubic Bézier segments and retain
  insertion order with the other added page objects.
- Preserve the earliest source z-order with `FPDFPage_InsertObjectAtIndex()`;
  added text and images are appended in insertion order. Convert coordinates
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
  full-size validation in the save writer.

## Memory and Performance Invariants

- Memory must scale with visible and near-visible pages, not total page count.
- Never store rendered images for every page.
- Never queue unlimited obsolete render jobs.
- Do not duplicate a full-page image in both a long-lived CPU cache and a
  `QQuickPaintedItem` backing store.
- Preserve native device-pixel-ratio quality unless the user explicitly accepts
  lower-quality rendering.
- Full-page rendering can still be expensive for scanned PDFs or high zoom.
  Tile rendering with a strict LRU byte budget is the next major optimization,
  not a larger full-page cache.

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

## CodeGraph

The repository has an initialized `.codegraph` index. Use the configured
CodeGraph tools for structural code exploration and impact analysis before
editing. The index watches source changes automatically; treat `.codegraph`
runtime files as local tooling state and do not commit them.
