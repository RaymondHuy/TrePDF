# TrePDF

TrePDF is a Qt Quick Controls desktop PDF reader powered by PDFium. The app
supports multiple PDFs in independent tabs, continuous page viewing,
asynchronous rendering, rich-text content edits, Save/Save As, and an ID-based
localization system with Vietnamese enabled by default.

Each document tab owns its PDF handle, draft/undo history, selection, zoom,
scroll position, and formatting-panel state. Only the active tab creates page
delegates and textures. Opening an already-open canonical path activates its
existing tab, and Save As cannot overwrite a PDF used by another open tab.

In Edit mode, selecting editable text opens contextual Edit and Delete actions.
Changes remain undoable session drafts until Save or Save As writes them into a
PDF. Compatible text objects on the same visual line are grouped into one
editable candidate while retaining every source object path for draft rendering
and persistence.

Draft geometry keeps immutable PDF source bounds separate from the layout frame
and laid-out content bounds. Existing PDF text seeds its frame from the Qt text
width so the original line fits. When its inline editor opens, TrePDF
transiently extends that frame from its current left edge to the remaining
column width, or to the inferred right page margin when no same-row text blocks
it. The wider frame is stored only after a real text edit; opening and closing
the editor leaves the PDF geometry unchanged. The selected frame exposes four
edge resize handles; dragging an edge reflows text live and stores the geometry
change in session undo/redo.

Each session draft stores text, character-format spans, paragraph formats, its
layout frame, and content bounds. Undo and redo snapshot the complete rich-text
draft. The inline rich-text editor supports multiple lines, wraps/reflows inside
the layout frame, renders mixed character styles, and applies formatting to the
selected text range.

Fonts placed under `src/assets/fonts` are bundled with the application and
preferred over matching system-installed families for draft display and PDF
embedding. The save writer can reuse the original PDF font when a replacement
run only uses characters already present in its source text, and can use a
compatible PDF standard font for ASCII. Edited non-ASCII text is normalized to
NFC; newly introduced Vietnamese text inheriting an Arial/Helvetica face is
written with the matching bundled Arial Unicode font instead of reusing a
potentially incomplete PDF subset. Eligible newly selected macOS system fonts
whose exact regular/bold/italic face is available are resolved with CoreText,
snapshotted before the save worker starts, and embedded as CID Unicode TrueType
fonts. On serialization, PDFium subsets each newly embedded face to the glyphs
used by the new text objects; original PDF font resources remain untouched. TTC
faces are materialized individually so PDFium does not select the wrong face.
The font picker probes the exact active style and excludes known
incompatible system faces such as color, CFF/CFF2, variable, bitmap-only, or
embedding-restricted fonts. A filtered source font can still be displayed as
the current PDF font without being offered in the popup. Unedited objects keep
their original font resources. Missing glyphs, system faces larger than 64 MiB,
and other failures discovered by the final content-specific check stop the
save with an error and keep the draft intact.

The same font dropdown provides Vietnamese, English, Chinese, Japanese, and
Korean filters. Vietnamese uses full precomposed Vietnamese coverage; English
uses Latin writing-system metadata plus an exact-face Basic Latin probe; CJK
groups combine Qt writing-system metadata with an exact-face glyph probe.
Language filters only narrow the already save-compatible face list, and one
family may appear in more than one group. A mixed-style selection intersects
the results for every regular/bold/italic face it actually uses.

For edited runs that still reuse their original PDF font, Save preserves the
exact inherited horizontal text scale. Runs using a newly embedded or standard
font receive Qt-to-PDFium metric compensation once; reopening and saving again
reuses that resulting scale instead of compounding it. Selecting a font family
also resets inherited stretch for the changed range, including explicitly
reselecting the current family to normalize text saved by an older version.
Because Qt font stretch is not linear for every macOS face, the editor measures
and calibrates the Qt stretch request per family, style, and pixel size; rich
text preview and save-time layout share that mapping while the PDF matrix keeps
the exact source scale.

Save writes the current file atomically; Save As preserves the source file and
opens the saved copy. The controller snapshots the draft on the GUI thread and
runs serialization on a worker so the interface can keep painting its saving
state. The writer uses a separate PDFium document, replaces every source object
represented by a draft, lays out multiline/rich-text runs, calls
`FPDFPage_GenerateContent()`, and only clears undo history after the saved PDF
has reopened successfully.

Editable source text is currently limited to top-level page objects with Fill,
Stroke, or Fill+Stroke rendering. Text used as a clipping path or nested inside
a Form XObject remains viewable but is not offered as safely editable content.

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
