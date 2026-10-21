# PDF splitting

**Edit > Pages > Split PDF** sits beside **Merge PDF** and opens a lazy modal.
The action is available on desktop even without an open document. When a PDF
is active, the dialog starts with that file; the user can choose another local
PDF. The original file is never modified.

The dialog offers three ways to split:

- **One file per page** creates a separate PDF for each source page.
- **Every N pages** creates consecutive groups of that size. The last file
  contains the remaining pages when the page count is not divisible by N.
- **Custom page groups** uses semicolons to separate output files and commas
  or inclusive ranges within a file. For example, `1-3;4,6` creates one PDF
  containing pages 1–3 and another containing pages 4 and 6.

Page numbers are one-based PDF positions, independent of printed page numbers.
Custom groups may omit pages. Each group keeps pages in their original source
order, and the groups themselves keep their entered order. Duplicate pages,
including duplicates across groups, descending ranges, empty groups and page
numbers outside the source document are rejected.

The output folder picker selects an existing parent directory. A successful
export creates a new `document-split` subfolder containing files such as
`document-part-0001.pdf`. If that name already exists, a numeric suffix selects
a new folder; existing folders and PDFs are never replaced.

## Ownership and session guards

`SplitPdfDialog.qml` owns the source/folder pickers, mode controls, range input
and progress presentation. `PDFSplitController` owns source inspection, the
validated output plan and bounded asynchronous export.

Main commits pending editors before opening the modal.
`PDFTabModel::splitValidationError()` checks that an open source has no unsaved
changes before export so the new PDFs contain the saved edits. Unrelated open
drafts do not block splitting. The modal participates in the existing
file-open, save, history and close guards. Split is an export operation, not an
undoable document edit.

`PDFTabModel::prepareSplitSource()` reuses the authenticated password of a
matching open tab entirely within C++. Matching includes canonical paths and
hard links; a different active tab does not supply its credentials. A selected
file that still needs authentication shows a masked password field inside the
Split modal, with Show password, Enter/Unlock, and retry. Entering a password
only inspects the source; it does not start exporting.

Passwords are kept in memory only. They are not exposed through a QML getter,
saved in settings, passed to an external process or logged. The input clears
after submission, when the source changes and when the modal closes. Closing
also clears the controller's source and credentials.

## Password protection

Encrypted sources are inspected using the supplied password. A user password
must permit both extraction and page assembly. A source that denies these
operations requires its owner password; the dialog allows another attempt.
Authentication never silently removes source restrictions.

Each output retains the source encryption settings, user and owner passwords,
and permission flags through qpdf's encryption-copy path. The modal explains
that split files keep the original protection. A PDF with an empty user password
continues to open without a password while retaining its owner restrictions;
splitting does not introduce or remove an opening password.

Export reauthenticates the private source snapshot, and output validation opens
each result with the authenticated password before publication. Password-only
sources and encrypted sources containing supported forms use the same atomic
batch export. The original document is never decrypted or rewritten in place.
Legacy RC4 protection is copied as-is. Validation compares `/P` as the same
32-bit permission mask when a source uses unsigned spelling and qpdf writes
the signed equivalent; all other encryption dictionary entries remain part
of the exact fingerprint.

## Ordinary forms

Split supports ordinary AcroForm text fields (including multiline and comb
fields), check boxes, radio groups and choice fields (dropdowns and list boxes).
They remain interactive PDF fields, with their saved values, field flags,
options, default appearance, fonts and widget appearances retained.

The output form tree includes only widgets on retained pages and their necessary
field ancestors. A field shared across retained pages stays one logical field;
widgets on omitted pages and empty field branches are removed. Each output is
an independent form: later changes in one output do not update another output
or the source. Split copies saved form values, so pending input in an open tab
must be saved first.

