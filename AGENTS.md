# TrePDF Agent Guide

## Communication

- Reply to the user in Vietnamese by default.
- Keep explanations practical and concise. The user prefers implementation over long proposals.
- Preserve user changes in the working tree. Do not reset, revert, or reformat unrelated code.

## Project Summary

TrePDF is a macOS desktop PDF reader built with Qt 6, Qt Quick Controls, QML,
C++17, and PDFium. The current scope is PDF viewing plus session-only draft
editing for supported PDF content. Persisting edits back into the PDF is
planned but is not implemented yet.

Current features:

- Open and close local PDF files.
- Render every page through PDFium.
- Virtualized continuous scrolling with reusable QML delegates.
- Asynchronous page rendering on a worker thread.
- Page navigation, zoom, and fit-to-width controls.
- Session-only text draft editing with selection, undo, redo, and discard.
- Compatible same-line text objects are grouped into one draft edit while each
  source object path remains available for rendering suppression.
- Fonts under `src/assets/fonts` are bundled and preferred over matching system
  families when resolving draft text fonts.
- Contextual Edit/Delete actions when selecting editable text; deletion remains
  a session-only draft and supports undo/redo.
- An edit-only, collapsible right-side text-formatting panel provides UI for
  font family, size, color, bold, italic, and underline. Font-family changes
  update the current draft and support undo/redo; the other controls are not
  connected to draft formatting yet.
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
- `src/Main.qml`: menus, toolbar, virtualized `ListView`, and page delegates.
- `src/PDFDocument.h/.cpp`: main-thread PDFium document, metadata, page sizes,
  navigation state, and source file path.
- `src/PDFEditController.h/.cpp`: editable-content discovery, selection,
  session draft state, and undo/redo history.
- `src/PDFPageItem.h/.cpp`: asynchronous rendering and scene graph texture
  upload for one page delegate.
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
- Wrap user-visible text in `qsTrId()` using the localization rules above.

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

The repository currently has no initialized `.codegraph` index. If CodeGraph is
available in a future session, ask the user before running `codegraph init -i`.
