# Manual UAT builds

The [`uat` workflow](../.github/workflows/uat.yml) builds a Release package for
each of these native platforms:

| Package | GitHub runner | Toolchain | Output |
| --- | --- | --- | --- |
| `windows-x64` | `windows-2022` | MSVC 2022 x64, Qt 6.12.0 | `.zip`, launch `bin/trepdf.exe` |
| `macos-arm64` | `macos-15` | Apple Clang, Qt 6.12.0 | `.zip`, open `trepdf.app` |
| `macos-arm64-qt6.11` | `macos-15` | Apple Clang, Qt 6.11.0 | `.zip`, open `trepdf.app` |
| `linux-x64` | `ubuntu-24.04` | GCC, Qt 6.12.0 | `.tar.gz`, run `./TrePDF.sh` |

Both macOS packages target Apple Silicon (M1 or newer). The `macos-arm64`
package requires macOS 14.4 or newer,
matching [Qt 6.12 supported platforms](https://doc.qt.io/qt-6.12/supported-platforms.html).
Use `macos-arm64-qt6.11` for macOS 13 or macOS 14.0–14.3; it targets macOS
13.0 and also runs on newer macOS versions, matching
[Qt 6.11 supported platforms](https://doc.qt.io/qt-6.11/supported-platforms.html).
Neither package supports macOS 12 or earlier, or Intel Macs. They are not
universal binaries. The Linux package targets Ubuntu 24.04
or a compatible newer desktop distribution; system glibc and graphics libraries
are not bundled. The Windows CI toolchain is MSVC; the separate local MinGW
profile documented in [windows-clion.md](windows-clion.md) is unchanged.

## Run and download

1. Commit and push the workflow, CI scripts and associated CMake/version changes.
   The workflow must exist on the default branch for GitHub to show its manual
   dispatch control.
2. Open **Actions → uat → Run workflow**, select the branch, and start the run.
3. Download the appropriate `TrePDF-0.0.x-<package>` entry under **Artifacts**
   from the four build outputs. For macOS 13, choose
   `TrePDF-0.0.x-macos-arm64-qt6.11`.
   Each artifact contains the platform archive and its SHA-256 file. Extract
   the platform archive before launching the app.

There are no automatic push, pull-request or schedule triggers. All four jobs
run independently; one failed platform does not cancel the others. Artifacts
are retained for 30 days, subject to repository policy. The workflow only needs
`contents: read`; no custom secret, tag write or release publication is required.

## Automatic version

`TREPDF_VERSION` is `0.0.${{ github.run_number }}`. The first new run is `0.0.1`,
the next is `0.0.2`, and so on. All four packages in a run share the version.
Failed or cancelled runs consume their number, so downloadable versions may
have gaps. **Re-run jobs** keeps the same version; start a new **Run workflow**
to increment it. This follows GitHub's
[workflow run-number semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/variables).

The version is passed to CMake and embedded in the application, macOS bundle
and Windows executable metadata. It is also recorded in the archive name,
`VERSION`, and `build-info.json` alongside the source commit and dependency
versions. The Qt 6.11 package has a distinct staging directory, artifact, ZIP
and checksum name; its metadata records `qt: 6.11.0` and `variant: qt6.11`.
Both macOS jobs reuse the same native `macos-arm64` PDF dependencies.
The build does not commit a version bump back to the repository.
Local builds keep the default `0.1.0` unless `-DTREPDF_VERSION=...` is supplied.

## Dependencies and packaging

[`prepare_dependencies.py`](../scripts/ci/prepare_dependencies.py) requires
Python 3.12 or newer. It downloads PDFium 157.0.8100.0 and verifies SHA-256 for
each platform. Desktop and iOS downloads use `downloads/pdfium-<version>/`
so cached archives from an earlier release do not block an upgrade.
qpdf 12.4.1 uses the checked-in macOS ARM64 package, the official
MSVC package on Windows, and a source build on Linux. Windows also builds
static zlib 1.3.1. libxlsxwriter is built from the vendored source.

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

Both `uat` and `uat-ipad` use Node.js 24 releases of Checkout, Setup Python
and Upload Artifact. Desktop Qt installation also uses a Node.js 24 action;
Windows initializes MSVC directly through Visual Studio's Developer PowerShell
instead of a JavaScript action. This runtime belongs to GitHub's build runner,
is not bundled with TrePDF, and does not change the macOS deployment targets
or require Node.js on the end user's computer.

Qt 6.12 also needs the `qttasktree` module for its QML dependencies. Windows
uses aqtinstall pinned to upstream commit
`076e1659807d0b362a3ed684d54c2e9c775eb9c7`, which understands Qt's split
Windows repositories; released aqtinstall 3.3.0 cannot resolve those packages.
macOS, Linux and the iPad workflow retain aqtinstall 3.3.0. The required
6.12.0 package metadata was checked for all four targets; remote workflow
builds still need a manual UAT run.

After macOS deployment, packaging signs nested Mach-O files and frameworks from
the inside out, then signs the complete app with an ad-hoc signature. It verifies
the complete resource seal with `codesign --verify --deep --strict`, creates the
ZIP, extracts it again, and repeats verification and startup on the extracted
app. This catches incomplete linker signatures and signatures invalidated by
library deployment before uploading an artifact.

[`package_uat.py`](../scripts/ci/package_uat.py) launches the staged app from a
temporary working directory with external Qt/library paths cleared and the
software backend enabled (Cocoa on macOS, offscreen on Windows/Linux).
It rejects early exits and common
QML/plugin loading failures before archiving. This is a startup smoke check;
`BUILD_TESTING=OFF` means the full feature test suite is not run. It does not
replace native rendering or manual UAT.

These are UAT archives, without a Developer ID distribution certificate or Apple
notarization. The macOS ad-hoc signature verifies integrity but does not establish
Gatekeeper trust. For a trusted UAT download, verify its checksum and app signature,
then use **System Settings → Privacy & Security → Open Anyway** if macOS blocks
the unidentified developer. See [Apple's instructions](https://support.apple.com/en-us/102445).
Do not override a damaged/invalid signature warning; rebuild or download the
fixed package. Each macOS archive includes `START-HERE.txt` with these instructions.
Public distribution without this manual approval needs Developer ID signing and
Apple notarization. The workflow does not change Gatekeeper settings or publish
a GitHub Release. The archives are not installers.

For deployment API details, see Qt's
[QML application deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).
