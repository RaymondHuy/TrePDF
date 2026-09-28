# Windows atomic save regression tests

This standalone suite uses the production `PDFiumDocument` reader and
`PDFAtomicSaveFile` writer. It creates disposable PDFs and never opens a user's
document.

It verifies atomic replacement with four live readers, lazy reading of the old
file after replacement, reopening the new file, move ownership, password and
file errors, Unicode filenames, and safe failure/retry when an external reader
does not share deletion. It also checks new destinations, cancelled/abandoned
writes, cleanup, and read-only destinations. The tests create no windows.

Configure with Qt Core/Test and the matching PDFium package, then build and run:

```powershell
cmake -S tests/windows-save -B build/windows-save-tests -G Ninja `
    -DCMAKE_PREFIX_PATH=E:/Qt/6.10.3/msvc2022_64 `
    -DPDFium_ROOT=E:/Projects/TreOffice/TrePDF/_deps/uat-msvc-check/pdfium
cmake --build build/windows-save-tests
ctest --test-dir build/windows-save-tests --output-on-failure
```

Use an initialized MSVC environment, or configure with the matching MinGW
compiler and Qt package. The production fallback for files larger than 4 GiB
retains PDFium's native reader because the custom-access API has a 32-bit size
on Windows; that fallback retains the original replacement limitation.

For the application's complete save path, build `trepdf_edit_controller_tests`
with `BUILD_TESTING=ON` and run `savesCurrentFileRoundTrip`,
`savesOpenPasswordOnSamePathWithTextChanges`,
`rehydratesTextFieldAfterSamePathSave`, and `keepsPendingOpenPasswordWhenSaveFails`.
The opt-in `savesExternalPageFourOnSamePath` test accepts
`TREPDF_TEST_MIXED_FONT_PDF` pointing at `500 Lines or Less.pdf`. It copies the
document into its temporary directory, installs the checked-in TeX Gyre fixtures
in an isolated test cache, edits page four and saves/reopens twice through
`PDFTabModel`. It verifies the original document's bytes remain unchanged.
