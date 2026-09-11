# TrePDF tests

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
