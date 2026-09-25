# TrePDF

TrePDF is a Qt Quick Controls desktop PDF reader powered by PDFium. The app
supports multiple PDFs in independent tabs, four page-display layouts,
asynchronous rendering, per-tab whole-document text search and left/right view
rotation, rich-text content edits, image insertion, Save/Save As, and an
ID-based localization system with Vietnamese enabled by default.

The Sign tab adds visible signatures and initials drawn with a mouse/trackpad,
typed in a chosen style, or imported from a local image with optional white
background removal. It also places formatted dates and check/cross marks.
Signatures and initials can be saved in a local library for reuse. Click the
page to place the prepared item, then move, resize, rotate, or delete it using
the existing image controls. These edits support Undo/Redo, Discard, Save and
Save As; saved items reopen as ordinary PDF images. The library stores up to
32 entries separately from document history. This is visual signing; certificate
signing, signature verification, and online signing requests are not implemented.

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
scroll position, page-display mode, separate-cover and split-view state, search
query/results, and formatting-panel state. The View tab exposes Page Display as
a compact dropdown: four layout actions, a Separate Cover toggle available only
for the two Facing layouts, and a Split action with Vertical and Horizontal
variants in a nested menu. Single Page and Facing materialize only the active
page or spread, while both Continuous layouts virtualize their stacked rows and
derive the current page from the page occupying most of the viewport. Vertical
and Horizontal Split provide two independently scrollable, zoomable, navigable
views of the same PDF. Only the active tab creates page delegates and textures.
Every viewport supports horizontal scrolling when zoomed pages or facing spreads
exceed its width. A horizontal scrollbar remains visible while there is overflow;
it disappears when the content fits, including after zooming out.
The fixed reader bar at the bottom provides first/previous/next/last navigation,
direct one-based page entry, bounded Previous/Next View history, four quick
page-layout buttons, a logarithmic zoom slider, and 10–500% zoom presets. In
Split mode these navigation and zoom controls follow the pane most recently
activated by the user. View history is isolated by tab and pane and also tracks
page or zoom changes initiated by ribbon commands, search navigation, and page
scrolling after the initial fit completes.
Opening an already-open canonical path activates its existing tab, and Save As
cannot overwrite a PDF used by another open tab.
Password-protected PDFs prompt for a masked password before adding a tab. A wrong
password leaves the prompt open for another attempt; Cancel or Escape skips that
file, including when opening several PDFs. Accepted passwords stay in memory and
are passed to the independent render, search, form, scan, export and save workers.
Without a pending password change, Save/reopen retains the password and source
protection. Encrypted operations that require the byte-level annotation/AcroForm
canonicalizer remain unsupported on that path and fail before replacing the
destination file.

The **Protect** tab, between Sign and Convert, adds an opening password to an
unencrypted PDF and changes the password of an unlocked PDF without permission
restrictions. **Set password** or **Change password** asks for a new password
and confirmation, with an option to show the entries. The current session has
already authenticated an encrypted PDF, so it does not ask for the old password
again. Apply stages the password for the current tab; Save or Save As writes it
with AES-256 encryption. The new password takes effect only after a successful
save, and Save As leaves the source file's password unchanged. Discard clears
the pending change and keeps the original password, while undoing content edits
retains the pending change. A failed save preserves both the pending change and
the destination file. Password-only changes preserve saved content, annotations,
and forms. Changing passwords on PDFs with permission restrictions, removing a
password, and configuring printing or editing restrictions are not implemented.

The Protect tab also provides **Watermark → Add / Update / Remove**. Watermarks
can use Unicode vector text or a snapshotted local image, with opacity, rotation,
nine positions, front/behind placement and page ranges. Changes support
Undo/Redo, Discard and Save/Save As; valid TrePDF watermarks remain manageable
after reopening. Other applications' watermarks are preserved. Save other page
edits before managing watermarks, and save/discard pending watermarks before
editing page content. See [watermark pipeline](docs/watermark-pipeline.md) for
the persistence contract and limits.

