# Header text, images and page numbers

Header > Add Text arms the placement cursor. Click inside the gray header area
to open a textbox on the page at that position. The right-side panel then opens
with font family, size, bold/italic/underline, text color, and all-pages or
1-based range selection such as `1-3, 5`. Choosing a font family applies only to
the selected characters, or to new text at the caret when nothing is selected.
Existing text outside the selection retains its font. Size and style update the
complete pending header textbox live. Choosing a color sets the color for new
text at the current caret; existing characters retain their colors. Enter the text on
the page and press Apply in the panel. Duplicate page
numbers are applied once. Cancel closes the panel without creating drafts.
Edit > Add Text shares this initial-family selection through `headerTextFontFamily()`.
The initial family prefers an available Arial face (bundled, system or downloaded), then
an available bundled/system text family. Native UI aliases that cannot be saved
are excluded from this fallback.

The Header tab shows a translucent gray guide in the first 100 PDF points of
each page. This guide is a non-interactive QML overlay and is never saved.
Header text is ordinary page content and remains visible outside the Header tab.

## Header selection

Entry requires the active PDF to have no unsaved changes. Main first commits
pending inline, panel and form input, then checks the session-wide draft state
(including page operations, links, watermarks and password changes). A blocked
entry keeps the previous ribbon tab/tool and opens a localized notice explaining
the multi-page impact, with Save and Got it actions. Save uses the normal save
flow; after saving or undoing all changes, click Header again to enter. New
edits made while already in Header do not leave the tab. Switching to a dirty
PDF while Header is active returns to View and shows the same notice. Dirty
background PDFs do not block a clean active PDF.

Leaving Header for another ribbon tab also requires a clean active PDF. The
exit notice preserves Header selection, guides and panels, and applies to
programmatic View navigation through Find as well. Pending text/image/page-number
placement counts as a change; arming an empty tool or opening an empty textbox
does not. The notice's Save action submits pending placement using its current
panel settings and page range before normal Save. Invalid placement remains
editable and prevents Save. Users may dismiss the notice to Undo committed
changes or cancel unapplied placement, then switch tabs again. Save failures
retain drafts and keep Header active. The clean-entry boundary and existing
per-session Undo/Redo history are reused; no parallel command stack is needed.

Header also provides Save in a separate File ribbon group after the Header tools,
matching View and using the shared Save icon. Its button is enabled for draft
changes, pending panel/inline edits or unapplied Header content,
and disabled without a document or while saving. It shares the exit notice's
save flow, including validation and submission of pending Header placement.
Cmd/Ctrl+S and Save As use the same Header preparation flow. With synchronization
enabled, saving commits panel and inline input, waits for the current matching
scan, and applies the selected range before writing the PDF or opening Save As.

Entering Header activates Select. It reuses Edit selection, inline text editing,
property panels, move/resize, Delete and Undo/Redo, with eligibility limited to
objects whose complete current frame and content bounds fit inside the first
100 PDF points. Objects crossing the boundary or lying in the body cannot be
selected by clicking, keyboard traversal or history restoration. Source content
and applied Header drafts follow the same rules; unsupported objects retain
their normal Edit restrictions. Changes initially affect the selected object on
its page. Enabled synchronization applies them to matching headers during Save.

`PDFEditController::headerSelectionOnly` scopes `SelectTool` without introducing
a second editing pipeline. Move, resize and image rotation stay within the
header; aspect-locked resize uses the same boundary in the QML preview. Text
uses the existing Edit reflow behavior: if newly entered content extends beyond
the header, finishing the edit clears its Header selection; it remains editable
through Edit. Switching away from Header removes the selection scope.
Starting a Header placement temporarily uses Browse; clicking Select cancels
pending placement and restores Header selection. The selection scope is transient
and is not written into the PDF.

Header Select uses the orange pointer when hovering a text candidate, as in Edit.
Inactive Header, form and link placement MouseAreas are hidden as well as disabled:
Qt Quick can otherwise let their cursor override Select even without accepting input.

`tst_PDFEditControllerHeaderSelection.cpp` checks source and draft eligibility,
boundary-crossing objects, history, geometry, deletion/Save and Main-window
pointer/keyboard/tool transitions.
`guardsHeaderEntryUntilChangesAreSavedOrUndone` in the Header sync UI suite
covers the entry guard, pending inline input, Save/Undo, forms/password drafts
and switching documents.

