# TrePDF

**A PDF reader and editor built with C++23, Qt 6, QML, and PDFium.**

> **Beta:** TrePDF is under active development. Features, PDF compatibility,
> and the interface are still evolving. Keep your original documents and use
> **Save As** on a copy when testing edits. Bug reports and contributions are welcome.

TrePDF lets you read, edit, annotate, fill forms, and convert PDFs on your own
computer. Document processing runs locally. Optional font and OCR-language
downloads require an internet connection; they do not upload your PDF.

[Build from source](#build) · [Contribute](CONTRIBUTING.md) ·
[Report a bug](https://github.com/RaymondHuy/TrePDF/issues) ·
[Source documentation](#source-documentation)

## What can it do?

| Area | Current desktop capabilities |
| --- | --- |
| Read | Multiple document tabs, thumbnails, search, text selection, zoom, rotation, facing pages, and split views. |
| Edit | Edit supported existing text, add text and images, draw shapes, create tables, and edit their cells. |
| Annotate | Notes, highlights, free text, drawing comments, stamps, replies, and review statuses. |
| Forms and signatures | Fill supported AcroForms, create form fields, and place visible signatures, initials, and dates. |
| Organize | Insert/delete pages, merge PDFs, and split a PDF into separate files. |
| Headers and footers | Add text, images, and page numbers, with page ranges and synchronization for supported objects. |
| Protect | Add or change an opening password on eligible PDFs; add, update, or remove managed watermarks. |
| Convert | Recognize scanned text with local OCR and export supported ruled tables to Excel (`.xlsx`). |

Edits are staged as document drafts with Undo/Redo and Discard. Save and Save As
persist supported changes and reopen the result. Each document tab keeps its own
editing history and view state.

### Beta limitations

- PDF editing depends on the document's fonts and structure. Complex or
  unsupported objects remain read-only; this is not a guarantee that every PDF
  can be edited without layout changes.
- Excel export requires text-based tables with supported ruled grids. Borderless
  tables and image-only scans are not supported by the table extractor.
- Visible signatures are images, not certificate-based digital signatures.
- Some encrypted-document edits and advanced AcroForm structures are unsupported.
  See the relevant pipeline document before changing these behaviors.
- Desktop PDF JavaScript is enabled for form behavior, with restricted host
  actions. PDFium has no hard execution timeout for synchronous scripts.

## Platforms and packages

The repository includes these build paths. A build recipe does not imply that
every feature has been verified on every platform.

| Platform | Build path |
| --- | --- |
| macOS, Apple Silicon | Bundled ARM64 dependencies, local CMake build, and the `uat` workflow. |
| macOS, Intel | Native dependency preparation and the separate `uat-macos-intel` workflow. |
| Windows x64 | Local MSVC packaging script and the `uat` workflow; a separate MinGW setup guide is available. |
| Linux x64 | Ubuntu 24.04 build and packaging recipe in the `uat` workflow. |
| iPad | Separate iOS build and `uat-ipad` workflow; see the [iPad guide](docs/ipad-build.md). |
| Browser | Separate WebAssembly preview with a smaller feature set; see the [web guide](docs/web-build.md). |

macOS packages are separate for ARM64 (M1 and newer) and Intel; they are not
universal binaries. The current desktop workflows configure Qt 6.12 packages
for macOS 14.4+, with Qt 6.11 variants targeting macOS 13+.

For test packages, follow the [manual UAT guide](docs/github-uat.md). It explains
how to run a workflow and download its artifacts. These workflows do not publish
GitHub Releases or run the full feature test suite. Current macOS test packages
use ad-hoc signing and are not notarized.

## Build

### Prerequisites

- Git, CMake **3.21+**, and a C++23-capable compiler and standard library.
- A matching **Qt 6.8+** desktop kit with QML, Quick, Quick Controls 2,
  Network, and LinguistTools. Install Qt Test for tests and Qt Svg for runtime
  deployment. The current desktop workflows use Qt 6.12.0, plus Qt 6.11.0
  macOS variants; their module lists are in the workflow files.
- Ninja for the commands below.
- Native PDFium, qpdf, Tesseract/Leptonica, and zlib dependencies matching your
  operating system and architecture. libxlsxwriter is built from vendored source.
- Python **3.12+** if using the dependency preparation or local packaging scripts.

The checked-in PDFium, qpdf, and OCR binaries are **macOS ARM64** packages.
They cannot be reused for Intel, Windows, Linux, iOS, or WebAssembly builds.
Normal desktop CMake configuration does not download missing dependencies.

### Get the source

```sh
git clone https://github.com/RaymondHuy/TrePDF.git
cd TrePDF
```

For contributions, clone your fork instead. Run the following commands from the
repository root.

### Development build on Apple Silicon

Install Xcode's command-line tools and a compatible Qt macOS kit first. Adjust
`TREPDF_QT_PREFIX` to your own installation; CLion is optional.

```sh
TREPDF_QT_PREFIX="$HOME/Qt/6.12.0/macos"

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DBUILD_TESTING=ON \
  -DTREPDF_DEPLOY_RUNTIME=OFF \
  -DQt6_DIR="$TREPDF_QT_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_PREFIX_PATH="$TREPDF_QT_PREFIX" \
  -DPDFium_ROOT="$PWD/third_party/pdfium" \
  -DQPDF_ROOT="$PWD/third_party/qpdf" \
  -DOCR_ROOT="$PWD/third_party/ocr"

cmake --build build --target trepdf --parallel 4
./build/trepdf.app/Contents/MacOS/trepdf
```

Re-run the build command after editing source. Use a separate build directory
when changing the compiler, architecture, or Qt kit. For an optimized `.app`
or a drag-to-install `.dmg`, see the [local macOS Release guide](docs/macos-release-local.md).

### Other desktop builds and local packages

The local scripts use installed tools, prepare native dependencies, and create
Release ZIP packages with checksums. The first run needs network access for
pinned dependency downloads. The check commands below validate local tools
without downloading dependencies or building the app.

**macOS ARM64 or Intel** — run natively on the target architecture with Qt,
Python 3.12+, CMake, Ninja, and Xcode tools installed:

```sh
bash pipelines/build-macos.sh --qt "$HOME/Qt/6.12.0/macos" --check
bash pipelines/build-macos.sh --qt "$HOME/Qt/6.12.0/macos"
```

**Windows x64** — use 64-bit PowerShell, Visual Studio C++ tools with a Windows
SDK, a Qt MSVC x64 kit, Python 3.12+, CMake, and Ninja. Replace the example Qt path:

```powershell
.\pipelines\build-windows.ps1 -QtRoot 'C:\Qt\6.12.0\msvc2022_64' -CheckOnly
.\pipelines\build-windows.ps1 -QtRoot 'C:\Qt\6.12.0\msvc2022_64'
```

Packages are written under `artifacts/local/<platform>/`. These scripts build
Release packages with tests disabled. For Linux or custom toolchains, use the
dependency preparation and configure steps in [the UAT workflow](.github/workflows/uat.yml).
Pass the prepared `PDFium_ROOT`, `QPDF_ROOT`, and `OCR_ROOT` explicitly to CMake.
The [UAT guide](docs/github-uat.md) describes platform requirements and outputs;
the [Windows CLion guide](docs/windows-clion.md) covers the separate MinGW setup.

### CLion

Open the repository as a CMake project, select your installed compiler and Qt
kit, and use the same CMake options as the development build above. Run the
`trepdf` target. Do not copy another developer's CMake cache or absolute SDK paths.

## Tests

With the development build configured using `BUILD_TESTING=ON`, build the test
targets and run CTest. This example uses the bundled Apple Silicon libraries:

```sh
cmake --build build --parallel 4
DYLD_LIBRARY_PATH="$PWD/third_party/pdfium/lib:$PWD/third_party/qpdf/lib" \
  QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ctest --test-dir build --output-on-failure
```

For other platforms, use their native dependency/runtime paths. See the
[test guide](tests/README.md) for individual Qt Test methods and fixture
conventions. Offscreen tests do not replace native UI checks, especially for
macOS Metal rendering. The [contribution guide](CONTRIBUTING.md#test-your-change)
explains which checks to run for each kind of change.

## Find your way around

| Location | Purpose |
| --- | --- |
| `src/main.cpp`, `src/Main.qml` | Desktop startup and application composition. |
| `src/Components/` | QML interface, document views, tools, and editing overlays. |
| `src/PDFDocumentSession.*`, `src/PDFTabModel.*` | Per-document state and document tabs. |
| `src/PDFEditController*.cpp` | Selection, drafts, editing commands, and history. |
| `src/PDFSaveWriter.cpp` | Validating and writing PDF content changes. |
| `src/PDFPageItem.*`, `src/PDFFormWorker.*` | Page rendering and worker-owned PDF/form interaction. |
| `src/web/`, `src/ios/`, `src/platform/` | Browser preview and native platform integrations. |
| `i18n/` | Qt translation catalogs. |
| `tests/` | Qt tests and reproducible fixtures. |
| `cmake/`, `pipelines/`, `scripts/ci/`, `.github/workflows/` | Dependency discovery, builds, and packaging. |
| `third_party/` | Bundled dependencies and their notices. |

QML presents the interface; C++ controllers own document state and operations.
Workers perform rendering and longer PDF tasks using their own handles. Edits
remain drafts until the save pipeline validates and commits them. Preserve these
boundaries when adding features; see [Contributing](CONTRIBUTING.md#work-in-the-codebase).

## Localization

The desktop UI currently defaults to Vietnamese. Catalogs for English,
Vietnamese, Croatian, and Thai live in `i18n/qml_<locale>.ts`.
The locale is selected in `src/main.cpp`; there is currently no build-time
language option. See [building with another UI language](docs/macos-release-local.md#build-with-another-ui-language).

Use Qt's ID-based translations: `qsTrId()` in QML and `qtTrId()` in C++, with
stable PascalCase IDs and English source comments. After changing UI strings:

```sh
cmake --build build --target update_translations
# Complete the affected translation entries before rebuilding.
cmake --build build --target trepdf trepdf_qmllint --parallel 4
```

To add a language, register it in `I18N_TRANSLATED_LANGUAGES` in
`CMakeLists.txt`, generate and translate its catalog, and select the locale in
the entry point. Preserve placeholders such as `%1` and `%n`. See the
[translation contribution guide](CONTRIBUTING.md#translate-the-interface).

## Contributing

New contributors can help with reproducible bug reports, PDF compatibility
tests, translations, accessibility, documentation, and platform testing as well
as code. Small, focused improvements are welcome.

Read [CONTRIBUTING.md](CONTRIBUTING.md) for setup, coding conventions, tests,
and what to include in a pull request. For a substantial feature or architectural
change, open an issue to discuss the intended behavior first. Share only PDF
samples you have permission to publish and remove private information.

## Source documentation

Start with the English overview above. Some detailed architecture and pipeline
documents are currently in Vietnamese; documentation translations are welcome.

- **Architecture:** [source map](docs/source-file-map.md), [C++ guide](docs/cpp-file-guide.md),
  [QML guide](docs/qml-file-guide.md), and [page overlays](docs/overlay-architecture.md).
- **Editing:** [text and save pipeline](docs/text-editing-pipeline.md),
  [fonts](docs/font-providers.md), [tables](docs/table-drawing.md),
  [comments](docs/comment-annotation-pipeline.md), and [headers/footers](docs/header-text-pipeline.md).
- **Document operations:** [merge](docs/pdf-merge-pipeline.md),
  [split](docs/pdf-split-pipeline.md), and [watermarks](docs/watermark-pipeline.md).
- **Conversion:** [OCR](docs/ocr-pipeline.md) and [Excel export](docs/excel-export-pipeline.md).
- **Development:** [tests](tests/README.md), [QML profiling](docs/qml-profiling-clion.md),
  and [repository implementation guidance](AGENTS.md).

## License

TrePDF's source is licensed under the [Apache License 2.0](LICENSE).
Third-party libraries, fonts, icons, and test fixtures retain their own licenses.
See the notices in `third_party/` and the relevant asset directories, including
[qpdf](third_party/qpdf/NOTICE.md), [OCR](third_party/ocr/README.md),
[libxlsxwriter](third_party/libxlsxwriter/README.trepdf.md), and
[icons](src/assets/icons/LICENSE.txt).
