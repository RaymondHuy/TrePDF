# OCR pipeline

## User contract

Convert > Recognize Text (OCR) opens a lazy right-side panel and a language
selection modal. English and Vietnamese are selected initially and included
with the application. The user can search the supported language names/native
names/codes and select one to four languages that actually occur in the scan.
The explanation distinguishes language guidance from automatic language
detection: the selected models help recognize the correct letters, accents and
words; multilingual documents may need more than one model. Selecting every
language is neither necessary nor supported.

**Use these languages** prepares existing files. If additional models are needed,
**Download and use** shows their download size and automatically downloads them.
The modal shows progress, cancellation and a retryable error. Only model files
are downloaded; document pages and recognized text stay on the user's device.
Successfully prepared models are retained for later offline use. Cancel/Escape
cancels preparation and leaves the panel's previous selection unchanged. A
successful preparation closes the modal without starting recognition.

**Choose languages** reopens the modal. Selection belongs to the current panel:
switching source sessions or destroying/recreating the panel resets it to
English + Vietnamese. Downloaded models remain installed. Source changes,
unload, page-structure changes, saving or an already-running OCR job close and
destroy the language modal; late preparation callbacks cannot apply stale input.

The panel also selects all pages, the current PDF page, or a custom
one-based inclusive range such as `1-3, 5`. **Create OCR PDF** remains a separate
action after language preparation. Recognition creates a separate
searchable PDF and opens the result in a new tab. The source must have no pending
content, page, password or form changes. OCR is an export operation and does not
enter the source document's Undo history.

The destination defaults to `<name>-ocr.pdf`. It must differ from the source and
from every open PDF. Existing destinations are replaced only after the complete
output passes validation. Cancellation, recognition failure, invalid input and
no newly recognized text leave the destination unchanged. Pages with any
non-whitespace source text, including existing invisible OCR, are skipped. This
also skips mixed pages with a text heading and a scanned body. Unselected pages
remain in the output; the range controls recognition, not page extraction.

The original page images, vector content and annotations are retained. OCR adds
invisible Unicode text for search, selection and copying; it does not reconstruct
editable fonts or turn a scan into ordinary Edit Text objects. Handwriting,
table-to-Excel reconstruction, region selection, result correction and replacing
an existing OCR layer are outside the first version. Page rotation and CropBox
geometry are mapped; automatically correcting sideways/upside-down scan pixels
and deskewing are not offered in this release.

## Responsibilities

- `PDFOCRController` owns session progress, range validation, an immutable request
  snapshot, cancellation and final atomic `QSaveFile` commit. One global worker
  accepts one job at a time; another request is rejected instead of queued. It
  rejects unknown, duplicate, empty or over-limit language selections and
  snapshots the installed model paths in selection order without hashing model
  files on the GUI thread. A missing model fails before dispatch with an action
  to prepare the languages again.
- `OCRLanguageManager` owns the fixed model catalog, preparation, asynchronous
  validation, bounded downloads and persistent model cache. It shares the
  catalog allowlist and pinned size/SHA-256 values with the OCR pipeline. This
  is independent of document drafts and has no access to PDF content.
- `PDFDocumentSession` owns the controller and cancels recognition when source,
  page structure, edit history, pending changes or save state changes. Closing
  the source session cancels work. Switching tabs leaves the job associated with
  its original session; successful completion is relayed through `PDFTabModel`.
- `PDFOCRPipeline` opens a private source PDFium document. Every PDFium operation,
  including destruction, runs under `PDFiumProcessLock`. The lock is released
  before recognition, qpdf work, progress callbacks and filesystem copies.
- Before opening Tesseract, `PDFOCRPipeline` copies each selected model and the
  bundled `pdf.ttf` into a private `QTemporaryDir`. Copies use bounded 64-KiB
  chunks with cancellation and verify exact pinned sizes and SHA-256 hashes.
  Missing, corrupt or symlinked files fail before recognition; a nonempty model
  path map must cover exactly the selection. Tesseract and its PDF renderers
  read only these private verified copies, which outlive the engine. Cache
  replacement or removal during recognition cannot change the running job.