The View tab searches the current PDF text asynchronously across its source
pages. Typing is debounced, a newer query cancels the older scan, and stale
results are rejected after a document or page-structure revision. Matches on
visible pages are highlighted in normalized page coordinates; the active match
uses orange, while Previous and Next cycle through the result set and center the
active match in the viewport, including after view rotation. Pending input and
results remain isolated per tab; a background completion never changes the page
after the user leaves View. Search text is
limited to 256 UTF-16 code units, result geometry shown by QML is bounded per
page, and bounded scan caps report a limited result set instead of retaining
unbounded data. Search reads the saved source text, so a new edit-history change
clears any existing query and highlights.

In Edit mode, selecting editable text opens contextual Edit and Delete actions.
The Link menu's **Auto-create links from URLs** scans the saved text layer across
the document on a background worker, recognizes HTTP/HTTPS and `www` addresses,
and skips regions already covered by PDF links. Save other pending PDF changes
before running it. Newly created links are one undoable batch; Discard removes
them, and Save/Save As writes invisible standard `/Link` URI annotations without
changing the page text or its appearance. Orange frames show the draft regions
in Edit; switch to View to open a link through the existing confirmation dialog.
The menu can cancel an active scan. Scans without a text layer are unsupported;
bounded scans report a limited result when content exceeds the safety limits.
Links remain independent page regions if text is subsequently edited or moved.
**Remove web links** removes editable HTTP/HTTPS link annotations across the
document, including newly created unsaved links, as one undoable batch. Internal
destinations, other URI schemes, comments, and protected links are preserved.
Save other pending edits first. Removed links stop accepting clicks immediately;
Save/Save As persists the removal without changing page text. The scan can be
cancelled, and Undo or Discard restores removed links.

**Add or edit a link** enables rectangle placement and displays existing link
regions. Double-click a link or use its Properties action to change its HTTP/HTTPS
URL or destination page in this PDF. Link properties include visible/invisible
borders, color, thickness, solid/dashed/underline styles, and the PDF highlight
mode. Links support dragging, eight resize handles, keyboard movement, deletion,
undo/redo/discard, and Save/Save As. Internal destinations track source pages
through insertion/deletion; saving fails safely if a surviving link targets a
deleted page. Save newly inserted pages before using them as a link source or
destination. Protected links and unsupported source actions/appearances remain
read-only. Web links use the existing external-link confirmation.

Changes remain undoable session drafts until Save or Save As writes them into a
PDF. Compatible text objects on the same visual line are grouped, then nearby
lines with matching font family and size, left alignment, and regular spacing
form one editable paragraph. Each line retains its bold, italic, underline,
and stretch in character spans; stretch may differ by one percentage point.
Source text with overlapping bounds stays separate, and grouping does not cross
columns or intervening content. Paragraphs retain line breaks
and every source object path for draft rendering and persistence. Editing uses
the existing Qt reflow; moving a paragraph alone preserves its original text
matrices and line layout.
Justify reflows the selected paragraph's source lines as continuous text while
preserving paragraph breaks entered by the user. Saved PDFs retain the spacing
between justified words without stretching the letters.

