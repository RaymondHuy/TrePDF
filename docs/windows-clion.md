# Windows / CLion setup

This local setup uses Qt 6.12.0 MinGW x64 and GCC 15.1.0.
CMake configuration and the application build have been verified with the
paths below. An offscreen startup check loaded the application for 12 seconds
without errors. The preset builds the application only; the full feature test
suite has not been run on Windows.

## Local toolchain

| Setting | Path |
| --- | --- |
| Qt prefix | `E:/Qt/6.12.0/mingw_64` |
| GCC / G++ / windres | `E:/Qt/Tools/mingw1510_64/bin` |
| CMake | `E:/App/CLion 2026.1.2/bin/cmake/win/x64/bin/cmake.exe` |
| Ninja | `E:/App/CLion 2026.1.2/bin/ninja/win/x64/ninja.exe` |
| ZLIB root | `E:/Qt/Tools/mingw1510_64/x86_64-w64-mingw32` |
| OCR root | `<repo>/_deps/windows/ocr-mingw1510` |

Use the MinGW Qt kit with this compiler. The separate `msvc2022_64` Qt kit
requires an MSVC toolchain and is not used by this preset. ZLIB headers and
the static library are already provided by the MinGW 15.1 sysroot.
The Qt kit's compiler runtime DLLs must match GCC 15.1; this installation
includes them. Qt TaskTree is also installed for Qt 6.12's QML dependencies.

`CMakeUserPresets.json` contains the machine-specific preset
`windows-clion-debug`, displayed as **Windows Qt 6.12.0 Debug**. It sets the
tool paths, dependency roots, a build-local PATH, and `BUILD_TESTING=OFF`.
The existing test suite includes Unix-specific code and is not enabled by
this application setup. The preset, `.idea/`, `_deps/`, and
`cmake-build-windows-qt612-debug/` are ignored by Git. This build directory
is separate from the old Qt 6.10/GCC 13 build to avoid reusing cached Qt module
paths or compiler checks. The profile name stays unchanged, so existing
run configurations continue to use it.

## Local PDF dependencies

Keep Windows packages separate from the checked-in macOS libraries:

```text
_deps/windows/
  downloads/
    pdfium-v8-win-x64.tgz
    qpdf-12.4.1-mingw64.zip
  pdfium/
    include/fpdfview.h
    lib/pdfium.dll.lib
    bin/pdfium.dll
  qpdf-package/qpdf-12.4.1-mingw64/
    include/qpdf/qpdf-c.h
    lib/libqpdf.a
    bin/qpdf30.dll
  ocr-mingw1510/
    include/tesseract/baseapi.h
    lib/libtesseract.a
    lib/libleptonica.a
    share/tessdata/
```

The checked-in `third_party/ocr` libraries are macOS arm64. The Windows
preset uses a separate native MinGW OCR package; build it with
`scripts/build-ocr.py --windows-toolchain mingw` as documented in
[`third_party/ocr/README.md`](../third_party/ocr/README.md). Its compiler and
zlib must match the toolchain above.

The downloaded packages and SHA-256 hashes are:

- [PDFium 157.0.8100.0, Windows x64 with V8/XFA](https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8100/pdfium-v8-win-x64.tgz):
  `10a926574aa43ef673ba1a89bc64c559307ab541ca6169299d77797a8472dd7b`
