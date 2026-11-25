# Manual UAT builds

The [`Tre PDF UAT` workflow](../.github/workflows/uat.yml) builds Windows, Apple Silicon
and Linux Release packages. The separate
[`Tre PDF UAT - macOS Intel` workflow](../.github/workflows/uat-macos-intel.yml) builds only
the Intel macOS packages. The
[`Tre PDF UAT - macOS Universal` workflow](../.github/workflows/uat-macos-universal.yml)
builds installers for both Intel and Apple Silicon Macs. Choose Qt 6.8, 6.11,
6.12 or all three in one run.
Run any workflow independently; all SDKs and
native dependencies are prepared on GitHub, with no local build required:

| Workflow | Package | GitHub runner | Toolchain | Output |
| --- | --- | --- | --- | --- |
| `Tre PDF UAT` | `windows-x64` | `windows-2022` | MSVC 2022 x64, Qt 6.12.0 | Application-folder artifact, launch `bin/Tre PDF.exe` |
| `Tre PDF UAT` | `macos-arm64` | `macos-15` | Apple Clang, Qt 6.12.0 | `.dmg` and `.zip` |
| `Tre PDF UAT` | `macos-arm64-qt6.11` | `macos-15` | Apple Clang, Qt 6.11.0 | `.dmg` and `.zip` |
| `Tre PDF UAT` | `macos-arm64-qt6.8` | `macos-15` | Apple Clang, Qt 6.8.3 | `.dmg` and `.zip` |
| `Tre PDF UAT - macOS Intel` | `macos-x64` | `macos-15-intel` | Apple Clang x86_64, Qt 6.12.0 | `.dmg` and `.zip` |
| `Tre PDF UAT - macOS Intel` | `macos-x64-qt6.11` | `macos-15-intel` | Apple Clang x86_64, Qt 6.11.0 | `.dmg` and `.zip` |
| `Tre PDF UAT - macOS Intel` | `macos-x64-qt6.8` | `macos-15-intel` | Apple Clang x86_64, Qt 6.8.3 | `.dmg` and `.zip` |
| `Tre PDF UAT - macOS Universal` | `macos-universal-qt6.12` | `macos-15` and `macos-15-intel` | Qt 6.12.0, macOS 14.4+ | Universal `.dmg` and `.zip` |
| `Tre PDF UAT - macOS Universal` | `macos-universal-qt6.11` | `macos-15` and `macos-15-intel` | Qt 6.11.0, macOS 13.0+ | Universal `.dmg` and `.zip` |
| `Tre PDF UAT - macOS Universal` | `macos-universal-qt6.8` | `macos-15` and `macos-15-intel` | Qt 6.8.3, macOS 12.0+ | Universal `.dmg` and `.zip` |
| `Tre PDF UAT` | `linux-x64` | `ubuntu-24.04` | GCC, Qt 6.12.0 | `.tar.gz`, run `./TrePDF.sh` |

