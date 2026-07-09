# TrePDF

Qt Quick Controls app scaffold for rendering PDFs with PDFium.

## Build

Install Qt 6.8 or newer and provide a PDFium build that contains `fpdfview.h`
and the `pdfium` library.

```sh
cmake -S . -B build \
  -DQt6_DIR=/Users/luongvanhuy/Qt/6.11.0/macos/lib/cmake/Qt6 \
  -DPDFium_ROOT=third_party/pdfium
cmake --build build
```

Run the app:

```sh
./build/trepdf.app/Contents/MacOS/trepdf
```

On non-macOS desktop builds, the binary is typically at `./build/trepdf`.

## CLion

Qt is already configured for this local SDK:

```sh
/Users/luongvanhuy/Qt/6.11.0/macos
```

PDFium is configured from `third_party/pdfium`.

Open the repository folder in CLion and use the default `Debug` CMake profile.
The project configures Qt and PDFium from the local paths above, then creates
the `trepdf` target.
