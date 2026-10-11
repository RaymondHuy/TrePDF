# TrePDF tests

`trepdf_excel_export_tests` is a separate focused executable for table geometry,
PDF-to-XLSX extraction, writer cleanup, literal cell data, atomic output,
cancellation, and worker/controller lifecycle. Run it with
`QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_excel_export_tests`.
The main suite includes `exposesExcelExportTabAndDraftGuard` for QML integration.
Geometry coverage includes rectangular merges, malformed/open grids, small
divider offsets, inline footnotes, and known Wingdings symbol conversion.

`trepdf_spreadsheet_workbook_tests` checks merged XLSX ranges, literal strings,
overlap/hidden-text rejection, aggregate memory budgets, and cleanup. Run it
with `cmake-build-debug/tests/trepdf_spreadsheet_workbook_tests`.

`trepdf_edit_controller_tests` remains one Qt Test executable and one
`PDFEditControllerTest` instance. Test names, registration order, fixtures, and
shared state are unchanged by the file split.

`tst_PDFEditControllerWebLinks.cpp` covers URL detection and duplicate rejection,
batch undo/redo/discard, scan cancellation, source-page remapping, unchanged
decoded content streams, and link activation through confirmation before and
after Save. Geometry and saved-link clicks cover all four page rotations, with
and without a CropBox. Removal tests cover mixed annotation types, protected
links, pending additions, history, source-page remapping, Browse suppression,
and source Link appearance restoration through undo/redo/discard. Manual-link
tests cover URL and internal page destinations, source metadata preservation,
editable appearance round-trips, geometry with CropBox/rotation, protected links,
page remapping, atomic failure for deleted destinations/action chains, and native
Browse routing. Saved border pixels cover solid/dashed/underline/invisible styles
at all four page rotations with and without CropBox, followed by recolor/resave.
The active link tool reloads its editable regions automatically after saving.

- `tst_PDFEditController.cpp`: suite initialization and `QTEST_MAIN`.
- `PDFEditControllerTest.h`: shared test class, slots in execution order, and
  fixture members.
- `tst_PDFEditController<Feature>.cpp`: test definitions grouped by feature,
  including each test's `_data` provider.
- `PDFEditControllerTestSupport.h`: shared fixture types, constants, and helper
  declarations.
- `PDFEditControllerTestSupport*.cpp`: helper definitions grouped by purpose.

Keep these files directly in `tests/` so existing `QFINDTESTDATA` paths continue
to resolve. Add new slots to the shared class and implementations to the matching
feature file. Register any additional source file in `tests/CMakeLists.txt`.

From the repository root, run the same executable as before:

```sh
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests
```

To run one test, append its unchanged method name:

```sh
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests savesTextSpacingWithoutStretchingGlyphs
```

## Opening Files and Standard Shortcuts

`tst_PDFEditControllerFileOpen.cpp` covers local PDF URL filtering (Unicode and
spaces, uppercase extensions, canonical duplicates and symlinks), launch/native
event coalescing, events arriving before the QML window, and repeated requests
while a password or file dialog is open. Native requests and actual Qt drag/drop
events open separate tabs without discarding existing drafts. Standard Open and
Find shortcuts are dispatched to the real main window; Find switches from Edit
to View, selects the current query, reveals the field in a narrow ribbon, and
keeps search scoped to the current tab. File dialogs use Qt's non-native fallback
only inside the automated test.

```sh
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests filtersAndQueuesLocalPdfOpenRequests opensNativePdfRequestsThroughMainWindow opensDroppedPdfsWithoutLosingExistingDrafts focusesDocumentSearchWithStandardShortcut opensPdfDialogWithStandardShortcut
```

Finder's Launch Services routing and installed bundle document association still
need a smoke test with the built macOS application.

## Unicode font persistence

`preservesDistinctUnicodeAliasesInTrueTypeFonts` checks exact Unicode extraction
after embedding complete Arial: ordinary/nonbreaking spaces, ASCII semicolon/Greek
question mark, and hyphen/soft hyphen retain distinct mappings despite sharing glyph IDs.
`preservesUnicodeSpacingThroughArialSaveReopen` covers the controller's Save,
reopen, color edit and second Save without normalizing away nonbreaking spaces.
It also verifies that independent drafts with NFC-normalized text share one
complete font resource.
The direct embedding test deliberately bypasses the editor's existing NFC
policy, which canonically maps U+037E to the ASCII semicolon. TrueType source
fixtures use explicit CID mappings too; intentional missing subset characters
remain absent from their font mapping.

`reloadsSavedCandidatesInNewDocumentInstances` saves twice to the same path and
reopens it with three independent `PDFDocument` instances. It checks both saved
page text and freshly scanned candidates, guarding against worker-cache reuse
when separate instances have the same local revision.

