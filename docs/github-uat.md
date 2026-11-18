# Manual UAT builds

The [`uat` workflow](../.github/workflows/uat.yml) builds Windows, Apple Silicon
and Linux Release packages. The separate
[`uat-macos-intel` workflow](../.github/workflows/uat-macos-intel.yml) builds only
the two Intel macOS packages. Run either workflow independently:

| Workflow | Package | GitHub runner | Toolchain | Output |
| --- | --- | --- | --- | --- |
| `uat` | `windows-x64` | `windows-2022` | MSVC 2022 x64, Qt 6.12.0 | Application-folder artifact, launch `bin/trepdf.exe` |
| `uat` | `macos-arm64` | `macos-15` | Apple Clang, Qt 6.12.0 | `.dmg` and `.zip` |
| `uat` | `macos-arm64-qt6.11` | `macos-15` | Apple Clang, Qt 6.11.0 | `.dmg` and `.zip` |
| `uat-macos-intel` | `macos-x64` | `macos-15-intel` | Apple Clang x86_64, Qt 6.12.0 | `.dmg` and `.zip` |
| `uat-macos-intel` | `macos-x64-qt6.11` | `macos-15-intel` | Apple Clang x86_64, Qt 6.11.0 | `.dmg` and `.zip` |
| `uat` | `linux-x64` | `ubuntu-24.04` | GCC, Qt 6.12.0 | `.tar.gz`, run `./TrePDF.sh` |

