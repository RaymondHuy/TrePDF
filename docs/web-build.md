# TrePDF Web preview

The browser preview is a separate executable, `trepdf_web`, enabled with
`-DTREPDF_BUILD_WEB=ON`. The default desktop and iPad builds keep their existing
targets and dependency preparation. The web target does not compile the desktop
document, editing, save or OCR controllers.

The first milestone supports opening a local PDF, rendering its pages, scrolling,
zooming and downloading the original PDF. PDF bytes stay in the browser; there is
no upload service. Closing the tab or reloading discards the in-memory document.
Editing, interactive forms, signatures, OCR and saving modified documents are not
part of this preview. Form widget appearances are not rendered by the current
page-only adapter; use the desktop app to read filled forms reliably.

## Architecture

- `src/web/` contains the browser entry point, bridge and PDF page adapter.
- `src/Components/web/` contains the preview UI and platform components.
- Qt Quick runs in single-threaded WebAssembly. PDFium runs in a dedicated
  Web Worker with its own WebAssembly memory, communicating asynchronously.
- `scripts/ci/prepare_web_dependencies.py` downloads a pinned PDFium browser
  build and verifies SHA-256 before extracting it. PDF JavaScript and XFA are
  disabled in this dependency build.
- The web branch returns from the root CMake file before desktop dependencies
  are discovered. No PDFium, qpdf or OCR desktop libraries are linked into it.

This boundary lets the browser proof of concept develop without adding browser
conditions to the desktop editing engine. Full desktop feature parity requires
separate work.

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
use Qt **6.12.0** for `wasm_singlethread`, a native Qt **6.12.0** host kit and
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
node --test scripts/ci/tests/test_web_bridge.mjs

python scripts/ci/build_web.py \
  --qt-cmake /tmp/trepdf-web-tools/Qt/6.12.0/wasm_singlethread/bin/qt-cmake \
  --qt-host-path /path/to/Qt/6.12.0/macos \
  --dependencies _deps/web --build build/web --stage build/web-site \
  --output artifacts/web --version 0.0.1

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

The workflow tests the actual PDFium WASM worker and the browser bridge before
building, then runs the packaged viewer in Chromium: open, render, scroll, zoom,
page navigation, byte-exact download, close, invalid-file recovery and resize.
Screenshot pixel checks verify that PDF content reaches the Qt canvas. Browser tests use
synthetic PDF files and a temporary localhost server. To run them locally with
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