The optional `savesExternalPageFourOnSamePath` probe also opens an independent
document after each Save and compares the complete edited paragraph. This avoids
mistaking whole-page extraction order for lost text when a wrapped line overlaps
a neighboring paragraph. Paragraph restoration validates its stored ranges
against actual PDF text objects.

Header UI tests wait for the asynchronous workspace and initial view to be ready,
then locate a page-specific editor. Their Save/Undo and pending-placement
assertions run only after Main can flush the real editor's contents.

The `mixed-font-paragraph` row in `preservesType1UrlThroughInlineEditing` covers
adjacent lines with slightly different font sizes. Entering and committing the
editor must preserve the paragraph separator's own format, including after
Save/reopen, and must not create a draft without a real change.

PDFium 157.0.8086.0 upgrade validation on macOS ARM64 / Qt 6.11 (2026-10-11):

- Controller suite with the optional book fixtures enabled: 1,056 passed,
  0 failed, 2 skipped. Both Header cases and the page-four repeated-save probe
  pass. Skips are the external CTAN download-cache check and a fresh-process
  probe that is invoked separately by its parent test.
- Excel suite: 24 passed. Native Metal rendering/zoom/thumbnail checks: 8 passed.
  The built app also rendered a 478-page PDF, scrolled in both directions,
  changed zoom, closed/reopened the document and quit without a crash report.
- Native Metal Header checks: 6 passed; baseline/Type 1 URL checks including the
  external page-four fixture: 9 passed. The document-instance cache regression
  also passes with the native macOS Qt platform.
- Windows/Linux app builds and iOS device execution were not run locally.

## Symbol font filter

`filtersSymbolFontFamilies` checks the available symbol families (including
Wingdings/Webdings and Unicode Symbol faces), excludes ordinary text families,
and verifies exact-style filtering and mixed-style intersections in Edit and
Header. Installed-family assertions use the same embedding eligibility as Save.

`supportsLegacySymbolCharacters` verifies CoreText/Qt agreement on legacy byte
aliases for installed Wingdings 1/2/3 and Webdings, while rejecting unsupported
Unicode and synthetic bold. `repairsSourceSymbolMappings` uses a generated font
with a symbol cmap and a PDF-only Unicode mapping; it verifies exact glyph and
outline preservation, absent-bullet rejection, bounded cmap handling and repeated
repair stability without depending on installed proprietary fonts.

```sh
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests filtersFontFamiliesByLanguage filtersSymbolFontFamilies intersectsFontFiltersAcrossMixedStyles
```

## Paragraph paint order

`preservesInterleavedParagraphPaintOrder` uses a generated mixed-style paragraph
with white backgrounds between its lines, deliberate foreground paint, unrelated
lower text and a second content stream. It checks rendered ink and unchanged
foreground/lower regions through two Save/reopen cycles, alongside extracted
text and restored paragraph candidates. Extraction alone cannot detect text
that still exists but is hidden behind a background.

```sh
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests preservesInterleavedParagraphPaintOrder
```

The optional `savesExternalMixedParagraphOnSamePath` probe reproduces page 2 of
`cmc4.pdf` through the inline editor and asynchronous Save. It edits a temporary
copy twice, checks raster ink in three paragraph bands and opens an independent
document after each Save to verify the complete paragraph. Its `body` row covers
the original missing-text report; the `wingdings` row covers the Must-have bullet
paragraph. That row also checks that editor preflight creates no download prompt
or draft, that the editor uses the Wingdings glyph rather than font fallback, and
that PDF bullet outlines and Unicode remain unchanged after both saves. It verifies that the
source file bytes remain unchanged. Set `TREPDF_TEST_MIXED_PARAGRAPH_PDF` to
enable it; otherwise the probe skips. Optional
`TREPDF_TEST_MIXED_PARAGRAPH_OUTPUT_DIR` retains before/after PNGs and both saved
PDF copies for inspection (`wingdings/` holds the bullet row). Use a fresh output directory for each run because
existing PDF outputs are not overwritten.

```sh
TREPDF_TEST_MIXED_PARAGRAPH_PDF="$HOME/Downloads/cmc4.pdf" \
TREPDF_TEST_MIXED_PARAGRAPH_OUTPUT_DIR="$(mktemp -d /tmp/trepdf-mixed-paragraph.XXXXXX)" \
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests savesExternalMixedParagraphOnSamePath
```

Validation on macOS ARM64 / Qt 6.11 / PDFium 157.0.8086.0 (2026-10-11):

