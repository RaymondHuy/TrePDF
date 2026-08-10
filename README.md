# TrePDF

TrePDF is a Qt Quick Controls desktop PDF reader powered by PDFium. The app
supports multiple PDFs in independent tabs, continuous page viewing,
asynchronous rendering, per-tab left/right view rotation, rich-text content
edits, image insertion, Save/Save As, and an ID-based localization system with
Vietnamese enabled by default.

Standard AcroForm widgets are rendered and interactive in Browse mode. Text
fields support focus, selection, typing, IME commits, clipboard shortcuts, and
field-local undo/redo. Eligible fixed-choice combo boxes and list boxes use a
Qt Quick overlay synchronized with the PDFium form session; it supports single
and multiple selection, accessible field labels, and keyboard navigation plus
type-ahead for combo popups. Editable combo boxes, choice fields that commit on
selection or define field actions, and controls too small for a safe custom
presentation stay on PDFium's native interaction path. Checkboxes, radio
buttons, and form buttons that do not need host integration also use their
native PDF behavior. PDFium does not expose undo/redo history for non-editable
choice fields, but all field values participate in the per-tab draft guard and
persist through Save/Save As.
PDF JavaScript is enabled automatically so form calculations, validation,
formatting, field visibility, and document/page actions work without a
per-document prompt, matching the normal reader experience. TrePDF keeps its
safe-reading sandbox active: internal page destinations navigate inside the
document when activated by the user, while user-activated HTTP and HTTPS links
show their destination and require explicit confirmation before opening in the
default browser. Other URI schemes, files, email, printing, form submission,
dialogs, and JavaScript timers remain blocked. PDFium's public API does not
provide a hard execution timeout, so a malicious synchronous script can still
make the app unresponsive.

The Forms tab can add Text Field, Check Box, Radio Button, fixed single-select
Dropdown, multi-select List Box, and Comb Field widgets by clicking or dragging
on a page.
Check boxes and radio buttons use square placement and start unchecked;
dropdowns start with three localized options, while list boxes start with five.
A Comb Field is persisted as a standard PDF text
field whose positive character limit defines the number of equal-width cells;
new fields start with five cells and cannot also be multi-line. Added fields can
be selected, moved, resized, or deleted before Save and persist as standard
interactive AcroForm widgets with explicit appearances. Selecting a Text Field
opens a right-side panel for configuring multi-line input and an optional
maximum character count. Selecting a Comb Field opens its own panel, where the
required character limit controls the number of cells. Selecting a Radio Button
opens a right-side panel where its Group ID can be edited or that member can be
deleted. Radio Buttons with the same Group ID are written as one parent field
with child Widgets, so selecting one clears the other members in TrePDF and
external PDF readers. TrePDF rehydrates saved text-field options, including
comb layout and cell count, plus group membership, radio state, dropdown/list-box
options, and choice selection after reopen.
A TrePDF-created Text Field, Comb Field, Dropdown, List Box, or Radio Button can then be
deleted as a Forms draft: Discard restores it, while Save or Save As removes the
Widget and safely rebuilds any remaining Radio group. Text Field, Comb Field,
Dropdown, and List Box removal is limited to top-level, single-widget TrePDF fields;
source-backed Check Box and arbitrary AcroForm structures are not removed by
this path.

Each document tab owns its PDF handle, draft/undo history, selection, zoom,
scroll position, and formatting-panel state. Only the active tab creates page
delegates and textures. Opening an already-open canonical path activates its
existing tab, and Save As cannot overwrite a PDF used by another open tab.

In Edit mode, selecting editable text opens contextual Edit and Delete actions.
Changes remain undoable session drafts until Save or Save As writes them into a
PDF. Compatible text objects on the same visual line are grouped into one
editable candidate while retaining every source object path for draft rendering
and persistence.

The Comment ribbon provides Sticky Note, Highlight, Squiggly, Underline, and
Strikeout tools plus a collapsible, whole-document comments panel. Supported
comments and replies, including annotations already in the PDF, can be selected
and have their text edited in the panel. Markup uses a compact curated color
grid and a system picker for custom colors. Squiggly is source-text-aware: its
drag rectangle is intersected with non-whitespace glyphs from the PDF text
layer, then snapped into per-line rectangles and QuadPoints.
Nearly horizontal drags use a three-pixel hit-test band, so small differences
in pointer height do not silently discard the gesture. If the asynchronous text
scan is still running at release, TrePDF keeps the page, selection, and color,
prioritizes that page, and creates the draft automatically when geometry is
ready.
Dragging only across whitespace or an area without source text creates no
annotation. Invisible OCR text layers are accepted for this hit testing but do
not become selectable or editable text candidates. Image-only scans without a
usable text layer therefore do not support Squiggly. The current axis-aligned
geometry accepts upright horizontal glyphs only; rotated or vertical glyphs
remain unsupported until oriented QuadPoints are implemented. Pages with more
than 50,000 text-layer characters skip the per-glyph cache for memory safety and
do not support Squiggly. Highlight, Underline, and Strikeout keep their existing
free-rectangle behavior. Root comments can receive replies;
each root has a dropdown control that expands its replies at one visual level,
and reply-to-reply is not offered. Reply annotations remain available in this
panel but their standalone appearances are hidden from the PDF page viewer.
Comment changes are per-tab session drafts with
undo, redo, discard, selection, and deletion support. Save and Save As persist
them as standard PDF annotations with geometry, color, contents, print
visibility, and `/IRT` reply links; annotation-only saves do not regenerate the
page content stream. Each comment or reply is limited to 700 characters, with
the panel showing a live counter before commit. Existing supported annotations
are rehydrated for navigation and display, including their author and
modification date (or
creation date when no modification date is present). Editing updates the source
annotation's `/Contents`, modification date, and rich-text value when present,
while preserving its appearance, popup, thread links, geometry, and other
metadata. Read-only and LockedContents annotations remain protected. Deleting
a root comment also removes its replies so the saved document cannot contain
an orphaned thread.