Choose `macos-arm64` for Apple Silicon (M1 or newer), or `macos-x64` for Intel
Macs. Both Qt 6.12 packages require macOS 14.4 or newer,
matching [Qt 6.12 supported platforms](https://doc.qt.io/qt-6.12/supported-platforms.html).
For macOS 13 or macOS 14.0–14.3, choose the matching architecture's `-qt6.11`
package. These target macOS 13.0 and also run on newer macOS versions, matching
[Qt 6.11 supported platforms](https://doc.qt.io/qt-6.11/supported-platforms.html).
The macOS packages do not support macOS 12 or earlier and are not universal
binaries. Intel jobs run natively on GitHub's
[`macos-15-intel` runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The Linux package targets Ubuntu 24.04
or a compatible newer desktop distribution; system glibc and graphics libraries
are not bundled. The Windows CI toolchain is MSVC; the separate local MinGW
profile documented in [windows-clion.md](windows-clion.md) is unchanged.

## Run and download

1. Commit and push the workflow, CI scripts and associated CMake/version changes.
   The workflow must exist on the default branch for GitHub to show its manual
   dispatch control.
2. For Intel, open **Actions → uat-macos-intel → Run workflow**. For Windows,
   Apple Silicon and Linux, choose **Actions → uat → Run workflow** instead.
   Select the branch and start the run.
3. On macOS, download the `TrePDF-0.0.x-<package>.dmg` entry under **Artifacts**
   in that workflow run. This downloads the DMG directly, with no outer ZIP to
   extract. For an Intel Mac, choose `TrePDF-0.0.x-macos-x64.dmg` (macOS 14.4+)
   or `TrePDF-0.0.x-macos-x64-qt6.11.dmg` (macOS 13+). The matching
   `.dmg.sha256` entry is a separate direct download for checksum verification.
4. To verify the DMG, save its `.dmg.sha256` file in the same directory and run
   the following command there, replacing the filename with your package:

   ```sh
   shasum -a 256 -c TrePDF-0.0.x-macos-arm64.dmg.sha256
   ```

   Open the `.dmg`: TrePDF appears on the left, with an arrow pointing to the
   **Applications** shortcut on the right. Drag the app onto that shortcut,
   eject the disk image and open TrePDF from Applications. Open
   `Read Me/START-HERE.txt` inside the image for signature verification and
   first-launch instructions.

The `TrePDF-0.0.x-<package>` artifact without a filename extension remains the
ZIP fallback on macOS: extract that artifact, then the package ZIP inside it.
It contains the ZIP and its checksum, while the DMG and DMG checksum have their
own entries. On Linux, download this package artifact and extract the platform
archive before launching the app.

On Windows, download `TrePDF-0.0.x-windows-x64` and extract the GitHub artifact
once, then run `bin/trepdf.exe`. The workflow uploads the deployed application
folder directly, so there is no second package ZIP or archive checksum inside.
GitHub's artifact download still wraps the folder in a ZIP. Keep all extracted
libraries, plugins and resources together with the executable.

These packaging changes apply to new runs of the updated workflow. After pushing
these changes, choose **Run workflow** and the updated branch. **Re-run jobs**
on an older run uses that run's old workflow and retains its previous packaging.
See GitHub's [re-run behavior](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs).

There are no automatic push, pull-request or schedule triggers. `uat` runs four
jobs; `uat-macos-intel` runs two. Neither workflow triggers the other. Within
each workflow, a failed matrix job does not cancel its siblings. Artifacts
are retained for 30 days, subject to repository policy. Both workflows only need
`contents: read`; no custom secret, tag write or release publication is required.

With GitHub CLI authenticated, you can also start only the Intel workflow from
the repository directory and watch its progress:

```sh
gh workflow run uat-macos-intel.yml
gh run watch
```

The first command uses the default branch. Add `--ref your-branch` to select
another pushed branch; select the Intel run when `gh run watch` prompts.

## Automatic version

`TREPDF_VERSION` is `0.0.${{ github.run_number }}`. Each workflow has its own
counter: its first new run is `0.0.1`, the next is `0.0.2`, and so on. All packages
in one run share the version, but separate workflows do not synchronize their
version numbers. The new Intel workflow starts its own sequence; it does not
continue the old Intel jobs' `uat` run numbers. Identify older and newer builds
by their workflow run and source commit when comparing across this split.
Failed or cancelled runs consume their number, so downloadable versions may
have gaps. **Re-run jobs** keeps the same version; start a new **Run workflow**
to increment it. This follows GitHub's
[workflow run-number semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/variables).

The version is passed to CMake and embedded in the application, macOS bundle
and Windows executable metadata. It is also recorded in the output folder/archive name,
`VERSION`, and `build-info.json` alongside the source commit and dependency
versions. Each Qt 6.11 package uses distinct names for its staging directory,
artifact, ZIP, DMG and checksums; its metadata records `qt: 6.11.0` and
`variant: qt6.11`.
Each macOS job uses native PDF dependencies matching its architecture.
The build does not commit a version bump back to the repository.
Local builds keep the default `0.1.0` unless `-DTREPDF_VERSION=...` is supplied.

## Dependencies and packaging

[`prepare_dependencies.py`](../scripts/ci/prepare_dependencies.py) requires
Python 3.12 or newer. It downloads PDFium 157.0.8100.0 and verifies SHA-256 for
each platform. Desktop and iOS downloads use `downloads/pdfium-<version>/`
so cached archives from an earlier release do not block an upgrade.
qpdf 12.4.1 uses the checked-in macOS ARM64 package, the official
MSVC package on Windows, and a source build on Linux and Intel macOS. Intel
builds use pinned static libjpeg-turbo 3.1.2 and the macOS SDK's system zlib,
with the same architecture and deployment target as the app. Windows builds
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

Windows passes `--no-archive` to `package_uat.py`. OCR validation, startup
verification, dependency notices and build metadata are retained in the deployed
folder. The workflow uploads that folder with compression disabled, without
creating an additional package ZIP or `.sha256` file. The local
`pipelines/build-windows.ps1` uses the same mode and leaves its runnable folder
under `artifacts/local/windows-x64/`.

`uat`, `uat-macos-intel` and `uat-ipad` use Node.js 24 releases of Checkout, Setup Python
and Upload Artifact. Desktop Qt installation also uses a Node.js 24 action;
Windows initializes MSVC directly through Visual Studio's Developer PowerShell
instead of a JavaScript action. This runtime belongs to GitHub's build runner,
is not bundled with TrePDF, and does not change the macOS deployment targets
or require Node.js on the end user's computer.

Qt 6.12 also needs the `qttasktree` module for its QML dependencies. Windows
uses aqtinstall pinned to upstream commit
`076e1659807d0b362a3ed684d54c2e9c775eb9c7`, which understands Qt's split
Windows repositories; released aqtinstall 3.3.0 cannot resolve those packages.
macOS, Linux and the iPad workflow retain aqtinstall 3.3.0. Both macOS
architectures use Qt's universal `clang_64` package; the workflow selects
`arm64` or `x86_64` through `CMAKE_OSX_ARCHITECTURES`.

After macOS deployment, packaging signs nested Mach-O files and frameworks from
the inside out, then signs the complete app with an ad-hoc signature. It verifies
the complete resource seal with `codesign --verify --deep --strict`, creates the
ZIP, extracts it again, and repeats verification and startup on the extracted
app. This catches incomplete linker signatures and signatures invalidated by
library deployment before uploading an artifact.
Packaging also checks that the app executable contains the requested ARM64 or
x86_64 architecture before signing, so an incorrectly labelled archive is rejected.

Both desktop workflows pass `--macos-dmg` to `package_uat.py` for their macOS
jobs. macOS jobs install the pinned dependencies in
[`requirements-macos-dmg.txt`](../scripts/ci/requirements-macos-dmg.txt):
`dmgbuild` 1.6.7, `ds-store` 1.3.3 and `mac-alias` 2.2.3. Packaging creates a
compressed UDZO disk image with a saved 640 × 420 Finder icon view: `trepdf.app`
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
and ZIP checksum. Packaging needs no additional secrets. The local
`pipelines/build-macos.sh` script does not enable this option and continues to
produce ZIP packages.

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
fixed package. These instructions are in `Read Me/START-HERE.txt` on the DMG
and `START-HERE.txt` in the ZIP package.
Public distribution without this manual approval needs Developer ID signing and
Apple notarization. The workflow does not change Gatekeeper settings or publish
a GitHub Release. The DMG provides drag-to-install packaging; it does not add
Developer ID signing or notarization.

For deployment API details, see Qt's
[QML application deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).
