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