- [qpdf 12.4.1, MinGW x64](https://github.com/qpdf/qpdf/releases/download/v12.4.1/qpdf-12.4.1-mingw64.zip):
  `6a47eeddc8ff712a6e003314daae25569402c6e904ba82b7b9181d7b0301b689`

Verify replacement downloads with PowerShell `Get-FileHash -Algorithm SHA256`.
The PDFium V8 package preserves the V8/XFA build options used on macOS.
libxlsxwriter continues to build from its vendored 1.2.4 source. Its missing
upstream `dev/release/pkg-config.txt` template was restored: the repository's
general `[Rr]elease/` ignore rule had excluded that required CMake input, and
a narrow exception now keeps it in the repository.

## Configure and build

Run from the repository root in PowerShell:

```powershell
$trepdfCMake = 'E:/App/CLion 2026.1.2/bin/cmake/win/x64/bin/cmake.exe'
& $trepdfCMake --preset windows-clion-debug
& $trepdfCMake --build --preset windows-clion-debug
```

The executable is `cmake-build-windows-qt612-debug/trepdf.exe`. After linking,
the Windows deployment helper copies PDFium and qpdf DLLs and runs the Qt
kit's `windeployqt` to deploy Qt libraries, QML imports, plugins, and compiler
runtime beside the executable. It uses the compiler runtime selected by Qt,
avoiding the extra runtime copies shipped in qpdf's portable package.
Application translations remain embedded.

The Windows application target defines `NOMINMAX` to prevent Windows headers
from interfering with standard C++ `min`/`max` calls. On Windows-hosted MinGW,
the icon/version resource uses `windres --use-temp-file` so compiler paths
containing spaces, such as the CLion installation above, work during preprocessing.
The resource is built in a separate object target so C++ dependency include paths
(including the zlib sysroot) do not reach windres's argument parser.

In CLion:

1. Open this repository and reload the CMake project.
2. Go to **Settings > Build, Execution, Deployment > CMake** and enable the
   imported **Windows Qt 6.12.0 Debug** preset. Disable the old unconfigured
   default Debug profile if CLion still tries to load it.
3. Choose the `trepdf` run target with that CMake profile, then Build / Run.

If local installation paths change, update `CMakeUserPresets.json` and reload
CMake. No global PATH or shared Mac Qt path needs to change.

## Local release package

`pipelines/build-windows.ps1` uses the separate MSVC Qt kit. With no override,
it selects `E:/Qt/6.12.0/msvc2022_64` when installed; `-QtRoot` and
`QT_ROOT_DIR` still take precedence. It prefers Visual Studio 2022 and also
accepts Visual Studio 2026, including Insiders, with x64 C++ tools and a
Windows SDK. Python 3.12+ x64, CMake and Ninja must be available on PATH.

```powershell
.\pipelines\build-windows.ps1 -CheckOnly
.\pipelines\build-windows.ps1
```

The script prepares its own MSVC PDF/OCR dependencies and writes packages
under `artifacts/local/windows-x64/`. Its build and dependencies are separated
by Qt version under `build/local/` and `_deps/local/`, independently of the
CLion MinGW preset.

## Saving an open PDF on Windows

`PDFiumDocument` owns the PDFium handle together with a custom Windows reader
opened with `FILE_SHARE_DELETE`. Main, form/preview, scan, export, and save
documents use this owner under their existing PDFium process lock. Replacing
the path leaves each old reader attached to the original file until its owner
closes it; reopening reads the new saved file.

`PDFAtomicSaveFile` retains `QSaveFile` staging and cancellation checks. On
Windows it commits that stage to the final path with `FileRenameInfoEx`
(`REPLACE_IF_EXISTS | POSIX_SEMANTICS`), because Qt's `MoveFileEx` replacement
still fails for live readers even when they share deletion. Unsupported
filesystems fall back to the ordinary atomic rename; there is no delete-first
or direct-write fallback. External readers that deny delete sharing and
read-only destinations still fail safely, leaving drafts available for retry.
PDF content/form/encryption stages use temporary directories rather than
retained `QTemporaryFile` handles that would also prevent replacement.

The custom PDFium access length is 32 bits on Windows. Files larger than 4 GiB
keep the native reader and its same-path save limitation. Non-Windows readers
keep PDFium's native loader and saves delegate directly to `QSaveFile`.

See `tests/windows-save/README.md` for the standalone reader/writer regressions
and the application-level Save/Save As, form, password, and page-four probes.

## macOS isolation

The preset is restricted to a Windows host. Windows DLL import and deployment
logic is guarded by `WIN32`; existing non-Windows imported-library behavior
and the `APPLE` bundle steps remain intact. The Mac Qt default and the checked-in
`third_party/pdfium` and `third_party/qpdf` arm64 dylibs are unchanged. Windows
dependencies and build output stay in the separate ignored directories above.