## Synchronizing similar headers

Selecting a text or image header enables **Apply to similar headers** by default
in the right panel and starts finding matches. Users can turn it off for the
selected object; collapsing and reopening the panel preserves that choice.
The panel scans other pages asynchronously and lists the matching PDF page
numbers. Edit its content, formatting or geometry, then Save copies the change
to all matches or the intersection with a custom 1-based range before writing
the PDF. There is no separate Apply changes button. Save, Cmd/Ctrl+S, Save As
and the Header exit notice's Save action share this flow. Disabling
synchronization saves the selected object's edit without propagating it.
The original page retains its edit even if it is outside the custom range.
An empty or invalid custom range, a failed scan or a rejected batch prevents
saving and leaves the edits and settings available for correction.

The pre-edit matching snapshot also supports deletion. An edited anchor with
synchronization enabled retains its snapshot and page scope after clicking a
blank area, so Save still propagates the change without a visible selection.
Selecting another object starts a new matching context; synchronization applies
to the current selection, rather than accumulating a queue of earlier anchors.
Collapsing the panel does not discard its synchronization choice or page range.

Text content, formatting or explicit resize changes also copy the edited textbox's physical
frame size, so previously narrower members of the same group do not introduce
extra line wrapping. For a managed Header group, movement copies each changed
physical coordinate from the edited member; an independently moved member does
not receive its old offset again. Unchanged coordinates remain intact. Unmarked
source headers retain relative movement. Resized text is remeasured before checking its content extent, so a
shortened textbox moved near the page edge is not rejected using its old width.
Every resulting frame and content extent must fit inside its page's
100-point header; otherwise synchronization leaves the other pages unchanged
and prevents Save. Page numbers
retain their per-value sizing behavior.

Saved Header text always retains its logical paragraph and layout frame through
`TrePDFParagraphLayout`, including plain single-format text. Reopening must not
shrink the frame to the ink bounds or split wrapped text into independent glyphs.
For older marked Header text without this layout metadata, scanning can join
nearby glyphs with the same UUID even when their ink overlaps by up to 0.25 em.
Baselines and increasing origins must agree; duplicate origins, separated blocks
and unmarked overprints keep the normal ambiguity checks. Inferred lines retain
their individual character calibration when joining into a Header paragraph.

Matching uses the snapshot taken before editing. Newly created Header text,
images and page numbers carry a bounded `TrePDFHeader` object mark with a group
UUID and role. The writer preserves this identity through Save/Save As and the
scanner restores it after reopening. For existing PDFs without these marks,
matching requires the same object type and nearby physical geometry (within
2 PDF points). Text also requires identical content and compatible character
formatting, including font size within 0.1 pt, face, style and color. Images
require the same decoded pixel fingerprint. A page with multiple matching
objects is ambiguous and is skipped. Objects outside the header or unsupported
by the normal Edit pipeline are excluded.

Header image Copy/Paste retains physical position and size. Newly pasted images
receive a pixel fingerprint before selection. For upright, unreflected, unclipped
top-level source images, Copy uses the same bounded intrinsic-pixel decoder as
matching; unrotated added images retain their pixels too. Keeping frame geometry
separate from pixel dimensions avoids unnecessary resampling and allows unmarked
copied logos to match before Save and after reopening. Complex transforms and
image-only Forms retain the appearance-rendering clipboard path; strict matching
and managed-group ownership rules remain unchanged.

Page-number groups preserve each page's existing value when formatting or
moving the group. Enlarged numbers expand their frame as needed to stay on one
line, within the page edge. Unchanged embedded font faces and calibration stay
specific to each number. For unmarked PDFs, canonical decimal/Roman values qualify
only when they advance with the PDF page index and share the same numbering
style and formatting. This fallback does not infer Roman/Arabic restarts.
Changing a number's contents cannot be propagated as a group edit.

`PDFEditControllerHeaderSync.cpp` uses a separate single scan worker, cancellation
tokens and source/page revisions. It retains at most 64 MiB of matches and scans
at most 10,000 pages. Source-image fingerprinting is limited to 4 million decoded
pixels per page. During Save preparation, `applyHeaderChanges()` validates the
entire target batch before changing other pages. It combines the selected
object's consecutive edits and the propagated
changes into one `HeaderDraftBatch`, so a single Undo restores the whole group.
Undo/Redo, unrelated history, page structure and session/tool changes cancel
the current matching session. A queued Save is bound to the session, workspace,
source and anchor; changing that context cancels the request. Starting the actual
PDF write clears matching state only after synchronization has completed.
Matching does not create drafts by itself.

