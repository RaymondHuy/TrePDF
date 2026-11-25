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
open "cmake-build-release/Tre PDF.app"
```

To see application output in Terminal instead:

```sh
"./cmake-build-release/Tre PDF.app/Contents/MacOS/Tre PDF"
```

The build result is `cmake-build-release/Tre PDF.app` inside your checkout.
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

Every desktop build includes all twelve UI languages. Open **Settings → Language**
and choose a language from the list below. The interface updates immediately,
and TrePDF remembers the selection after restarting. The menu scrolls when the
window is too short to show every language.
Vietnamese is the default when no supported selection has been saved.

The bundled translation catalogs are:

| Locale | UI language | Catalog |
| --- | --- | --- |
| `vi` | Vietnamese | `i18n/qml_vi.ts` |
| `en` | English | `i18n/qml_en.ts` |
| `hr` | Croatian | `i18n/qml_hr.ts` |
| `th` | Thai | `i18n/qml_th.ts` |
| `fr` | French | `i18n/qml_fr.ts` |
| `de` | German | `i18n/qml_de.ts` |
| `es` | Spanish | `i18n/qml_es.ts` |
| `pt_BR` | Portuguese (Brazil) | `i18n/qml_pt_BR.ts` |
| `ja` | Japanese | `i18n/qml_ja.ts` |
| `zh_CN` | Chinese (Simplified) | `i18n/qml_zh_CN.ts` |
| `ko` | Korean | `i18n/qml_ko.ts` |
| `id` | Indonesian | `i18n/qml_id.ts` |

Switching between these languages requires no source change, rebuild, new
package, or download. The choice is a per-user setting, stored as
`Interface/language` through `QSettings` by
[`ApplicationLanguage.h`](../src/ApplicationLanguage.h). It changes interface
text, not document contents or OCR recognition languages.

For a language without a catalog, follow the
[localization instructions](../README.md#localization): register the locale in
`I18N_TRANSLATED_LANGUAGES`, generate and complete its catalog, add it to
`ApplicationLanguage.h` and `LanguageMenuButton.qml`, then rebuild. The website's
language list is independent of the application's catalogs.

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

A `.dmg` is a disk image containing `Tre PDF.app` and an Applications shortcut.
Open the image and drag the application onto that shortcut to install it.
TrePDF does not need a `.pkg` installer for this workflow.

The local recipe below creates a plain disk image. The
[GitHub Actions DMG](github-uat.md#run-and-download) also saves a Finder icon
layout with the app on the left, an arrow, and Applications on the right.

First complete **Configure and build** above. The raw build bundle can still
depend on your Qt SDK. Enable `TREPDF_DEPLOY_RUNTIME` and run `cmake --install`
to gather the Qt frameworks, QML imports and plugins into an installed bundle.
This uses the project's existing
[Qt deployment script](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html),
alongside the bundled PDFium, qpdf and OCR resources. Qt Svg must also be installed
in the selected SDK.

Run this entire block from the repository root. It reuses your Release build's
version and bundled language catalogs, creates a fresh staging directory, signs the
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

  python3 -B - "$trepdf_stage/Tre PDF.app" <<'PY'
from pathlib import Path
import runpy
import sys

helpers = runpy.run_path("scripts/ci/package_uat.py")
app = Path(sys.argv[1]).resolve()
helpers["sign_macos_bundle"](app)
helpers["smoke_test"](app / "Contents/MacOS/Tre PDF", "macos-arm64")
PY

  ln -s /Applications "$trepdf_stage/Applications"
  trepdf_dmg="$trepdf_package_dir/TrePDF-macos-arm64.dmg"
  hdiutil create -volname "Tre PDF" -srcfolder "$trepdf_stage" \
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
it. Users select their UI language from Settings after installing the same package.

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
2. Open **Actions → Tre PDF UAT - macOS Intel → Run workflow** and select that branch.
3. After the Intel job succeeds, download its DMG directly under **Artifacts**:
   - `TrePDF-0.0.x-macos-x64.dmg` for macOS 14.4 or newer (Qt 6.12).
   - `TrePDF-0.0.x-macos-x64-qt6.11.dmg` for macOS 13 or newer (Qt 6.11).
4. Download the matching `.dmg.sha256` entry to verify the checksum, then open
   the DMG and drag `Tre PDF.app` onto its Applications shortcut. There is no
   outer ZIP to extract for these entries. Eject the image and launch Tre PDF
   from Applications. The artifact without the `.dmg` extension remains a ZIP
   fallback. The DMG's **Read Me** folder holds the notices and
   `START-HERE.txt`; keep them with the package when sharing it. See the
   [download instructions](github-uat.md#run-and-download).

After pushing the direct-download workflow changes, start a new **Run workflow**
on the updated branch. Re-running an older job retains its old workflow.

This workflow launches only the two Intel jobs. Windows, Apple Silicon and Linux
remain in the independent `Tre PDF UAT` workflow. Each macOS job produces a separate
DMG and ZIP, each with its own checksum; these are architecture-specific
packages. The GitHub jobs use [Developer ID signing and notarization](github-uat.md#macos-signing-and-notarization). Each workflow has its own
`0.0.x` version counter. The local `pipelines/build-macos.sh` script continues
to produce ZIP packages; use the local DMG instructions above for that path.

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
