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
