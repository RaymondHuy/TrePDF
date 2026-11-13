# Local macOS Release build

Build an optimized TrePDF application, run it on your development Mac, or package
it as a drag-to-install `.dmg` for local testing.
These commands use the project's current Apple Silicon toolchain and a separate
`cmake-build-release` directory, leaving `cmake-build-debug` unchanged.

## Requirements

- An Apple Silicon Mac with macOS 14.4 or newer for the configured Qt 6.12.0 SDK.
- Xcode with an active Apple Clang toolchain that supports C++23.
- Qt installed at `$HOME/Qt/6.12.0/macos`, or an equivalent macOS SDK location.
- CLion installed at `/Applications/CLion.app`; the commands below use its
  bundled CMake and Ninja, so neither tool needs to be on `PATH`. CMake 3.21 or
  newer is required if substituting another installation.
- The local dependency packages under `third_party/pdfium`, `third_party/qpdf`
  and `third_party/ocr`. OCR includes the Tesseract/Leptonica libraries,
  English/Vietnamese models and PDF font. Normal application builds do not
  download these dependencies.

`$HOME` refers to the current user's home directory. Adjust the Qt SDK path if
your installation uses another location. The bundled PDF libraries are ARM64;
the local commands below produce an ARM64 application. For a separate Intel
package, see [Build for Intel Macs](#build-for-intel-macs).

## Configure and build

Open Terminal in the root of your TrePDF checkout (the directory containing
`CMakeLists.txt`). Run all commands in this guide from that directory:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  -S . -B cmake-build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DBUILD_TESTING=OFF \
  -DTREPDF_DEPLOY_RUNTIME=OFF \
  -DCMAKE_MAKE_PROGRAM=/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja \
  -DQt6_DIR="$HOME/Qt/6.12.0/macos/lib/cmake/Qt6"

/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build cmake-build-release --target trepdf --parallel 8
```

CMake defaults to the bundled PDFium, qpdf and OCR packages from this repository;
existing cache entries and the `QPDF_ROOT` or `OCR_ROOT` environment variables can
override those paths.
The `Release` setting is chosen during configuration; `--config Release` alone
does not switch a single-configuration Ninja build from Debug to Release.
The commands disable test targets and runtime deployment for this local build.
They do not run the test suite.

## Run and locate the application

After the build completes successfully, open the bundle:

```sh
open cmake-build-release/trepdf.app
```

To see application output in Terminal instead:

```sh
./cmake-build-release/trepdf.app/Contents/MacOS/trepdf
```

The build result is `cmake-build-release/trepdf.app` inside your checkout.
Quit any running Debug build before checking the Release application so the
windows are easy to distinguish.

## Rebuild after source changes

From the repository directory, run the build command again:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build cmake-build-release --target trepdf --parallel 8
```

Ninja rebuilds the changed targets. Re-run configuration when changing SDK paths,
build options or the application version. To use a different version, add
`-DTREPDF_VERSION=0.1.1` to the configure command and rebuild; replace `0.1.1`
with the intended `major.minor.patch` value. A fresh local build defaults to
`0.1.0`; an existing build directory retains its configured version.

## Build with another UI language

The desktop application currently chooses its UI language explicitly in
[`src/main.cpp`](../src/main.cpp):

```cpp
engine.setUiLanguage(QStringLiteral("vi"));
```

There is currently no CMake option or command-line argument for selecting the UI
language. To build an English application, change that line to:

```cpp
engine.setUiLanguage(QStringLiteral("en"));
```

The existing translation catalogs are:

| Locale | UI language | Catalog |
| --- | --- | --- |
| `vi` | Vietnamese | `i18n/qml_vi.ts` |
| `en` | English | `i18n/qml_en.ts` |
| `hr` | Croatian | `i18n/qml_hr.ts` |
| `th` | Thai | `i18n/qml_th.ts` |

After changing the locale, rebuild and launch the application:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build cmake-build-release --target trepdf --parallel 8

open cmake-build-release/trepdf.app
```

Quit the running application before opening the rebuilt bundle. Selecting one
of these existing locales does not require running `update_translations` or
changing the list of languages in CMake. All configured catalogs remain bundled;
the selected locale determines which one the application loads.

This is a source change shared by subsequent builds, not a per-build-directory
setting. Change the line back to `vi` and rebuild when you want Vietnamese again.
It changes interface text, not the document contents or OCR recognition languages.

For a language without a catalog, follow the
[localization instructions](../README.md#localization): register the locale in
`I18N_TRANSLATED_LANGUAGES`, generate and complete its catalog, then select that
locale in `src/main.cpp` and rebuild. The website's language list is independent
of the application's catalogs.

## Common setup issues

- **Ninja not found:** keep the explicit `CMAKE_MAKE_PROGRAM` argument. Ninja
  bundled with CLion may not be on `PATH`.
- **Qt not found:** check that `Qt6Config.cmake` exists under the `Qt6_DIR` path
  above, or point it to your installed macOS Qt SDK.
- **PDFium, qpdf or OCR not found:** restore the corresponding dependency package,
  or supply its full path with `-DPDFium_ROOT=...`, `-DQPDF_ROOT=...` or
  `-DOCR_ROOT=...` during configuration. See the [qpdf package notes](../third_party/qpdf/README.trepdf.md)
  and [OCR package notes](../third_party/ocr/README.md).
- **Generator or architecture mismatch in an existing build directory:** use a
  new directory such as `cmake-build-release-arm64` in both configure and build
  commands, then open the application from that directory.

## Create a local DMG

A `.dmg` is a disk image containing `trepdf.app` and an Applications shortcut.
Open the image and drag the application onto that shortcut to install it.
TrePDF does not need a `.pkg` installer for this workflow.

First complete **Configure and build** above. The raw build bundle can still
depend on your Qt SDK. Enable `TREPDF_DEPLOY_RUNTIME` and run `cmake --install`
to gather the Qt frameworks, QML imports and plugins into an installed bundle.
This uses the project's existing
[Qt deployment script](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html),
alongside the bundled PDFium, qpdf and OCR resources. Qt Svg must also be installed
in the selected SDK.

Run this entire block from the repository root. It reuses your Release build's
version and selected UI language, creates a fresh staging directory, signs the
installed app ad-hoc, checks startup, and creates a compressed DMG. Python 3 and
the macOS `codesign` and `hdiutil` tools are required. The startup check opens the
packaged application for about 12 seconds and then closes that test process.

```sh
(
  set -eu
  trepdf_cmake=/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake

  "$trepdf_cmake" -S . -B cmake-build-release \
    -DTREPDF_DEPLOY_RUNTIME=ON
  "$trepdf_cmake" --build cmake-build-release --target trepdf --parallel 8

  trepdf_package_dir="$(mktemp -d "$PWD/cmake-build-release/package.XXXXXX")"
  trepdf_stage="$trepdf_package_dir/stage"
  "$trepdf_cmake" --install cmake-build-release --config Release \
    --prefix "$trepdf_stage"

  python3 -B - "$trepdf_stage/trepdf.app" <<'PY'
from pathlib import Path
import runpy
import sys

helpers = runpy.run_path("scripts/ci/package_uat.py")
app = Path(sys.argv[1]).resolve()
helpers["sign_macos_bundle"](app)
helpers["smoke_test"](app / "Contents/MacOS/trepdf", "macos-arm64")
PY

  ln -s /Applications "$trepdf_stage/Applications"
  trepdf_dmg="$trepdf_package_dir/TrePDF-macos-arm64.dmg"
  hdiutil create -volname TrePDF -srcfolder "$trepdf_stage" \
    -format UDZO "$trepdf_dmg"
  hdiutil verify "$trepdf_dmg"
  printf '\nDMG created: %s\n' "$trepdf_dmg"
  open -R "$trepdf_dmg"
)
```

Finder reveals the resulting `TrePDF-macos-arm64.dmg` inside a new
`cmake-build-release/package.XXXXXX/` directory (`XXXXXX` is generated by
`mktemp`). Each run creates a separate package without overwriting earlier
images. Share the DMG; its adjacent `stage` directory is only packaging input.
The commands leave `TREPDF_DEPLOY_RUNTIME=ON` in the Release build cache.

The package targets Apple Silicon and, with the Qt 6.12.0 SDK above, macOS 14.4
or newer. Check it on a compatible Mac without the Qt SDK before distributing
it. Changing the UI language requires rebuilding and creating a new package.

The commands reuse only the signing and startup-check helpers from
`scripts/ci/package_uat.py`. Do not invoke that script's full CLI for this local
flow: it expects UAT version numbers and a CI dependency manifest.

## Build for Intel Macs

M1/M2/M3/M4 Macs share the ARM64 package. Intel Macs need an `x86_64` package.
Changing only `CMAKE_OSX_ARCHITECTURES` is insufficient: the PDFium, qpdf,
Tesseract and Leptonica binaries checked into this checkout are ARM64.

The separate [Intel UAT workflow](github-uat.md#run-and-download) prepares Intel
dependencies and builds on GitHub's `macos-15-intel` runner. Use this route when
your development Mac is Apple Silicon:

1. Ensure the branch on GitHub contains the source changes you want to package.
2. Open **Actions → uat-macos-intel → Run workflow** and select that branch.
3. After the Intel job succeeds, download its artifact:
   - `TrePDF-0.0.x-macos-x64` for macOS 14.4 or newer (Qt 6.12).
   - `TrePDF-0.0.x-macos-x64-qt6.11` for macOS 13 or newer (Qt 6.11).
4. Extract the artifact and the package ZIP inside it, then drag `trepdf.app`
   into Applications. Keep the included notices and `START-HERE.txt` with the
   package when sharing it.

This workflow launches only the two Intel jobs. Windows, Apple Silicon and Linux
remain in the independent `uat` workflow. Each Intel job produces a separate
ZIP and checksum, not a universal binary or a DMG. These packages use ad-hoc
signing as described below. Each workflow has its own `0.0.x` version counter.

For a local build on an Intel Mac, follow the dependency preparation and CMake
commands in [`.github/workflows/uat-macos-intel.yml`](../.github/workflows/uat-macos-intel.yml), with
Python 3.12+, CMake/Ninja on `PATH`, Xcode, and the matching Qt SDK. Use a separate
dependency directory and build directory, and set `MACOS_ARCHITECTURE=x86_64`
and `MACOS_DEPLOYMENT_TARGET` to the matching target above. Pass the prepared
`PDFium_ROOT`, `QPDF_ROOT` and `OCR_ROOT` explicitly to CMake.

The dependency script checks its Python process's OS and architecture; an ARM64
Python process is rejected for `--platform macos-x64`. The existing workflow is
a native Intel build procedure, not a cross-compilation recipe for an ARM64
Terminal. Its packaged app is also checked for the `x86_64` architecture and
tested at startup before upload.

## Public distribution

The DMG above uses ad-hoc signing for local testing. It does not have a Developer
ID signature or an Apple notarization ticket, so a recipient's Mac may block it
after download. A DMG container alone does not establish Gatekeeper trust.

For a public release, sign the application with a Developer ID Application
certificate and hardened runtime, notarize the deliverable with Apple's
`notarytool`, and staple the ticket before publishing. See
[Apple's notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
The local block does not perform these steps.

The [UAT workflow guide](github-uat.md) describes the existing CI packages,
dependency notices and signature verification.
