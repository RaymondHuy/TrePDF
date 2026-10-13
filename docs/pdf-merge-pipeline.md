# PDF merging

`EditRibbon.qml` exposes **Merge PDF** in the Pages group. This operation is
independent of the active document and is available without an open PDF.
The ribbon action is desktop-only; the iOS document importer/exporter needs
separate multi-file routing before exposing it on iPad.
`Main.qml` lazily loads `MergePdfDialog.qml`, which owns the file pickers and
selection UI. `PDFMergeController` owns the ordered list and background work.

Users add two or more local PDFs, review file names and page counts, reorder
by dragging or the Up/Down controls, and remove unwanted entries. Merge and
Save As opens a PDF destination picker. Success opens the result in a new tab.

## Session and file safety

Main commits pending editors before opening the dialog. Before merging,
`PDFTabModel::mergeValidationError()` checks that selected open inputs have no
unsaved edits and the destination is not already open. Unrelated open drafts
do not block merging. The modal dialog suspends document history/save/close
shortcuts and queues external open requests until it closes.

The controller rejects source destinations, validates inputs again in its
worker and writes into temporary staging before atomically committing the
destination. Errors and cancellation retain the input list and leave the
original PDFs untouched. There is no edit-history command: the output is a
new document.

## PDF content

qpdf copies PDF objects directly. Page text, fonts, images and vector content
are retained without rendering pages to bitmaps. Ordinary annotations and
supported AcroForms are retained. Internal destinations must refer to the
corresponding page in the merged output.

The dialog states that bookmarks, embedded attachments and accessibility tags
are not merged. Unsupported security or interactive structures are reported
without producing a partial output.

Encrypted files, digital signatures, XFA, optional-content layers, scripts and
unsupported actions are rejected. Absolute URI links and supported internal
destinations survive merging; relative URI and remote GoTo destinations are
rejected because moving the output could change their meaning.

## Work limits

One worker processes at most 100 files and 10,000 pages, with a 256 MiB limit
per input and a 1 GiB combined input limit. Input snapshots retain SHA-256
digests to detect changes before committing. Cancellation is cooperative
between processing phases; an active qpdf job finishes its current phase
before cancellation takes effect. No destination is committed after an
accepted cancellation.

## Verification

`trepdf_merge_tests` exercises output order/content and file-safety failures.
`mergesPdfsThroughMainWindow` and
`guardsMergeAgainstOpenPdfDraftsAndDestinations` in the controller suite cover
ribbon availability, dialog loading, open-draft/output guards and opening the
result in a new tab. Native macOS verification covers the localized dialog,
multiple-file selection, button/drag reordering, saving and automatically
opening a five-page result, zooming, changing scroll position, closing and
reopening the result, and normal application exit. The `cmc.pdf` fixture is
copied from Downloads and its original bytes remain unchanged.