If a radio group's selected or default option is on an omitted page, that
value becomes `/Off` in the output. Remaining options keep their appearances
and stay selectable; a selection on a retained page remains selected.
For radio groups with indexed `/Opt` export labels, the retained labels,
numeric appearance states and selected/default indices are renumbered together.
Custom nonnumeric `/Opt` state encodings are rejected instead of guessing how
their export values correspond to widgets.

XFA, signature fields, form actions/scripts and calculation dependencies remain
unsupported. Malformed, ambiguous or oversized field/widget graphs are rejected
before publication. The feature does not flatten fields or claim to preserve
digital signatures.

## PDF content and links

qpdf copies page objects directly without rendering text, fonts, vectors or
images into bitmaps. Ordinary page annotations and absolute URI links are
retained. Internal links to pages present in an output are remapped to that
output's page objects. Links to pages omitted from that output are disabled
by removing their action or destination. Their annotation objects and
appearances remain intact, preserving popup and reply relationships.

For supported comment annotations, `/Popup`, `/Parent` and `/IRT` references are
remapped to the matching output annotation indices. This avoids retaining
duplicate annotation objects introduced while qpdf copies references.
This first version requires comment, popup and reply relationships to be
unambiguous and confined to one page; cross-page, ambiguous and self-referencing
relationships are rejected.

Widget `/Parent` references belong to the form field hierarchy and are handled
separately from comment relations.

Digital signatures, XFA, optional-content layers, scripts and unsupported
actions remain rejected. Only supported internal GoTo destinations and absolute
URI actions are allowed. Relative URI and remote GoTo links are rejected
because their meaning may change when the output moves.

Outputs are built as new documents. Catalog-level bookmarks, embedded
attachments and accessibility tags are not transferred. Page-level
FileAttachment annotations remain with their pages. The dialog explains the
document-level limitations before export.

## Atomic publication and cancellation

Source inspection and export run on a bounded worker. A private source
snapshot and SHA-256 fingerprint protect the operation from source changes;
export revalidates the source instead of trusting earlier inspection.

All outputs are written and validated in private staging on the destination
filesystem. The batch is published only after every file succeeds, using a
directory rename that refuses to replace an existing destination. An error or
accepted cancellation before publication removes staging and leaves no partial
final output folder. Source files and existing destination contents remain
untouched.

Cancellation is cooperative between copy, inspection and output phases; it
does not interrupt a qpdf call in the middle of that call. Publication is the
final commit boundary: cancellation after a completed commit cannot retract
files already successfully exported.

## Limits and verification

The operation supports a source of at most 10,000 pages and 256 MiB, at most
1,000 output files, and at most 1 GiB of combined output. Custom range input is
limited to 65,536 UTF-16 code units. The fixed worker bound prevents repeated
actions from queuing unbounded PDF jobs.

Form validation accepts at most 10,000 field/widget nodes and 32 parent levels.
It rejects cycles, duplicate field identities or ambiguous names, and widgets
that do not belong to exactly one source page and one form tree. Password input
is limited to 1,024 UTF-8 bytes and cannot contain null characters.

Automated coverage should exercise all three modes, range parsing and limits,
page order and content, internal links to retained/omitted pages, source
changes, unsupported PDFs, destination collisions and cancellation before
publication. Password coverage includes missing/wrong/correct credentials,
open-session reuse, owner authorization, retained output protection and source
integrity. Form coverage includes retained values and appearances, shared
fields, omitted widgets, filling and saving reopened outputs, and encrypted
forms. UI coverage should exercise adjacent ribbon actions, lazy dialog
loading with and without an open PDF, source draft guards and the success
state, password retry/clearing and the protection notice. Run the application
build and QML lint after changing the dialog, and
check localized labels in the native application.

The password/form extension was verified on macOS with all 64 Split test cases,
the complete eight-suite CTest run (1,367 passed, 13 skipped), and the Split
suite under AddressSanitizer/UndefinedBehaviorSanitizer. Native verification
covered password masking/retry/clearing, owner-password requirements, open-tab
credential reuse, export into a new temporary folder, opening the result with
the original user password, rendering, zoom/scroll and normal application exit.