The Comment ribbon starts with a Select tool in the same position as the Edit
ribbon, followed by Sticky Note, Free Text, Highlight, Squiggly, Underline,
Strikeout, Stamp, Rectangle, Oval, Line, Polygon, Polyline, and Pencil tools
plus a collapsible, whole-document comments panel. Entering the Comment tab
activates its Select tool by default. This mode shares the normal selection
interaction but filters hit testing, direct selection, keyboard traversal, and
history restoration to comment candidates, so page text, images, Edit shapes,
and form objects cannot be selected through it. Supported comments and replies,
including annotations already in the PDF, can be selected and have their text
edited in the panel. Markup uses a compact curated color
grid and a system picker for custom colors. Highlight, Underline, Squiggly, and
Strikeout are source-text-aware: their drag rectangle is intersected with
non-whitespace glyphs from the PDF text layer, then snapped into per-line
rectangles and QuadPoints.
Nearly horizontal drags use a three-pixel hit-test band, so small differences
in pointer height do not silently discard the gesture. If the asynchronous text
scan is still running at release, TrePDF keeps the page, selection, tool, and color,
prioritizes that page, and creates the draft automatically when geometry is
ready.
Dragging only across whitespace or an area without source text creates no
annotation. Invisible OCR text layers are accepted for this hit testing but do
not become selectable or editable text candidates. Image-only scans without a
usable text layer therefore do not support these text-markup tools. The current axis-aligned
geometry accepts upright horizontal glyphs only; rotated or vertical glyphs
remain unsupported until oriented QuadPoints are implemented. Pages with more
than 50,000 text-layer characters skip the per-glyph cache for memory safety and
do not support glyph-aware text markup. Free Text places a bounded text box by click or drag;
its text is edited in the comments panel and is saved as a standard
`/FreeText` annotation with semantic `/Contents` plus a wrapped,
Unicode-capable appearance. Root comments can receive replies;
each root has a dropdown control that expands its replies at one visual level,
and reply-to-reply is not offered. Annotation-state transition records created
by readers such as Foxit (`/State` with `/StateModel /Review` or `/Marked`) are
not shown as comments, while remaining part of the internal thread graph for
safe deletion and permission checks. Root and reply cards expose a three-dot action menu
with a cascading Set Status submenu and Mark with Checkmark. Both controls create
independent undoable drafts and Save/Save As writes author-specific PDF state
transition annotations. Reviewer-name case variants reuse the exact spelling
established by the document, including on replies without their own prior state.
The canonical exact-case history is preferred, and its terminal `/IRT` node
restores each Review and Marked state after reopen without exposing transition
logs as comments. Active status and
checkmark values appear as compact badges on their root or reply comment card.
Reply annotations remain available in this panel but their standalone appearances
are hidden from the PDF page viewer.
Square, Circle, Line, Polygon, and PolyLine comments use shared semantic stroke,
interior, opacity, border, effect, and line-ending properties. Save writes
standard `/Rect`, `/L` or `/Vertices` geometry and a matching vector `/AP`.
Selecting a drawing comment in the Edit tab opens a dedicated properties panel
for stroke color, optional fill color, and opacity; the Comments panel remains
focused on comment text and threads. Each accepted property change is
independently undoable, and preview updates immediately. Added drawing comments
support this before their first Save. All six reopened TrePDF-managed drawing
subtypes also support it when their captured appearance is safe;
external/custom appearances remain property-read-only. Property-only Save
preserves `/BS`, `/BE`, `/LE`, and unrelated dictionary entries.
Valid source Square, Circle, Line, Polygon, PolyLine, and Ink comments can be
dragged or moved with arrow keys, resized with eight handles, deleted,
undone/redone, discarded, saved, and reopened. Source geometry updates patch
`/Rect` plus `/L`, `/Vertices`, or `/InkList` after PDFium
serialization. Pure moves and external/custom appearances preserve `/AP`;
resized TrePDF-managed comments receive a matching canonical `/AP` when all
captured properties are supported and the indexed source appearance still
matches at Save time.
Each Pencil drag creates one `/Ink` annotation. QML samples the freehand path at
least one logical pixel apart, keeps at most 4,096 raw points, and continues
tracking the final endpoint at that cap; the controller validates and
deduplicates it, simplifies it at roughly 0.75 PDF
point, and caps the stored result at 1,024 points. Rehydrated Ink supports
multiple strokes from `/InkList`; source scanning is bounded to 64 strokes,
4,096 points per stroke, and 32,768 points total. Malformed or oversized source
Ink is kept as a generic read-only comment. Ink drafts and their gesture preview
share the Qt Quick Shapes `InkDrawingView`; Save maps every normalized point
through `FPDF_DeviceToPage()` and writes `/Subtype /Ink`, `/InkList`, `/BS`, `/C`,
`/CA`, the Print flag, and a vector `/AP` with round caps and joins. Source Ink
follows the same editable geometry lifecycle, affinely transforms every stroke,
and patches `/Rect` plus `/InkList`. Stamp offers 24 presets in four groups:

- Dynamic: Approved, Revised, Reviewed, Received, and Confidential.
- Standard: Approved, Void, Verified, Revised, Reviewed, Received, Final,
  Expired, Emergency, Draft, Confidential, and Completed.
- Sign Here: Accepted, Witness, Sign Here, Rejected, and Initial.
- Additional: For Comment and Not Approved.