- Tesseract's LSTM engine uses the selected `tessdata_fast` models from pinned
  commit `87416418657359cb625c412a48b6e1d6d41c29bd`. English/Vietnamese are bundled;
  other catalog languages are installed on demand. Recognition preserves the
  user's selected language order. Leptonica receives a grayscale `Pix` from a
  300-DPI PDFium raster.
  Recognition excludes annotation appearances to avoid duplicating their text.
- Tesseract emits a text-only PDF with its Unicode font and ToUnicode map. qpdf
  imports that layer as a Form XObject into the original PDF; page content is
  not regenerated by PDFium. A mapping derived from `FPDF_DeviceToPage` converts
  layer coordinates through page rotation and crop offsets. The isolated layer
  is prepended with balanced graphics state and `TrePDFOCR` provenance.
- The complete PDF is written to a temporary file. Validation reopens it with
  PDFium, verifies page count, encryption revision, recognized-page dimensions,
  annotation counts, extracted text presence and identical bounded page renders.
  Only then does the controller copy to `QSaveFile` and atomically commit.

## Bounds and lifecycle

At most 1,000 pages are selected, with a 4,096-character range expression.
At most four unique catalogued language codes are accepted. The fixed catalog
is in `src/ocr-languages.json`; arbitrary filenames, paths and model URLs are
never accepted from the UI. Recognition does not initiate downloads or modify
the shared model cache. Language preparation is a separate cancellable step.
Recognition uses one page raster at a time, at most 20 × 1,024 × 1,024 pixels
and 10,000 pixels per dimension. The RGB raster is released before OCR.
Each page has a 120-second recognition deadline and a cancellation callback.
Results are bounded to 50,000 characters per page and two million characters
per job. These limits bound application inputs; they are not a hard process-RSS
limit for third-party PDF/image decoders or the OCR engine.

Source path identity, revision, file size and modification time protect against
stale results. Cancellation and the final rename share a mutex; cancellation
cannot claim that a file already committed successfully was never written.
Opening an OCR destination in another tab, or saving another document to it,
cancels the job before exposing/replacing that file. Third-party exceptions do
not escape the worker and temporary data is cleaned up on all failure paths.

Encryption is preserved. OCR requires content modification and copying
permissions; it does not remove document security. The pipeline rejects
unsupported input and validation failures instead of publishing partial output.

## Dependencies and portability

Tesseract 5.5.3 and Leptonica 1.87.0 are static native dependencies, linked with
the existing zlib. English/Vietnamese models and the PDF font are deployed with
the application. Additional models are downloaded from the pinned official
`tesseract-ocr/tessdata_fast` revision and stored in
`AppLocalDataLocation/ocr/tessdata-fast-87416418657359cb625c412a48b6e1d6d41c29bd/`.
Runtime recognition does not use network access, an external executable or a
system Tesseract installation. See `third_party/ocr/README.md` for rebuilds,
checksums, notices and platform packaging. Desktop macOS/Windows/Linux use the
same controller and pipeline. The existing iOS build has an unavailable backend
and hides the OCR action.

## Verification

`trepdf_ocr_pipeline_tests` exercises actual engine output and PDF persistence.
It also verifies language-order combinations, input allowlists, missing and
same-size-corrupt model rejection, invalid model maps, font integrity, unchanged
output on failure, and independence from cache removal during a multi-page job.
`TREPDF_TEST_FRA_MODEL` can point to a verified local French `.traineddata`
fixture to exercise an additional downloaded model with the actual engine;
the test never downloads fixtures or changes the user's model cache.
`trepdf_ocr_controller_tests` uses a deterministic backend to exercise ranges,
guards, cancellation, source invalidation, bounded scheduling and atomic output
failure. `routesOCRPanelAndGuardsDirtySources` in the main Qt integration suite
checks panel loading, language/range controls, dirty-source guards, session
switching and completion routing. Tests create their fixtures and outputs in
temporary directories and never overwrite user PDFs.

Native platform release validation should also exercise Search/Copy in the
opened output, long Vietnamese translations, cancellation on long scans and
opening/closing/scrolling/zooming while recognition runs. OCR accuracy and
throughput depend on the source scan; synthetic regression fixtures are not an
accuracy benchmark for arbitrary documents.
