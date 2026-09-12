# libxlsxwriter in TrePDF

This directory contains the build sources from libxlsxwriter 1.2.4.
Tests, examples, and documentation are omitted. Required upstream build files,
bundled helper sources, and their notices are retained.

- Upstream: https://github.com/jmcnamara/libxlsxwriter
- Release archive: https://codeload.github.com/jmcnamara/libxlsxwriter/tar.gz/refs/tags/v1.2.4
- Original archive SHA-256: `e0db59fc248a5ffa465a05ea83a9d466d4bca0e53ab42771515d4ebb467a41c1`
- Included paths: `CMakeLists.txt`, `License.txt`, `src/`, `include/`, `cmake/`,
  `third_party/`, and `dev/release/pkg-config.txt`.

The top-level TrePDF CMake file builds this dependency statically, with its
bundled minizip/tmpfileplus/dtoa helpers and the platform zlib. Configuring does
not need network access or a Homebrew libxlsxwriter installation.

Only `src/LibxlsxwriterWorkbook.cpp` in TrePDF includes the backend headers.
The controller and table detector depend on `SpreadsheetWorkbook.h`. To replace
the library, implement that interface, change its factory, and update the build
dependency. Abandoning a workbook uses the pinned library's free-only
`lxw_workbook_free()` entry point to release cells without writing an incomplete
XLSX archive. The adapter uses normal memory mode for general merged ranges,
with explicit aggregate cell and UTF-8 text budgets; streaming mode cannot
preserve arbitrary vertical merges through the public API.

Local patch in `src/worksheet.c`: `lxw_worksheet_free()` closes any remaining
`optimize_tmpfile` and frees `optimize_buffer`; normal packaging nulls these
pointers after releasing them. This fixes row-spill file descriptor leaks when
an export is cancelled or packaging fails before writing sheet data.