Choose `macos-arm64` for Apple Silicon (M1 or newer), or `macos-x64` for Intel
Macs. Both Qt 6.12 packages require macOS 14.4 or newer,
matching [Qt 6.12 supported platforms](https://doc.qt.io/qt-6.12/supported-platforms.html).
For macOS 13 or macOS 14.0–14.3, choose the matching architecture's `-qt6.11`
package. These target macOS 13.0 and also run on newer macOS versions, matching
[Qt 6.11 supported platforms](https://doc.qt.io/qt-6.11/supported-platforms.html).
For macOS 12, the `-qt6.8` variants target **macOS 12.0**.
[Qt 6.8 supports macOS 12+](https://doc.qt.io/qt-6.8/supported-platforms.html).
These jobs use the public Qt 6.8.3 SDK and do not require a commercial Qt account.
The minimum supported Qt version for building TrePDF is 6.8.

These are beta compatibility builds. A successful GitHub run verifies build,
deployment, binary minimum-OS metadata and startup on macOS 15; it does not
replace feature testing on an actual macOS 12 machine. Test text editing,
Save/reopen, forms, OCR and table export before distributing them widely.
The `Tre PDF UAT` and `Tre PDF UAT - macOS Intel` macOS packages contain one architecture.
Choose the Universal package for your minimum macOS version when distributing
one installer to both Intel and Apple Silicon users.
Intel jobs run natively on GitHub's
[`macos-15-intel` runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The Linux package targets Ubuntu 24.04
or a compatible newer desktop distribution; system glibc and graphics libraries
are not bundled. The Windows CI toolchain is MSVC; the separate local MinGW
profile documented in [windows-clion.md](windows-clion.md) is unchanged.

## Run and download

1. Commit and push the workflow, CI scripts and associated CMake/version changes.
   The workflow must exist on the default branch for GitHub to show its manual
   dispatch control.
2. For Intel, open **Actions → Tre PDF UAT - macOS Intel → Run workflow**. For Windows,
   Apple Silicon and Linux, choose **Actions → Tre PDF UAT → Run workflow** instead.
   For one installer supporting both Mac architectures, choose
   **Actions → Tre PDF UAT - macOS Universal → Run workflow**.
   In that workflow, select **Qt series** (`6.8`, `6.11`, `6.12` or `all`).
   The default is `6.11`.
   Select the branch. Enter an optional **version** such as `1.2.3`, or leave
   it blank to use `0.0.<run number>`, then start the run.
3. On macOS, download the `TrePDF-<version>-<package>.dmg` entry under **Artifacts**
   in that workflow run. This downloads the DMG directly, with no outer ZIP to
   extract. For an Intel Mac, choose `TrePDF-0.0.x-macos-x64.dmg` (macOS 14.4+)
   or `TrePDF-0.0.x-macos-x64-qt6.11.dmg` (macOS 13+). For macOS 12, choose
   the `-qt6.8.dmg` package for your architecture, for example
   `TrePDF-0.0.x-macos-arm64-qt6.8.dmg` on Apple Silicon. The matching
   `.dmg.sha256` entry is a separate direct download for checksum verification.
   For example, selecting `6.8` and entering version `1.2.3` produces
   `TrePDF-1.2.3-macos-universal-qt6.8.dmg` for either Mac architecture.
4. To verify the DMG, save its `.dmg.sha256` file in the same directory and run
   the following command there, replacing the filename with your package:

   ```sh
   shasum -a 256 -c TrePDF-0.0.x-macos-arm64.dmg.sha256
   ```

   Open the `.dmg`: Tre PDF appears on the left, with an arrow pointing to the
   **Applications** shortcut on the right. Drag the app onto that shortcut,
   eject the disk image and open Tre PDF from Applications. Open
   `Read Me/START-HERE.txt` inside the image for signature verification and
   first-launch instructions.

The `Tre PDF-0.0.x-<package>` artifact without a filename extension remains the
ZIP fallback on macOS: extract that artifact, then the package ZIP inside it.
It contains the ZIP and its checksum, while the DMG and DMG checksum have their
own entries. On Linux, download this package artifact and extract the platform
archive before launching the app.

On Windows, download `Tre PDF-0.0.x-windows-x64` and extract the GitHub artifact
once, then run `bin/Tre PDF.exe`. The workflow uploads the deployed application
folder directly, so there is no second package ZIP or archive checksum inside.
GitHub's artifact download still wraps the folder in a ZIP. Keep all extracted
libraries, plugins and resources together with the executable.

These packaging changes apply to new runs of the updated workflow. After pushing
these changes, choose **Run workflow** and the updated branch. **Re-run jobs**
on an older run uses that run's old workflow and retains its previous packaging.
See GitHub's [re-run behavior](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs).

There are no automatic push, pull-request or schedule triggers. Each workflow
runs its configured matrix independently. A failed matrix job does not cancel
its siblings. Final downloads are retained for 30 days, subject to repository
policy. The universal workflow retains its intermediate `internal-*` artifacts
for seven days. The workflows use `contents: read`. macOS packaging requires the
five repository secrets listed below.

With GitHub CLI authenticated, you can also start only the Intel workflow from
the repository directory and watch its progress:

```sh
gh workflow run uat-macos-intel.yml
gh run watch
```

The first command uses the default branch. Add `--ref your-branch` to select
another pushed branch; select the Intel run when `gh run watch` prompts.

## Build version

All five manual workflows accept a `version` input in numeric `major.minor.patch`
format without leading zeros. All packages in a run use the same version.
The desktop app shows it on the **Information** tab. For example, to build
the Qt 6.12 Universal installer and display **Version 1.2.3**, run
`gh workflow run uat-macos-universal.yml -f qt=6.12 -f version=1.2.3`.
Use `-f qt=all` to build all three installers with the same app version.

With the field empty, each workflow uses its own `0.0.${{ github.run_number }}`
sequence. **Re-run jobs** retains the original version in either mode.

The version is passed to CMake and embedded in the application, macOS bundle
and Windows executable metadata. It is also recorded in the output folder/archive name,
`VERSION`, and `build-info.json` alongside the source commit and dependency
versions. Each alternate Qt package uses distinct names for its staging
directory, artifact, ZIP, DMG and checksums; for example, Qt 6.8 metadata records
`qt: 6.8.3`, `variant: qt6.8` and `macos_deployment_target: 12.0`.
The single-architecture macOS jobs use native PDF dependencies matching their
architecture; the universal workflow combines both sets before building.
All macOS jobs verify that the bundle name, display name and executable name
are `Tre PDF` before signing and packaging. GitHub artifact labels use `Tre PDF`;
package filenames keep the `TrePDF-<version>-<package>` prefix used by the upload
and checksum steps.
The build does not commit a version bump back to the repository.
Local builds keep the default `0.1.0` unless `-DTREPDF_VERSION=...` is supplied.

## Dependencies and packaging

[`prepare_dependencies.py`](../scripts/ci/prepare_dependencies.py) requires
Python 3.12 or newer. It downloads PDFium 157.0.8100.0 for current platforms,
or the checksum-pinned **151.0.7920.0** V8 package for macOS 12 targets.
Both ARM64 and Intel compatibility libraries were checked for a 12.0 Mach-O
deployment target and the public APIs used by TrePDF. The older PDFium pin is
confined to these compatibility jobs; do not downgrade current builds.
Every download is SHA-256 verified. Desktop and iOS downloads use `downloads/pdfium-<version>/`
so cached archives from an earlier release do not block an upgrade.
qpdf 12.4.1 uses the checked-in macOS ARM64 package for targets 13+, the official
MSVC package on Windows, and a source build on Linux, Intel macOS and ARM64
macOS 12. The macOS source builds use pinned static libjpeg-turbo 3.1.2 and the
macOS SDK's system zlib,
with the same architecture and deployment target as the app. Windows builds
static zlib 1.3.1. libxlsxwriter is built from the vendored source.

For each selected Qt series, two native jobs prepare the ARM64 and x86_64
dependencies for that series' minimum macOS version. Then
the universal build combines matching PDFium, qpdf, Tesseract and Leptonica
libraries with `lipo`. It builds TrePDF and libxlsxwriter with
`CMAKE_OSX_ARCHITECTURES=arm64;x86_64` and deploys the universal Qt SDK.
Packaging requires both slices in every bundled Mach-O file. Native startup
checks run on both Apple Silicon and Intel runners before the final DMG, ZIP
and their SHA-256 files are uploaded. This produces one universal app, with
the same drag-to-Applications installer layout as the other macOS packages.

The root `.gitattributes` preserves the exact bytes under `third_party/qpdf/`.
This prevents Windows Git's `core.autocrlf` from converting headers to CRLF and
breaking the checked-in `SHA256SUMS` verification. Keep this rule when updating
the dependency; checksums are verified without newline normalization.

UAT enables `TREPDF_DEPLOY_RUNTIME` and uses `cmake --install` to deploy Qt,
QML imports, plugins and PDF libraries into an isolated staging directory.
The Linux launcher supplies its bundled library directory for transitive
dependencies. The archives include dependency notices and build metadata;
macOS uses `ditto` and Linux uses tar to preserve permissions and symlinks.
The workflow pins third-party Actions to commit SHAs.

Windows passes `--no-archive` to `package_uat.py`. OCR validation, startup
verification, dependency notices and build metadata are retained in the deployed
folder. The workflow uploads that folder with compression disabled, without
creating an additional package ZIP or `.sha256` file. The local
`pipelines/build-windows.ps1` uses the same mode and leaves its runnable folder
under `artifacts/local/windows-x64/`.

`Tre PDF UAT`, `Tre PDF UAT - macOS Intel`, `Tre PDF UAT - macOS Universal` and `Tre PDF UAT - iPad` use Node.js 24 releases of Checkout, Setup Python
and Upload Artifact. Desktop Qt installation also uses a Node.js 24 action;
Windows initializes MSVC directly through Visual Studio's Developer PowerShell
instead of a JavaScript action. This runtime belongs to GitHub's build runner,
is not bundled with TrePDF, and does not change the macOS deployment targets
or require Node.js on the end user's computer.

Qt 6.12 also needs the `qttasktree` module for its QML dependencies. The Qt 6.8
matrix rows install only `qtshadertools qtimageformats`; TaskTree is
not available in that repository. All supported builds use the public
QSGTextNode API and the standard Qt 6.8 translation and popup APIs.
Windows uses aqtinstall pinned to upstream commit
`076e1659807d0b362a3ed684d54c2e9c775eb9c7`, which understands Qt's split
Windows repositories; released aqtinstall 3.3.0 cannot resolve those packages.
macOS, Linux and the iPad workflow retain aqtinstall 3.3.0. Both macOS
architectures use Qt's universal `clang_64` package; the workflow selects
`arm64`, `x86_64`, or both through `CMAKE_OSX_ARCHITECTURES`.

After macOS deployment, packaging signs nested Mach-O files and frameworks from
the inside out with a Developer ID Application identity, hardened runtime and
secure timestamps, then signs the complete app. It verifies
the complete resource seal with `codesign --verify --deep --strict`, creates the
ZIP, extracts it again, and repeats verification and startup on the extracted
app. This catches incomplete linker signatures and signatures invalidated by
library deployment before uploading an artifact.
Packaging checks every bundled Mach-O file, including Qt frameworks and plugins,
for the requested ARM64 or x86_64 slice and a deployment target no newer than
the package's declared minimum. `Info.plist` and dependency target metadata must
also agree. A macOS 13-only library therefore stops a macOS 12 job before it
publishes an installer. In the macOS 12 jobs, qpdf, JPEG and OCR are built with
the same target as the app.

The desktop workflows pass `--macos-dmg` to `package_uat.py` for their macOS
jobs. macOS jobs install the pinned dependencies in
[`requirements-macos-dmg.txt`](../scripts/ci/requirements-macos-dmg.txt):
`dmgbuild` 1.6.7, `ds-store` 1.3.3 and `mac-alias` 2.2.3. Packaging creates a
compressed UDZO disk image with a saved 640 × 420 Finder icon view: `Tre PDF.app`
on the left, Applications on the right, and a Retina arrow between them. A
visible **Read Me** folder below contains `START-HERE.txt`, `VERSION`,
`build-info.json` and dependency licenses. The layout is written into the image
without automating Finder. The ZIP keeps its existing directory layout.

Packaging runs `hdiutil verify`, mounts the image read-only, checks the app
signature, Applications shortcut, saved icon positions, background and support
files, then detaches it. Each ZIP and DMG receives
its own SHA-256 file before upload. The pinned
[`upload-artifact` action](https://github.com/actions/upload-artifact/blob/cf430e030ddbb5b0abf93d22962f4752f3646cd9/README.md#upload-an-individual-file-unzipped)
uploads the DMG and its checksum separately with `archive: false`, so each is
downloaded as its original file. The existing package artifact contains the ZIP
and ZIP checksum. The local
`pipelines/build-macos.sh` script does not enable this option and continues to
produce ZIP packages.

[`package_uat.py`](../scripts/ci/package_uat.py) launches the staged app from a
temporary working directory with external Qt/library paths cleared and the
software backend enabled (Cocoa on macOS, offscreen on Windows/Linux).
It rejects early exits and common
QML/plugin loading failures before archiving. This is a startup smoke check;
`BUILD_TESTING=OFF` means the full feature test suite is not run. It does not
replace native rendering or manual UAT.

## macOS signing and notarization

The macOS jobs in `Tre PDF UAT`, `Tre PDF UAT - macOS Intel` and `Tre PDF UAT - macOS Universal` use these
GitHub Actions secrets during the packaging step:

| Secret | Value |
| --- | --- |
| `APPLE_CERTIFICATE_BASE64` | Base64 of a .p12 export containing a valid **Developer ID Application** certificate and its private key |
| `APPLE_CERTIFICATE_PASSWORD` | Password used to export that .p12 |
| `APPLE_API_KEY_BASE64` | Base64 of the App Store Connect team API key's .p8 file |
| `APPLE_API_KEY_ID` | ID of that API key |
| `APPLE_API_ISSUER_ID` | Issuer ID for that key |

Each runner imports the certificate into a temporary keychain. It submits a
ZIP of the signed app to Apple with `notarytool`, waits for acceptance and
staples the ticket to the app. The final ZIP and DMG both contain this app.
It also signs the DMG and submits it separately for its own ticket. Packaging
checks the tickets and Gatekeeper assessments before uploading downloads and
computes checksums after stapling. A rejected submission fails the job and
prints Apple's diagnostic log. Local packaging keeps its ad-hoc signature.

For deployment API details, see Qt's
[QML application deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).
