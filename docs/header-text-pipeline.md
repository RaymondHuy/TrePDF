# Header text, images and page numbers

Header > Add Text arms the placement cursor. Click inside the gray header area
to open a textbox on the page at that position. The right-side panel then opens
with font family, size, bold/italic/underline, text color, and all-pages or
1-based range selection such as `1-3, 5`. Font family, size and style update the
complete pending header textbox live. Choosing a color sets the color for new
text at the current caret; existing characters retain their colors. Enter the text on
the page and press Apply in the panel. Duplicate page
numbers are applied once. Cancel closes the panel without creating drafts.

The Header tab shows a translucent gray guide in the first 100 PDF points of
each page. This guide is a non-interactive QML overlay and is never saved.
Header text is ordinary page content and remains visible outside the Header tab.

## Ownership and lifecycle

- `AppRibbon.qml` emits `headerTextRequested`; `Main.qml` prepares the active
  workspace and arms placement.
- `DocumentWorkspace.qml` owns `HeaderTextPlacement.qml`, holding temporary
  text, character colors, caret and PDF-point geometry independently of virtualized delegates.
  `HeaderTextOverlay.qml` captures clicks only in the gray region while armed,
  then lazily loads a scaled inline editor above page interactions. The panel
  appears only after placement and contains formatting, page selection and Apply/Cancel.
  Input remains separate from document drafts. Switching tabs/documents,
  changing page structure/history, or starting Save closes the panel. Save
  does not implicitly apply its input.
- Apply calls `PDFEditController::addHeaderText(text, pages, xPoints, yPoints, widthPoints, format)`. Failures retain
  the input and show a localized message. Success closes the panel.
- `PDFEditControllerHeaderText.cpp` builds and validates all target drafts
  before changing controller state. The additions form one `ObjectDraftBatch`
  history command. Undo/Redo affects the complete batch; other existing drafts
  retain their own history. Discard clears the additions normally.

## Placement and persistence

The clicked position is converted to PDF points on the unrotated page surface
and reused on every target page. Clicks near the right/bottom edges move inward
to leave at least 48 points of width and 18 points of height. The initial textbox
uses the Edit Add Text proportions: 25% of the page width and 4% of its height,
constrained by the page edge and the remaining header area. Its height grows
with the entered lines up to the header boundary. It defaults to black, 12-point
text; the panel supports integral sizes from 1 to 512 pt, available font faces,
bold, italic, underline and opaque text colors. The language filter follows
Edit's font catalogue and respects the selected bold/italic style. Text wraps
within the frame width; narrower target pages reduce the width.
The editor scales as a whole with zoom to preserve line wrapping. Every selected page must fit the complete text inside its
100-point header area. Page geometry uses the existing crop/rotation-aware
coordinate mapping. Zoom and view rotation do not change saved geometry.

The existing added-text preview and transactional text writer handle these
drafts. Character colors are stored as rich-text spans and survive batch
Undo/Redo and Save/reopen. The editor keeps color changes with their inserted
text in local undo history. Moving the caret picks up the adjacent text color.
Save/Save As writes searchable PDF text; Save As leaves the original
file unchanged. A failed save retains the drafts. Page insertion/deletion
uses the existing candidate/draft remapping and history snapshots.

After Save/reopen, the text is ordinary PDF content and can be edited per page
through Edit. Header batches do not carry managed group metadata: another Apply
adds another batch, and does not replace an earlier header. The range is a
snapshot of the page order at Apply; pages inserted later receive no automatic
header. Footer currently has no editing commands.

Text is limited to 700 UTF-16 code units after newline/NFC normalization, with
at most 10,000 selected pages and 1,000,000 total text code units per batch.
Page-range parsing is bounded separately. Invalid ranges, empty text, unavailable
font glyphs, restricted editing, and overflow reject the complete request before
any page changes. Header creation also respects the existing save and pending
watermark guards.

The optional `format.colors` list contains one opaque color per UTF-16 code unit
of the input text. Missing or empty lists preserve the uniform-color API.
Newline and NFC normalization map colors with the text; a composed grapheme
uses its base character's color. Malformed color lists reject the entire batch.

## Verification

