# qpdf backend for TrePDF

Version: 12.4.1
Source: https://github.com/qpdf/qpdf/releases/download/v12.4.1/qpdf-12.4.1.tar.gz
SHA-256: `f045aa277be2356ff53a89a8622945958291177d2483afc20ede7c8a8cd3873c`

This package contains a prebuilt Release macOS arm64 `lib/libqpdf.dylib`
(minimum macOS 13), upstream public headers under `include/qpdf`, `VERSION`,
`SHA256SUMS`, and upstream notices. No qpdf implementation sources are vendored.
The source archive checksum above identifies the upstream release; `SHA256SUMS`
records the files in this binary package.

TrePDF imports this library through `cmake/FindQPDF.cmake` as `QPDF::QPDF` and
copies it into the app bundle. Set `QPDF_ROOT` to use another compatible package.
Normal configure/build steps neither download nor compile qpdf; TrePDF remains
C++17 and calls the C API. No qpdf CLI or Homebrew installation is required.
The public C++ headers are included because `qpdf-c.h` exposes optional C++
interop declarations when included from C++ code.

The library uses native crypto with OS secure randomness. JPEG from the Qt SDK
is linked statically into the dylib; runtime dependencies are system zlib,
libc++, and libSystem only.

## Rebuild for a dependency upgrade

From the repository root:

```sh
scripts/build-qpdf-macos.sh /Users/luongvanhuy/Qt/6.11.0/macos
```

The optional second argument selects an output directory instead of
`third_party/qpdf`. This separate step builds only the library from the verified
official source archive and requires a C++20 compiler, CMake, Ninja, and the Qt
SDK's static JPEG library. It does not build the qpdf CLI, tests, or docs.
`TREPDF_CMAKE_BIN` and `TREPDF_NINJA_BIN` can select tool executables. For an
offline rebuild, set `TREPDF_QPDF_ARCHIVE` to the downloaded release archive;
the script still verifies its SHA-256 before using it.

`PDFPasswordWriter` applies AES-256 (revision 6), encrypts metadata, and permits
all document operations after opening. It only accepts unencrypted input and
writes a separate staging path. TrePDF validates that output with PDFium before
atomically replacing the destination. Passwords never appear on a command line.