Dynamic stamps capture the current comment author and date/time when placed;
contents edits, undo/redo, and Save/reopen keep that printed snapshot fixed.
The shared `PDFStampPresets.h` builds the colored background and white vector
artwork, including icons and Unicode text outlines, for preview and saved AP.
A click uses a 160-by-50-point frame and a drag sets a custom frame; new Stamp
drafts can be moved, resized with eight handles, deleted, undone/redone, and
saved as standard `/Stamp` annotations with `/Name`, semantic `/Contents`, the
Print flag, and a vector `/AP`. Eligible existing Stamp annotations also support
drag/arrow-key movement, eight-handle resize, contents, replies, deletion,
undo/redo/discard, and Save/Save As. Saving source Stamp geometry patches
`/Rect` while preserving its original `/AP`, appearance `/BBox`, and unrelated
dictionary entries.
Caret, FileAttachment, Sound, Redact, and PDF 2.0 Projection are still indexed
read-only for reading and navigation.
Comment changes are per-tab session drafts with
undo, redo, discard, selection, and deletion support. Save and Save As persist
them as standard PDF annotations with geometry, color, contents, print
visibility, and `/IRT` reply links; annotation-only saves do not regenerate the
page content stream. Each comment or reply is limited to 700 characters, with
the panel showing a live counter before commit. Existing supported annotations
are rehydrated for navigation and display, including their author and
modification date (or
creation date when no modification date is present). Editing updates the source
annotation's `/Contents`, modification date, and rich-text value when present.
Comment line endings are normalized to PDF CR on save and LF in the editor.
New Sticky Notes and replies receive a vector normal appearance; existing
appearances are preserved, while a missing Sticky Note appearance is repaired
when that annotation is edited. An edited Free Text comment
receives TrePDF's normalized wrapped Unicode appearance so stale text is not
shown. Popup, thread links, geometry, and other metadata remain intact.
Read-only and LockedContents annotations remain protected. Deleting
a root comment also removes its replies so the saved document cannot contain
an orphaned thread.

The Shapes dropdown offers rectangles, ovals, lines, arrows, triangles, and
diamonds. After choosing a shape, drag over a page to create a vector session
draft with a black outline and transparent fill, then select it to move, resize,
or delete it. Shapes have eight side/corner resize handles, and selecting one
in Edit opens Shape Properties for stroke color, fill/no fill on closed shapes,
and opacity. Near-horizontal and near-vertical lines and arrows snap to their
axis. Shape creation, geometry, appearance, and deletion participate in undo,
redo, and discard. Save and Save As write shapes as PDF path objects, including cubic
Bézier ovals, so they remain sharp when zoomed and keep the same placement on
cropped or rotated pages. Newly saved TrePDF shapes carry `TrePDFShape` metadata;
validated top-level paths reopen as editable shapes with the same properties,
move/resize/delete, history, and persistence. Older unmarked top-level paths
also become editable when their geometry and style match a canonical rectangle,
triangle, or diamond; editing and saving them adds `TrePDFShape` metadata.
Arbitrary PDF paths and paths inside Form XObjects are excluded. Rewriting a source shape
preserves its position in the page's drawing order; Edit shapes remain page
content, separate from drawing-comment annotations.

The Image tool changes the page cursor to a placement `+`. Clicking a page opens
the local image picker; the chosen PNG, JPEG, BMP, GIF, WebP, or TIFF is decoded
and snapshotted immediately, displayed as a resizable session draft, and added
to undo/redo. The draft can also be dragged to a new position. Save/Save As
embeds the pixels as a PDF image object, preserving the editor geometry on
cropped and rotated pages.

