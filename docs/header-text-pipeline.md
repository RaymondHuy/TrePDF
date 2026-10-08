# Header text

Header > Add Text arms the placement cursor. Click inside the gray header area
to open a textbox on the page at that position. The right-side panel then opens
with font family, size, bold/italic/underline, text color, and all-pages or
1-based range selection such as `1-3, 5`. Formatting updates the complete
pending header textbox live. Enter the text on
the page and press Apply in the panel. Duplicate page
numbers are applied once. Cancel closes the panel without creating drafts.

The Header tab shows a translucent gray guide in the first 100 PDF points of
each page. This guide is a non-interactive QML overlay and is never saved.
Header text is ordinary page content and remains visible outside the Header tab.

## Ownership and lifecycle

- `AppRibbon.qml` emits `headerTextRequested`; `Main.qml` prepares the active
  workspace and arms placement.
- `DocumentWorkspace.qml` owns `HeaderTextPlacement.qml`, holding temporary
  text, page and PDF-point geometry independently of virtualized delegates.
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
drafts. Save/Save As writes searchable PDF text; Save As leaves the original
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