- Full offscreen suite with the external book and `cmc4.pdf` probes enabled:
  1,058 passed, 0 failed, 2 skipped (optional CTAN catalogue and a helper that
  runs separately in the font-download test's child process).
- Both paint-order probes passed on the native Cocoa/Metal backend. Poppler
  independently rendered the second saved copy with the complete paragraph.
- A separate app instance opened the saved four-page PDF, scrolled both ways,
  changed zoom repeatedly, closed/reopened the document and quit with exit 0.

After the Wingdings mapping fix, on the same macOS/Qt/PDFium configuration:

- Full offscreen suite with the external book and `cmc4.pdf` probes enabled:
  1,061 passed, 0 failed, 6 skipped.
- Native Cocoa/Metal font preflight, symbol mapping and both `cmc4.pdf` rows:
  12 passed, 0 failed, 0 skipped. Both Save/reopen cycles retain the original
  square-bullet outlines; an independent Poppler render confirms their appearance.
- The app and test targets build successfully. Windows/Linux execution was not
  run locally.

## CTAN Font Catalogue

`loadsEveryCtanCatalogueEntry` checks the packaged JSON and exact URL/style
contracts for every entry. `downloadsInconsolataCtanStyles` uses unmodified
upstream fixtures and a fake network to verify downloads, cache reuse, missing
styles and PDF Save/reopen without an external network dependency.

After regenerating the catalogue, validate every selected font against Qt and
the application's runtime validator using the generator's download cache:

```sh
TREPDF_TEST_CTAN_CACHE=/tmp/trepdf-ctan-cache/files QT_QPA_PLATFORM=offscreen \
  cmake-build-debug/tests/trepdf_edit_controller_tests validatesExternalCtanCatalogFaces
```

This optional test does not install the inspected files; it skips when the
environment variable is unset. The normal download tests use an isolated test
font store and never modify the user's TrePDF font cache.

## Image-Only Form XObjects

`editsSharedNestedImagesRoundTrip` covers move/resize/rotate/delete, history,
shared occurrences, nested matrices, image masks, internal clipping, CropBox,
page rotation, and inherited resources. `previewsSharedNestedImageChanges`
compares worker previews against saved output and verifies undo/discard restore
the original raster. Unsafe mixed/marked/clipped/transparent Forms stay read-only.
Resource regressions also cover adding/removing a neighboring image, page
insertion/deletion, password replacement, ordinary encrypted Save, repeated
Save/reopen, and preserving drafts/destination after save failure.

The optional local-book regression checks page 1 while preserving shared page 3
and the original file bytes:

```sh
TREPDF_TEST_NESTED_IMAGE_PDF="/path/to/500 Lines or Less.pdf" QT_QPA_PLATFORM=offscreen \
  cmake-build-debug/tests/trepdf_edit_controller_tests editsExternalNestedImageWithoutChangingSharedPage
```

Set `TREPDF_TEST_NESTED_IMAGE_OUTPUT_DIR` to an empty scratch directory to retain
rendered PNGs of the original and each operation for visual inspection. Edited
PDFs remain temporary test artifacts; the supplied PDF is never overwritten.

## Single-Text Form XObjects

`editsSharedTextFormsRoundTrip` covers content/color/resize replacement,
matrix-only movement, deletion, history/discard, repeated Save/reopen, nested
transforms, inherited font resources, CropBox and page rotation. Two occurrences on one page and a shared
occurrence on another page must remain independent. `keepsUnsafeTextFormsReadOnly`
covers mixed content, marks, clipping (including a smaller Form BBox),
transparency, inherited paint, unsupported text transforms/render modes and
over-deep chains. `previewsSharedTextFormMovesAndDeletion` compares worker
previews against saved rendering and checks undo/redo/discard restoration.

The optional book regression moves the subtitle on page 3 with its original
Museo font, then explicitly selects a local Arial face for content edits
(Qt cannot load the original raw CFF subset). It checks editor font preflight,
neighboring content and page 1, and saves only temporary copies:

```sh
TREPDF_TEST_FORM_TEXT_PDF="/path/to/500 Lines or Less.pdf" QT_QPA_PLATFORM=offscreen \
  cmake-build-debug/tests/trepdf_edit_controller_tests editsExternalFormTextWithoutChangingNeighbors
```

Set `TREPDF_TEST_FORM_TEXT_OUTPUT_DIR` to an empty scratch directory to retain
the original, edited and edited-again page PNGs. The input PDF is never written.

`tst_PDFEditControllerThumbnails.cpp` covers thumbnail navigation in the active
split pane, per-tab collapse state, page insertion/deletion with undo/discard,
and bounded rendering/pooling over 240 mixed-size pages. It checks collapse
cleanup, rotation, current-page reveal, and close/reopen during pending renders.
Run both thumbnail tests with `QSG_RHI_BACKEND=metal` for native texture lifecycle
coverage in addition to the offscreen suite.
