# TrePDF

TrePDF is a Qt Quick Controls desktop PDF reader powered by PDFium. The app
supports continuous page viewing, asynchronous rendering, draft content edits,
and an ID-based localization system with Vietnamese enabled by default.

In Edit mode, selecting editable text opens contextual Edit and Delete actions.
Deletion is stored only as a session draft and can be undone or redone.
Compatible text objects on the same visual line are grouped into one editable
candidate while retaining every source object path for draft rendering and
undo/redo.

Draft geometry keeps immutable PDF source bounds separate from the layout frame
and laid-out content bounds. Existing PDF text seeds its frame from the Qt text
width so the original line fits, while later edits wrap within that frame. The
selected frame exposes four edge resize handles; dragging an edge reflows
text live and stores the geometry change in session undo/redo.

Each session draft stores text, character-format spans, paragraph formats, its
layout frame, and content bounds. Undo and redo snapshot the complete rich-text
draft. The inline rich-text editor supports multiple lines, wraps/reflows inside
the layout frame, renders mixed character styles, and applies formatting to the
selected text range.

Fonts placed under `src/assets/fonts` are bundled with the application and
preferred over matching system-installed families for draft text display.

Edit mode also shows a collapsible text-formatting panel on the right with font
family, size, color, bold, italic, and underline controls. Font family, bold,
italic, underline, and color apply immediately to selected draft text and
support undo/redo. Font size is currently UI-only.

The complete editable-text discovery, geometry, font, baseline, overlay, and
draft-rendering flow is documented in
[`docs/text-editing-pipeline.md`](docs/text-editing-pipeline.md).

## Build

Install Qt 6.8 or newer and provide a PDFium build that contains `fpdfview.h`
and the `pdfium` library.

```sh
cmake -S . -B build \
  -DQt6_DIR=/Users/luongvanhuy/Qt/6.11.0/macos/lib/cmake/Qt6 \
  -DPDFium_ROOT=third_party/pdfium
cmake --build build
```

Run the app:

```sh
./build/trepdf.app/Contents/MacOS/trepdf
```

On non-macOS desktop builds, the binary is typically at `./build/trepdf`.

## CLion

Qt is already configured for this local SDK:

```sh
/Users/luongvanhuy/Qt/6.11.0/macos
```

PDFium is configured from `third_party/pdfium`.

Open the repository folder in CLion and use the default `Debug` CMake profile.
The project configures Qt and PDFium from the local paths above, then creates
the `trepdf` target.

## Localization

TrePDF uses Qt's ID-based translation system. Translation keys are unique
project-wide English names written in PascalCase. QML uses `qsTrId()`, while
C++ uses `qtTrId()`.

```qml
//% "Open PDF"
text: qsTrId("OpenPdf")
```

Vietnamese is the current default UI language. Its source catalog is
`i18n/qml_vi.ts`; CMake compiles it to `qml_vi.qm` and embeds it next to the
main QML file under `:/qt/qml/TrePDF/src/i18n`.

After adding or changing a user-visible string, update the catalog and rebuild:

```sh
cmake --build build --target update_translations
cmake --build build --target trepdf
```

To add another language, append its locale to `I18N_TRANSLATED_LANGUAGES` in
`CMakeLists.txt`, reconfigure the project, run `update_translations`, then
complete the generated `i18n/qml_<locale>.ts` catalog. Preserve placeholders
such as `%1` and `%n` in every translation.