`tst_PDFEditControllerHeaderSync.cpp` covers pre-edit matching, ambiguous and
different headers, custom ranges, group metadata after Save/reopen, page-number
preservation, image pixel matching, deletion and atomic history. The Main-window
UI test covers the panel, inline editing and synchronization through Save.

## Ownership and lifecycle

- `AppRibbon.qml` emits `headerTextRequested`; `Main.qml` prepares the active
  workspace and arms placement.
- `DocumentWorkspace.qml` owns `HeaderTextPlacement.qml`, holding temporary
  text, character colors/font families, caret, pending typing format and PDF-point geometry independently of virtualized delegates.
  `HeaderTextOverlay.qml` captures clicks only in the gray region while armed,
  then lazily loads a scaled inline editor above page interactions. The panel
  appears only after placement and contains formatting, page selection and Apply/Cancel.
  Input remains separate from document drafts. Switching tabs/documents,
  changing page structure/history, or starting the actual PDF write closes the
  panel. Header Save preparation submits pending placement with the current
  settings first; invalid placement stays editable and prevents saving.
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
drafts. Character colors and font families are stored as rich-text spans and
survive batch Undo/Redo and Save/reopen. The editor keeps typing-format changes
with their inserted text in local undo history; a selected-range font change is
separately undoable. Moving the caret picks up the adjacent text color and font.
Mixed font families also participate in header fit validation.
Save/Save As writes searchable PDF text; Save As leaves the original
file unchanged. A failed save retains the drafts. Page insertion/deletion
uses the existing candidate/draft remapping and history snapshots.

After Save/reopen, the text is ordinary PDF content and can be edited per page
through Edit or synchronized through Header Select using its group metadata.
Another placement Apply adds a new group and does not replace an earlier header. The range is a
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
uses its base character's color. The optional `format.fontFamilies` list likewise
contains one family per UTF-16 code unit and follows the same normalization.
Each family is validated against only its own characters and the global style.
Malformed lists or unsupported faces/glyphs reject the entire batch.

## Verification

`tst_PDFEditControllerHeaderText.cpp` covers all/custom pages, deduplication,
atomic rejection and history, independent sessions, Unicode Save/Save As,
page insertion/deletion, cropped and rotated pages, save/permission/watermark
guards, and failed-save retention. `tst_PDFEditControllerHeaderTextUi.cpp`
covers Main/ribbon/panel wiring, actual mouse placement and style buttons,
the color picker lifecycle, caret/selected-range family changes, local Undo/Redo,
font/color retention through delegate pooling and Save/reopen, pending size edits, Apply/Cancel, validation and
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
After reopen, each image is ordinary page content editable independently in Edit
or synchronizable through Header Select. Group identity persists, but later
pages receive no automatic insertion.

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


## Footer parity

Footer reuses the Header ribbon, placement, selection, synchronization and Save
pipeline. `PDFEditController::footerMode` chooses the last 100 PDF points instead
of the first 100. Shared `addHeaderText`, `addHeaderImage` and
`addHeaderPageNumbers` APIs accept band-local Y coordinates: zero is the top of
the footer band, even when pages have different heights. Drafts and selection
retain ordinary normalized page coordinates. Move, resize and rotation enforce
both top and bottom bounds of the active band.

The Footer tab defaults to Select and exposes text, images, page numbers and a
separate File/Save group. Save submits pending placement and propagates enabled
similar-footer changes before writing. Entry and exit require saved/undone
changes, including Header-to-Footer transitions; notices name the affected tab.
Shared QML placements are canceled on mode changes, and the save request retains
its originating mode so an asynchronous scan cannot save a different margin.

Footer groups use `footerText`, `footerImage` and `footerPageNumber` roles in the
existing bounded `TrePDFHeader` metadata. Group matching is isolated from Header
roles. Unmarked footers match text/style/geometry or image pixels using physical
coordinates relative to the footer band. Copying an image in Footer preserves
its physical size and band-local Y when pasted onto another page; destination
bounds still apply. Roman/Arabic page-number values remain page-specific during
format/position synchronization.

`tst_PDFEditControllerFooter.cpp` covers different page heights, persistence,
synchronization, number values, clipboard geometry, tool state and tab guards.
