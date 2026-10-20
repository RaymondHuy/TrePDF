# Desktop OCR package

This directory contains the native **macOS arm64** static libraries and public
headers for Tesseract **5.5.3** and Leptonica **1.87.0**, built with a macOS 13.0
deployment target. `share/tessdata` contains the English and Vietnamese
`tessdata_fast` models at commit `87416418657359cb625c412a48b6e1d6d41c29bd` and
Tesseract's `pdf.ttf`. Upstream license notices are in `licenses/`; file hashes
are in `SHA256SUMS`. This package adds no OCR command-line executable to PATH.

The app uses raw Leptonica `Pix` input rendered by PDFium, so optional image
codecs, training tools, legacy recognition, OpenMP, curl, archive support,
ScrollView and native-CPU-only compiler flags are disabled. Zlib remains enabled
for text-only PDF output. No Homebrew runtime libraries are required.

Rebuild on the destination OS and architecture:

```sh
python3 scripts/build-ocr.py --prefix /path/to/native-ocr --work /path/to/ocr-build
cmake -S . -B build -DOCR_ROOT=/path/to/native-ocr
```

The script requires Python 3.9+, CMake, Ninja and a native compiler. Set `--cmake`
and `--ninja` when these are outside PATH. `--offline` uses the verified files
already in `<work>/downloads`. Every network artifact has a fixed SHA-256 pin.
On Windows, the default `--windows-toolchain msvc` uses an MSVC developer shell.
Pass `--zlib-root` to the static zlib prefix prepared by
`scripts/ci/prepare_dependencies.py` and match the app's build configuration
(`--build-type Debug` for MSVC Debug). MSVC packages contain `.lib` files.

For a MinGW app, select `--windows-toolchain mingw` and put the matching `gcc`
and `g++` first on PATH. Supply a zlib prefix built for that compiler. Use separate
work and install directories from MSVC; MinGW packages contain `libtesseract.a`
and `libleptonica.a`. For the local Qt 6.12 / MinGW 15.1 kit:

```powershell
$env:PATH = 'E:\Qt\Tools\mingw1510_64\bin;' + $env:PATH
python scripts/build-ocr.py --windows-toolchain mingw `
  --prefix _deps/windows/ocr-mingw1510 --work _deps/windows/ocr-build-mingw1510 `
  --zlib-root E:/Qt/Tools/mingw1510_64/x86_64-w64-mingw32 `
  --cmake 'E:/App/CLion 2026.1.2/bin/cmake/win/x64/bin/cmake.exe' `
  --ninja 'E:/App/CLion 2026.1.2/bin/ninja/win/x64/ninja.exe'
```

Point the app's `OCR_ROOT` at `_deps/windows/ocr-mingw1510`. The checked-in
macOS package in this directory remains separate from these Windows outputs.
On macOS, `--arch` and `--macos-target` select the native architecture and
minimum supported OS.

`scripts/ci/prepare_dependencies.py` rebuilds this package for Windows x64,
macOS arm64/x64 and Linux x64 and exports `OCR_ROOT`. An ordinary CMake build
does not download anything. CMake copies language data into the app bundle on
macOS and next to the executable on Windows/Linux. The UAT packager verifies
the deployed data against the prepared package before archiving.

OCR is not included in the existing iOS target.