The Shapes dropdown offers rectangles, ovals, lines, arrows, triangles, and
diamonds. After choosing a shape, drag over a page to create a vector session
draft with a black outline and transparent fill, then select it to move, resize,
or delete it. Near-horizontal and near-vertical lines and arrows snap to their
axis. Shape creation and geometry changes participate in undo, redo, and
discard. Save and Save As write shapes as PDF path objects, including cubic
Bézier ovals, so they remain sharp when zoomed and keep the same placement on
cropped or rotated pages.

The Image tool changes the page cursor to a placement `+`. Clicking a page opens
the local image picker; the chosen PNG, JPEG, BMP, GIF, WebP, or TIFF is decoded
and snapshotted immediately, displayed as a resizable session draft, and added
to undo/redo. The draft can also be dragged to a new position. Save/Save As
embeds the pixels as a PDF image object, preserving the editor geometry on
cropped and rotated pages.

Existing top-level PDF images can be selected in Select mode, dragged without
changing their size, or removed with Backspace, Delete, or the contextual
Delete action. Movement and deletion participate in undo/redo and remain
session drafts until Save/Save As succeeds. Moving an existing image translates
its original PDF matrix, preserving its pixels, orientation, and z-order.
Images nested inside a Form XObject remain non-editable because their transform,
clip, and resource scope cannot yet be rewritten safely.

Draft geometry keeps immutable PDF source bounds separate from the layout frame
and laid-out content bounds. Existing PDF text seeds its frame from the Qt text
width so the original line fits. When its inline editor opens, TrePDF
transiently extends that frame from its current left edge to the remaining
column width, or to the inferred right page margin when no same-row text blocks
it. The wider frame is stored after a real text edit, a paragraph-format
change, or an explicit resize; opening and closing the editor without changes
leaves the PDF geometry unchanged. The selected frame exposes four edge resize
handles; dragging an edge reflows text live and stores the geometry change in
session undo/redo.

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
that needs replacement or deletion, translates moved source images, lays out
multiline/rich-text runs, calls
`FPDFPage_GenerateContent()`, and only clears undo history after the saved PDF
has reopened successfully.

The Edit ribbon can also insert a blank page immediately after the current page
using its page size, or delete the current page while keeping at least one page
in the document. These page operations participate in undo, redo, and discard
like other session drafts. Save and Save As persist the resulting page order
without changing the source file during Save As.

Editable source text is currently limited to top-level page objects with Fill,
Stroke, or Fill+Stroke rendering. Text used as a clipping path or nested inside
a Form XObject remains viewable but is not offered as safely editable content.
Top-level image objects support position changes and deletion; TrePDF does not
expose their pixels as editable text and does not permit either operation inside
a Form XObject.

Edit mode also shows a collapsible text-formatting panel on the right with font
family, size, color, bold, italic, and underline controls. Font family, bold,
italic, underline, and color apply immediately to selected draft text and
support undo/redo. While the inline editor is active, bold, italic, and
underline affect only the highlighted range; with a collapsed caret they set
the format of text typed next without restyling existing text. Using one of
those controls on selected text automatically opens the inline editor with the
caret at the end. Font size is currently UI-only.

## Source documentation

- [`docs/source-file-map.md`](docs/source-file-map.md): architecture, ownership,
  thread boundaries, main runtime flows, and an index of every current QML/C++
  implementation file.
- [`docs/qml-file-guide.md`](docs/qml-file-guide.md): detailed responsibilities,
  inputs, outputs, lifecycle, and C++ relationships for all 27 QML files.
- [`docs/cpp-file-guide.md`](docs/cpp-file-guide.md): detailed responsibilities,
  ownership, threading, persistence boundaries, and tests for all 16 `src/` C++
  implementations, the C++ test file, and support headers.
- [`docs/overlay-architecture.md`](docs/overlay-architecture.md): the current
  per-page overlay tree, stacking order, coordinate systems, mode gates, input
  routing, texture synchronization, and pooling cleanup.
- [`docs/text-editing-pipeline.md`](docs/text-editing-pipeline.md): the complete
  editable-text discovery, geometry, font, baseline, overlay, and
  draft-rendering flow.

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
