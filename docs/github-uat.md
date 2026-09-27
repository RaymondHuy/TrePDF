# Manual UAT builds

The [`uat` workflow](../.github/workflows/uat.yml) builds a Release package for
each of these native platforms:

| Package | GitHub runner | Toolchain | Output |
| --- | --- | --- | --- |
| `windows-x64` | `windows-2022` | MSVC 2022 x64, Qt 6.10.3 | `.zip`, launch `bin/trepdf.exe` |
| `macos-arm64` | `macos-15` | Apple Clang, Qt 6.10.3 | `.zip`, open `trepdf.app` |
| `linux-x64` | `ubuntu-24.04` | GCC, Qt 6.10.3 | `.tar.gz`, run `./TrePDF.sh` |

The macOS package targets Apple Silicon (M1 or newer) and macOS 13 or newer.
It is not an Intel or universal binary. The Linux package targets Ubuntu 24.04
or a compatible newer desktop distribution; system glibc and graphics libraries
are not bundled. The Windows CI toolchain is MSVC; the separate local MinGW
profile documented in [windows-clion.md](windows-clion.md) is unchanged.

## Run and download

1. Commit and push the workflow, CI scripts and associated CMake/version changes.
   The workflow must exist on the default branch for GitHub to show its manual
   dispatch control.
2. Open **Actions → uat → Run workflow**, select the branch, and start the run.
3. Download the three `TrePDF-0.0.x-<platform>` entries under **Artifacts**.
   Each artifact contains the platform archive and its SHA-256 file. Extract
   the platform archive before launching the app.

There are no automatic push, pull-request or schedule triggers. All three jobs
run independently; one failed platform does not cancel the others. Artifacts
are retained for 30 days, subject to repository policy. The workflow only needs
`contents: read`; no custom secret, tag write or release publication is required.

## Automatic version

`TREPDF_VERSION` is `0.0.${{ github.run_number }}`. The first new run is `0.0.1`,
the next is `0.0.2`, and so on. All three platforms in a run share the version.
Failed or cancelled runs consume their number, so downloadable versions may
have gaps. **Re-run jobs** keeps the same version; start a new **Run workflow**
to increment it. This follows GitHub's
[workflow run-number semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/variables).

The version is passed to CMake and embedded in the application, macOS bundle
and Windows executable metadata. It is also recorded in the archive name,
`VERSION`, and `build-info.json` alongside the source commit and dependency
versions. The build does not commit a version bump back to the repository.
Local builds keep the default `0.1.0` unless `-DTREPDF_VERSION=...` is supplied.

## Dependencies and packaging

[`prepare_dependencies.py`](../scripts/ci/prepare_dependencies.py) requires
Python 3.12 or newer. It downloads PDFium 154.0.8021.0 and verifies SHA-256 for
each platform. qpdf 12.4.1 uses the checked-in macOS ARM64 package, the official
MSVC package on Windows, and a source build on Linux. Windows also builds
static zlib 1.3.1. libxlsxwriter is built from the vendored source.

UAT enables `TREPDF_DEPLOY_RUNTIME` and uses `cmake --install` to deploy Qt,
QML imports, plugins and PDF libraries into an isolated staging directory.
The Linux launcher supplies its bundled library directory for transitive
dependencies. The archives include dependency notices and build metadata;
macOS uses `ditto` and Linux uses tar to preserve permissions and symlinks.
The workflow pins third-party Actions to commit SHAs.

[`package_uat.py`](../scripts/ci/package_uat.py) launches the staged app from a
temporary working directory with external Qt/library paths cleared and the
software backend enabled (Cocoa on macOS, offscreen on Windows/Linux).
It rejects early exits and common
QML/plugin loading failures before archiving. This is a startup smoke check;
`BUILD_TESTING=OFF` means the full feature test suite is not run. It does not
replace native rendering or manual UAT.

These are UAT archives, without a distribution signing certificate or Apple
notarization. They are not installers. The workflow does not publish a GitHub
Release.

For deployment API details, see Qt's
[QML application deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).