`tst_PDFEditControllerHeaderText.cpp` covers all/custom pages, deduplication,
atomic rejection and history, independent sessions, Unicode Save/Save As,
page insertion/deletion, cropped and rotated pages, save/permission/watermark
guards, and failed-save retention. `tst_PDFEditControllerHeaderTextUi.cpp`
covers Main/ribbon/panel wiring, actual mouse placement and style buttons,
the color picker lifecycle, pending size edits, Apply/Cancel, validation and
transient input lifecycle. Format tests verify font, size, style, color and
underline geometry after Save/reopen. Native macOS checks cover translated panel layout, selected pages,
Undo/Redo, scrolling/zoom, Save/reopen and normal shutdown.

## Header images

Header > Image uses the Edit image icon and local file filter. Click in the
100-point gray area to choose an image; canceling the picker creates no draft.
The image is centered at the click and clamped into the header with its aspect
ratio intact (initial width at most 150 pt/25% page width and height at most
80 pt). The right panel appears after successful decoding. Drag and eight
handles move/resize it within the header. Aspect lock defaults on; quarter
turns preserve physical size and are disabled when the swapped frame cannot fit.
Select all pages or a 1-based range, then Apply.

`HeaderImagePlacement.qml` owns temporary geometry and a bounded preview URL;
`PDFEditController` owns the decoded full-resolution QImage. `HeaderImageOverlay.qml`
loads only for the selected visible page, so pooling does not lose placement.
`HeaderImagePanel.qml` owns only panel input. Main binds the file picker to the
active session/placement and discards stale results. Image and text placement
cancel each other. Cancel, Escape, tab/session/history/page-structure changes,
Save and workspace destruction clear temporary input without creating history.

`prepareHeaderImage()` accepts local files using the Edit limits (64 MiB,
40 million pixels), applies orientation and snapshots ARGB32 pixels. Its PNG
preview has a maximum dimension of 1024. `addHeaderImage()` reuses the page-range
parser (at most 10,000 pages), checks permissions, save/watermark guards, finite
geometry and every target page before making a single `ObjectDraftBatch`.
All targets use the same physical frame inside the first 100 pt; smaller pages
that cannot fit reject the entire request and retain the pending input.
Decoded pixels are shared between drafts and never reopened from the source
path during Save. The existing image writer handles crop/rotation, alpha,
quarter turns, undo/redo/discard, Save/Save As and failed-save retention.
After reopen, each image is ordinary page content editable independently in Edit;
there is no persistent Header group or automatic insertion onto later pages.

`tst_PDFEditControllerHeaderImage.cpp` verifies snapshots, atomic batch history,
range/geometry rejection and Save/reopen; `tst_PDFEditControllerHeaderImageUi.cpp`
checks pending placement, page selection, frame controls and lifecycle behavior.


## Header page numbers

Header > Page numbers arms placement in the same gray 100-point header. Clicking
opens a preview at that PDF-point position and a right-side panel. The panel
accepts a first/last PDF page (inclusive), an optional Roman start and an optional
Arabic start. All positions are 1-based PDF page indices. Roman numbering starts
at `i`; Arabic numbering restarts at `1`, including when following Roman numbers.
When both are enabled, the Roman start must precede the Arabic start. Pages
before the earliest enabled start, or outside the outer range, remain unnumbered.
For example: range 2–10, Roman start 2, Arabic start 6 yields `i, ii, iii, iv` on
PDF pages 2–5 and `1, 2, 3, 4, 5` on pages 6–10. Either numbering style can be
used alone. Roman numbering is limited to 3,999 pages; batches to 10,000 pages.

`HeaderPageNumberPlacement.qml` owns temporary settings independently of pooled
pages; `HeaderPageNumberOverlay.qml` creates previews only for visible delegates.
`headerPageNumberText()` shares sequence generation with the backend. The preview
uses the default Header font at 12 pt in black, at a constant physical position
on each target page. `HeaderPageNumberPanel.qml` validates ranges before Apply.
All three Header placement tools cancel each other. Escape, tab/session/history/
page-structure changes and Save abandon unapplied numbering.

`addHeaderPageNumbers()` validates the whole sequence and uses the shared
`addHeaderTextBatch()` transaction, including font, permission, geometry, save
and watermark guards. Every page must fit before any drafts are created. Apply
is one undoable batch; Save/Save As writes ordinary searchable text. Numbers are
snapshots of page order at Apply and do not automatically renumber after later
page insertion/deletion. Reopened numbers are independently editable through Edit;
applying again adds a new batch instead of replacing previously saved numbers.

`tst_PDFEditControllerHeaderPageNumbers.cpp` covers restart/stop boundaries,
Roman conversion/limits, invalid ranges and atomic geometry failure, history,
Save As/source preservation/reopen, and Main/ribbon/placement/panel wiring.
