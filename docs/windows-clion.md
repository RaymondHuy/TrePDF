# Windows / CLion setup

This local setup uses Qt 6.10.3 MinGW x64 and CLion's bundled GCC 13.1.0.
CMake configuration and the application build have been verified with the
paths below. An offscreen startup check loaded the application for 12 seconds
without errors. The preset builds the application only; the full feature test
suite has not been run on Windows.

## Local toolchain

| Setting | Path |
| --- | --- |
| Qt prefix | `E:/Qt/6.10.3/mingw_64` |
| GCC / G++ | `E:/App/CLion 2026.1.2/bin/mingw/bin` |
| CMake | `E:/App/CLion 2026.1.2/bin/cmake/win/x64/bin/cmake.exe` |
| Ninja | `E:/App/CLion 2026.1.2/bin/ninja/win/x64/ninja.exe` |
| ZLIB root | `E:/App/CLion 2026.1.2/bin/mingw/x86_64-w64-mingw32` |

Use the MinGW Qt kit with this compiler. The separate `msvc2022_64` Qt kit
requires an MSVC toolchain and is not used by this preset. ZLIB headers and
the static library are already provided by the CLion MinGW sysroot.

`CMakeUserPresets.json` contains the machine-specific preset
`windows-clion-debug`, displayed as **Windows Qt 6.10.3 Debug**. It sets the
tool paths, dependency roots, a build-local PATH, and `BUILD_TESTING=OFF`.
The existing test suite includes Unix-specific code and is not enabled by
this application setup. The preset, `.idea/`, `_deps/`, and
`cmake-build-windows-debug/` are ignored by Git.

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
```

The downloaded packages and SHA-256 hashes are:

- [PDFium 154.0.8021.0, Windows x64 with V8/XFA](https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8021/pdfium-v8-win-x64.tgz):
  `bc9b671c57a3ee3bbc18fda26e4374759b6e57f3bef6ddcc5c9a60d7b4ef6999`
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

The executable is `cmake-build-windows-debug/trepdf.exe`. After linking,
the Windows deployment helper copies PDFium and qpdf DLLs and runs the Qt
kit's `windeployqt` to deploy Qt libraries, QML imports, plugins, and compiler
runtime beside the executable. It uses the compiler runtime selected by Qt,
avoiding the extra runtime copies shipped in qpdf's portable package.
Application translations remain embedded.

The Windows application target defines `NOMINMAX` to prevent Windows headers
from interfering with standard C++ `min`/`max` calls. On Windows-hosted MinGW,
the icon/version resource uses `windres --use-temp-file` so compiler paths
containing spaces, such as the CLion installation above, work during preprocessing.

In CLion:

1. Open this repository and reload the CMake project.
2. Go to **Settings > Build, Execution, Deployment > CMake** and enable the
   imported **Windows Qt 6.10.3 Debug** preset. Disable the old unconfigured
   default Debug profile if CLion still tries to load it.
3. Choose the `trepdf` run target with that CMake profile, then Build / Run.

If local installation paths change, update `CMakeUserPresets.json` and reload
CMake. No global PATH or shared Mac Qt path needs to change.

## macOS isolation

The preset is restricted to a Windows host. Windows DLL import and deployment
logic is guarded by `WIN32`; existing non-Windows imported-library behavior
and the `APPLE` bundle steps remain intact. The Mac Qt default and the checked-in
`third_party/pdfium` and `third_party/qpdf` arm64 dylibs are unchanged. Windows
dependencies and build output stay in the separate ignored directories above.