Existing top-level PDF images can be selected in Select mode, dragged, resized
with eight side/corner handles, or removed with Backspace, Delete, or the
contextual Delete action. Selecting an image in Edit opens a dedicated Image
Properties panel on the right with Rotate left 90°, Rotate right 90°, and
Lock aspect ratio for both added and eligible source images. The panel hides
when the selection is no longer an image or the user leaves Edit. The lock starts enabled, stays independent
per tab, and does not create a draft or become a saved PDF property. Unlock it
to resize width and height independently. Movement, resizing, rotation, and deletion
participate in undo/redo and remain session drafts until Save/Save As succeeds.
Moving, resizing, or rotating an existing image transforms its original PDF
matrix,
preserving its pixel resource, drawing order, and original rotation/reflection
within the new frame. Resizing and rotation require a finite, nonsingular matrix,
no custom clipping, and an image entirely inside the visible page; other top-level images
retain their existing move/delete support. Rotation swaps physical width and height,
keeps the center where possible, and shifts the frame inward at page edges. The
rotation buttons are disabled if the rotated frame cannot fit on the page without
scaling. Each click is one undoable command.
Images nested inside a Form XObject remain non-editable because their transform,
clip, and resource scope cannot yet be rewritten safely.

Draft geometry keeps immutable PDF source bounds separate from the layout frame
and laid-out content bounds. Existing PDF text seeds its frame from the Qt width
of its longest original line so each line fits. When its inline editor opens,
TrePDF transiently extends that frame from its current left edge to the remaining
column width, or to the inferred right page margin when no same-row text blocks
it. The wider frame is stored after a real text edit, a font-size or
paragraph-format change, or an explicit resize; opening and closing the editor without changes
leaves the PDF geometry unchanged. The selected frame exposes four edge resize
handles; dragging an edge reflows text live and stores the geometry change in
session undo/redo.

Each session draft stores text, character-format spans, paragraph formats, its
layout frame, and content bounds. Undo and redo snapshot the complete rich-text
draft. The inline rich-text editor supports multiple lines, wraps/reflows inside
the layout frame, renders mixed character styles, and applies formatting to the
selected text range.

Font resolution prefers a usable embedded PDF face, then an exact system
face, the app's font cache, Google Fonts, CTAN, and the existing fallback.
Fonts under `src/assets/fonts` remain bundled for the fallback paths.
The save writer can reuse the original PDF font when a replacement
run only uses characters already present in its source text, and can use a
compatible PDF standard font for ASCII. Edited non-ASCII text is normalized to
NFC; newly introduced Vietnamese text inheriting an Arial/Helvetica face is
written with an exact complete local face if the embedded subset lacks glyphs.
Eligible newly selected macOS system fonts
whose exact regular/bold/italic face is available are resolved with CoreText,
snapshotted before the save worker starts, and embedded as CID Unicode TrueType
fonts. New font programs retain their shaping and kerning tables so editing
after reopen preserves advances; this can produce larger files than subsetting.
Original PDF font resources remain untouched. TTC
faces are materialized individually so PDFium does not select the wrong face.
The font picker probes the exact active style and excludes known
incompatible system faces such as color, CFF/CFF2, variable, bitmap-only, or
embedding-restricted fonts. A filtered source font can still be displayed as
the current PDF font without being offered in the popup. Unedited objects keep
their original font resources. Missing glyphs, system faces larger than 64 MiB,
and other failures discovered by the final content-specific check stop the
save with an error and keep the draft intact.

The download service keeps Google Fonts first and checks CTAN only when the
family is absent from that catalogue. Its reviewed CTAN catalogue includes
TeX Gyre Termes, Heros, Pagella, Schola and Cursor in four styles, downloaded
through `https://mirror.ctan.org/` under the GUST Font License. If the selected
mirror is blocked or unavailable, bounded retries use two explicitly listed
official CTAN HTTPS mirrors. Validated fonts
are saved atomically in the app cache and registered with
`QFontDatabase::addApplicationFont()`; subsequent uses reuse that cache.
Static CTAN OpenType/CFF fonts use a dedicated PDF staging step that writes
`FontFile3 /OpenType` without changing the font bytes. Unknown families keep
the existing fallback; no arbitrary font websites are searched. See
[font providers](docs/font-providers.md) for the provider interface, cache,
licensing and supported formats.

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
that needs replacement or deletion, transforms moved/resized/rotated source
images, lays out multiline/rich-text runs, calls
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
Top-level image objects support movement, eligible resizing and rotation, and
deletion; images inside a Form XObject remain protected.

