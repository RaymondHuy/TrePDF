# TrePDF Web preview

The browser preview is a separate executable, `trepdf_web`, enabled with
`-DTREPDF_BUILD_WEB=ON`. The default desktop and iPad builds keep their existing
targets and dependency preparation. The web target does not compile the desktop
document, editing, save or OCR controllers.

The preview uses the app's QML ribbon, document tabs, reader controls and empty
state. It supports opening a local PDF, rendering its pages, Single Page and
Continuous display, scrolling, page navigation, view rotation and zoom. The
**Edit** tab provides a limited editing preview: add text, draw rectangles,
ovals, lines, arrows, triangles and diamonds, then select, change, move or delete
those additions. Eligible original text and images can also be selected and
edited. Undo and Redo apply to these session edits. Save/Save As downloads a PDF
with the changes written into page content. Without any edits,
download preserves the original PDF bytes.

Choose Add Text and click or drag on the page to type directly in the text box.
Use Edit Text and click eligible original text, or double-click selected text,
to edit it directly. Original text changes use the bundled Liberation Sans font;
the panel explains the substitution before applying it. Moving original text
without changing its contents or style preserves its original font. Enter inserts
a line break; Ctrl/Cmd+Enter applies the text, Escape cancels the pending input,
and Save applies pending text before downloading the PDF. The right panel edits
font size, color and geometry. Text selection, Backspace/Delete and Undo/Redo
inside the text box operate on its text. Pending input survives scrolling or
switching panels. The inline editor and PDF export use the same bundled font.

One PDF is open at a time; choosing another file replaces it. PDF bytes stay in
the browser; there is no upload service. Download the edited PDF before closing
the document or browser tab, or reloading the page. Session history is not stored
in the PDF: after reopening an exported PDF, those additions are source content
and eligible text and images can be selected again.

Original images support movement, eight-handle resizing, optional aspect lock,
quarter-turn rotation, deletion and replacement with a local PNG/JPEG/WebP image.
Replacement keeps the selected frame. It does not upload the image.

Only conservatively validated top-level text and images are editable. Complex,
clipped, masked or nested content remains unchanged; the viewer explains when
objects are skipped. Existing links, page operations, interactive forms,
signatures and OCR are not supported. Their controls remain disabled. Form widget
appearances are not rendered by the current page adapter; use the desktop app to
read filled forms reliably. The web preview does not have the desktop editor's
complete feature set.

## Architecture

- `src/Components/` provides the shared presentation components:
  `RibbonTabStrip`, `RibbonSurface`, `ViewRibbonTools`, `EditRibbonTools`, `DocumentTabs`,
  `ReaderControlBar` and `DocumentEmptyState`. The desktop wrappers keep their
  native controller bindings; the browser supplies scalar state and handles the
  same UI signals through its own adapter.
- `src/Components/platform/AppWindow.qml` owns the window boundary. Qt File
  Selector chooses its `+web/AppWindow.qml` override for the browser.
- `src/Components/web/Viewer.qml` composes those shared components. Its
  `DocumentViewport` adapter handles browser page presentation. It does not copy
  the desktop `Main.qml` or its document-editing page.
- `src/web/` contains the browser entry point, file bridge and PDF page adapter.
  `pdfium-editor.js` validates browser edit drafts and writes their text and
  vector paths through the PDFium worker. `pdfium-source.js` lazily indexes eligible
  saved text/images and reconstructs changed pages in their original drawing
  order. Each edit builds a candidate document from immutable source bytes and
  swaps it in only after validation. Browser session history and interaction
  stay separate from the desktop editing controllers.
- `src/web/fonts/LiberationSans-Regular.ttf` supplies the unmodified Liberation
  Sans 2.1.5 Regular face for added text, including Vietnamese. The worker fetches
  it from the same host and embeds it in exported PDFs. Its SIL OFL license,
  authors and source notice ship in `licenses/LiberationSans/`.
- Qt Quick runs in single-threaded WebAssembly. PDFium runs in a dedicated
  Web Worker with its own WebAssembly memory, communicating asynchronously.
- `scripts/ci/prepare_web_dependencies.py` downloads a pinned PDFium browser
  build and verifies SHA-256 before extracting it. PDF JavaScript and XFA are
  disabled in this dependency build.
- The web branch returns from the root CMake file before desktop dependencies
  are discovered. No PDFium, qpdf or OCR desktop libraries are linked into it.

The desktop PDF engine, editing history, persistence and native integrations keep
their existing implementation. Shared QML handles presentation; browser file I/O,
rendering and limited editing stay at the platform boundary.

## Build using GitHub Actions

Run **Actions → uat-web → Run workflow** on the branch containing these files.
This workflow is manual and uploads a `TrePDF-0.0.x-web-wasm` artifact. Its version
counter is independent of the desktop `uat` workflow. It does not publish a site.

The artifact contains a ZIP and SHA-256 checksum. Extract the ZIP, then serve the
extracted directory:

```sh
cd /path/to/extracted/TrePDF-web
python3 -m http.server 8000 --bind 127.0.0.1
```

Open `http://127.0.0.1:8000` in a browser with WebGL 2. Opening `index.html` with
`file://` does not work. Serve only the extracted website directory, never the
repository, SDK or build tree.

