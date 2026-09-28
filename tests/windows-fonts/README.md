# Windows system-font integration tests

This independent CMake project exercises the production font resolver without
building the application's broader test suite. It uses installed Windows fonts
and checked-in TeX Gyre fixtures, with no network access or system font installation.
The real `FontDownloadService` downloads fixture responses into a unique QtTest
private cache via `QStandardPaths::setTestModeEnabled(true)`, registers all four
faces, verifies editor lookup/cache reuse, and removes its own files at cleanup.
An optional live HTTPS check uses the same service and temporary test cache:
set `TREPDF_TEST_LIVE_FONT_DOWNLOAD=1` and run only `automaticCtanDownload`.
Ordinary CTest runs use fixture responses.

At least one of Arial or Cambria must be installed. All four real styles of
installed Arial and Georgia must work. Optional coverage checks both Cambria and
Cambria Math from a TTC, Segoe UI
Emoji rejection, and Segoe UI Symbol's unavailable italic style. The PDFium tests
save Vietnamese text with Arial/Cambria and English with Georgia (some Windows
versions lack Vietnamese glyphs), reopen the PDF, extract the text,
and compare its embedded font program to the original resolved TTF.

Use Qt and a compiler with matching ABI, plus the PDFium and zlib packages used by TrePDF.
For example, from a PowerShell shell configured for MSVC:

```powershell
cmake -S tests/windows-fonts -B build/windows-fonts-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=E:/Qt/6.10.3/msvc2022_64 -DPDFium_ROOT=E:/Projects/TreOffice/TrePDF/_deps/windows/pdfium -DZLIB_ROOT=E:/Projects/TreOffice/TrePDF/_deps/uat-msvc-check/zlib
cmake --build build/windows-fonts-msvc
ctest --test-dir build/windows-fonts-msvc --output-on-failure
```

For MinGW, use a separate build directory and the matching Qt MinGW prefix;
pass `-DCMAKE_CXX_COMPILER` and `-DCMAKE_MAKE_PROGRAM` if they are not on PATH.
CTest adds the selected Qt and compiler runtime directories to PATH and uses
Qt's native Windows platform without creating windows, because the offscreen
plugin does not enumerate installed Windows fonts. The build copies only `pdfium.dll` into its own test
output directory; the application's CLion output is untouched.
Detailed test results are written to `windows-system-fonts.xml` in the test build
directory, including when Qt routes console output to the Windows debugger.