Edit mode also shows a collapsible text-formatting panel on the right with font
family, size, color, bold, italic, and underline controls. Font family, bold,
italic, underline, and color apply immediately to selected draft text and
support undo/redo. While the inline editor is active, bold, italic, and
underline affect only the highlighted range; with a collapsed caret they set
the format of text typed next without restyling existing text. Using one of
those controls on selected text automatically opens the inline editor with the
caret at the end. Font size accepts whole PDF points from 1 to 512 and reflects
the selected text independently of zoom; mixed sizes show an empty value.
Changing size on a selected text object formats the whole object. In an active
editor it formats the selected range, or only subsequently typed text at a
collapsed caret. Size changes preserve local editor undo/redo, reflow live,
and persist through Save/Save As.

The **Convert** tab exports detected tables to `.xlsx`, with one worksheet per
table and per-document cancellation. The ribbon shows only action buttons;
results use the app status message. Save PDF changes before exporting.
Text-based tables need a closed ruled perimeter and at least two rows and two
columns. Rectangular merged cells are preserved; scans without a text layer,
borderless tables, malformed merged regions, and rotated text are unsupported.
Cell values are written as
text to preserve Unicode, leading zeros, and literal formula-like strings.
Export uses a replaceable `SpreadsheetWorkbook` interface; libxlsxwriter 1.2.4
is the initial, statically linked backend bundled under `third_party/`.

## Source documentation

- [`docs/excel-export-pipeline.md`](docs/excel-export-pipeline.md): table detection,
  export transactions, cancellation, limits, backend replacement, and tests.

- [`docs/source-file-map.md`](docs/source-file-map.md): architecture, ownership,
  thread boundaries, main runtime flows, and an index of every current QML/C++
  implementation file.
- [`docs/qml-file-guide.md`](docs/qml-file-guide.md): detailed responsibilities,
  inputs, outputs, lifecycle, and C++ relationships for all 36 QML files.
- [`docs/cpp-file-guide.md`](docs/cpp-file-guide.md): detailed responsibilities,
  ownership, threading, persistence boundaries, and tests for all 26 `src/` C++
  implementations, 22 headers, and the C++ test file, including the separate
  implementation files of `PDFEditController`.
- [`docs/overlay-architecture.md`](docs/overlay-architecture.md): the current
  per-page overlay tree, stacking order, coordinate systems, mode gates, input
  routing, texture synchronization, and pooling cleanup.
- [`docs/comment-annotation-pipeline.md`](docs/comment-annotation-pipeline.md):
  the current PDF comment/annotation scope, indexing, draft/history, geometry,
  replies, rendering, persistence, interoperability invariants, and roadmap.
- [`docs/text-editing-pipeline.md`](docs/text-editing-pipeline.md): the complete
  editable-text discovery, geometry, font, baseline, overlay, and
  draft-rendering flow.

## Build

Install Qt 6.8 or newer and provide a PDFium build that contains `fpdfview.h`
and the `pdfium` library. Like PDFium, qpdf 12.4.1 is bundled as a prebuilt macOS
arm64 library with public headers under `third_party/qpdf` (macOS 13 or newer).
CMake imports it as `QPDF::QPDF` and copies the library into the app bundle.
Normal configure/build steps download no dependencies and require neither qpdf
sources nor a qpdf CLI installation; the application remains C++17. Use
`-DQPDF_ROOT=/path/to/qpdf` to select another compatible package.

To rebuild qpdf when upgrading the dependency, run
`scripts/build-qpdf-macos.sh <Qt-prefix> [output-directory]`. Only this separate
step needs C++20 and the Qt SDK's static JPEG library. See
[`third_party/qpdf/README.trepdf.md`](third_party/qpdf/README.trepdf.md) for
provenance, checksums, and offline rebuild instructions.

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

For a separate Windows Qt/MinGW profile, see
[`docs/windows-clion.md`](docs/windows-clion.md). Windows SDK paths and downloaded
libraries stay in ignored local files; the macOS setup below is unchanged.

Qt is already configured for this local SDK:

```sh
/Users/luongvanhuy/Qt/6.11.0/macos
```

PDFium is configured from `third_party/pdfium` and qpdf from `third_party/qpdf`.

Open the repository folder in CLion and use the default `Debug` CMake profile.
The project configures Qt, PDFium, and qpdf from the local paths above, then creates
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