For a hosted test, copy the ZIP contents to an HTTPS static host and serve `.wasm`
files with `Content-Type: application/wasm`. Keep `pdfium/` beside `index.html`.
The initial single-threaded Qt build and independent PDFium worker do not need
SharedArrayBuffer or COOP/COEP headers. PDFium assets come from the same host;
there are no runtime CDN imports.

## Optional local build

Local SDK installation is not required when using Actions. To build locally,
use Python **3.12 or later**, Qt **6.12.0** for `wasm_singlethread`, a native Qt **6.12.0** host kit and
Emscripten **5.0.5**, the supported pair in
[Qt's WebAssembly documentation](https://doc.qt.io/qt-6/wasm.html).

Install build tools into a virtual environment and keep the SDKs in a dedicated
temporary directory if they are only needed for validation:

```sh
python3 -m venv /tmp/trepdf-web-tools/venv
source /tmp/trepdf-web-tools/venv/bin/activate
python -m pip install 'cmake==3.31.6' 'ninja==1.11.1.3' 'aqtinstall==3.3.0'

python -m aqt install-qt all_os wasm 6.12.0 wasm_singlethread \
  --outputdir /tmp/trepdf-web-tools/Qt \
  --archives qtbase qtdeclarative qtsvg --modules qtshadertools
```

Use an existing native Qt 6.12.0 installation for host tools, or install a host
kit with `aqt` as shown in the workflow. Install and activate Emscripten 5.0.5
using [emsdk](https://emscripten.org/docs/getting_started/downloads.html), then:

```sh
source /path/to/emsdk/emsdk_env.sh
python scripts/ci/prepare_web_dependencies.py --directory _deps/web
node scripts/ci/tests/test_web_pdfium.mjs _deps/web/pdfium/lib
node scripts/ci/tests/test_web_edit_pdfium.mjs _deps/web/pdfium/lib
node scripts/ci/tests/test_web_source_pdfium.mjs _deps/web/pdfium/lib
node --test scripts/ci/tests/test_web_bridge.mjs

python scripts/ci/build_web.py \
  --qt-cmake /tmp/trepdf-web-tools/Qt/6.12.0/wasm_singlethread/bin/qt-cmake \
  --qt-host-path /path/to/Qt/6.12.0/macos \
  --dependencies _deps/web --build build/web --stage build/web-site \
  --output artifacts/web --version 0.0.5

python3 -m http.server 8000 --bind 127.0.0.1 --directory build/web-site
```

On Linux, the native Qt host kit directory is normally `gcc_64`. The build helper
invokes the Qt shell wrapper through `bash`, because the platform-independent Qt
WASM archive may not retain its executable bit.

Only allowlisted runtime files, licenses and build metadata are copied to
`build/web-site`. The package contains `build-info.json`, `VERSION`, and local
startup instructions. The helper replaces only a staging directory marked as a
previous TrePDF web package; it refuses to overwrite another nonempty directory.

## Limits and validation

Files are limited to 64 MiB and 2,000 pages. Password-protected PDFs are rejected
with a message. Only visible pages are rasterized, with one active worker request
and a maximum of 4 million pixels / 4096 pixels per dimension for each raster.
An open or render operation that exceeds 30 seconds terminates the worker and
releases its document. The page can then open another PDF. The Qt heap has a
512 MiB maximum; PDFium has a separate heap.

Editing supports up to 128 changed/added objects and 32 history states per session.
Source indexing is lazy and bounded to 2,048 page objects, 50,000 page text
characters, 8 million image pixels per page and 4,096 indexed candidates per PDF.
Replacement images are limited to 16 MiB per input, 4 million decoded pixels and
4096 pixels per side; retained decoded image snapshots are bounded to 32 MiB. Each
text object accepts up to 1,024 characters at 6–144 points from the bundled
font's Latin and Vietnamese coverage. Signed or protected PDFs are read-only. The viewer prompts
before replacing or closing a document with unsaved edits or pending input;
the browser also receives a before-unload guard for reloading or leaving the tab.

The workflow tests the actual PDFium WASM worker and the browser bridge before
building, then runs the packaged viewer in Chromium: shared ribbon and footer,
open, render, scroll, zoom, page navigation, Single Page/Continuous display,
rotation, disabled tools, byte-exact download without edits, close, invalid-file
recovery and resize. Editing checks cover creating text and shapes, Undo/Redo,
original-text replacement, source-image movement/resizing/replacement, and
rendering the downloaded PDF after reopening it. Tests verify old text is removed
from actual PDF content and replacement image pixels survive export. Screenshot pixel checks
verify that PDF content reaches the Qt canvas
and an asymmetric page marker rotates with it. QML warnings and JavaScript errors
fail the browser test. Browser tests use synthetic PDF files and a temporary
localhost server. To run them locally with
Playwright 1.62.1 and pngjs 7.0.0 installed in a separate directory:

```sh
NODE_PATH=/path/to/node_modules \
  node scripts/ci/tests/test_web_browser.cjs build/web-site
```

Set `TREPDF_TEST_CHROME_CHANNEL=chrome` to use an installed Chrome instead of
Playwright's Chromium; `TREPDF_WEB_SCREENSHOTS=/tmp/trepdf-web-shots` saves screenshots.
Manually check representative real PDFs and the intended Safari/iPad devices;
the Chromium smoke test does not establish support for every browser or PDF.
Desktop build and packaging workflows stay independent of these web tests.
