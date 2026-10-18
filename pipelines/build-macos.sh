#!/usr/bin/env bash
# Local equivalent of the macOS uat job. Uses installed tools; never installs Qt/Node.
set -euo pipefail

usage() {
    cat <<'HELP'
Usage: bash pipelines/build-macos.sh [options]
  --qt PATH                  Installed desktop Qt kit (or QT_ROOT_DIR / qmake in PATH)
  --version 0.0.x            Package version; default: next local version for this architecture
  --parallel N              Build jobs (default: 3)
  --deployment-target X.Y    macOS minimum (default: 13.0 for Qt 6.11, otherwise 14.4)
  --refresh-dependencies     Re-run the pinned PDF/OCR dependency preparation
  --check                   Check local tools and print the plan without downloading/building
  -h, --help                Show this help

Examples:
  bash pipelines/build-macos.sh --qt "$HOME/Qt/6.12.0/macos"
  bash pipelines/build-macos.sh --qt "$HOME/Qt/6.11.0/macos" --version 0.0.10

Requires installed Qt >= 6.8 (QML/Quick/QuickControls2, LinguistTools, ShaderTools,
ImageFormats; TaskTree for newer Qt), Python >= 3.12, CMake, Ninja and Xcode CLT.
PYTHON may name an installed Python executable. Qt/Tools CMake and Ninja are also
recognized. Native architecture only: Apple Silicon -> arm64, Intel -> x64.
First run prepares/downloads pinned PDFium/qpdf/OCR dependencies, then caches them.
Builds Release, deploys Qt/runtime, verifies the app, and creates ZIP + SHA-256 in
artifacts/local/macos-{arm64,x64}. macOS signing is ad-hoc, without notarization.
No Node.js, GitHub login, Qt download, package-manager install or source version edit.
HELP
}
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
qt_root="${QT_ROOT_DIR:-}"
version="${TREPDF_VERSION:-}"
parallel=3
deployment_target="${MACOS_DEPLOYMENT_TARGET:-}"
refresh=0
check_only=0
while (($#)); do
    case "$1" in
        --qt|--version|--parallel|--deployment-target)
            (($# >= 2)) && [[ -n "$2" ]] || fail "Missing value for $1"
            case "$1" in
                --qt) qt_root="$2" ;; --version) version="$2" ;;
                --parallel) parallel="$2" ;; --deployment-target) deployment_target="$2" ;;
            esac
            shift 2 ;;
        --refresh-dependencies) refresh=1; shift ;;
        --check) check_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1 (use --help)" ;;
    esac
done
[[ "$(uname -s)" == Darwin ]] || fail 'Run this script on macOS.'
[[ "$parallel" =~ ^[1-9][0-9]*$ ]] || fail '--parallel must be a positive integer.'
python_bin="${PYTHON:-}"
if [[ -z "$python_bin" ]]; then
    for candidate in python3 python3.14 python3.13 python3.12; do
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c 'import sys; sys.exit(sys.version_info < (3,12))' 2>/dev/null; then
            python_bin="$(command -v "$candidate")"; break
        fi
    done
fi
[[ -n "$python_bin" ]] || fail 'Python >= 3.12 was not found. Set PYTHON to an installed executable.'
"$python_bin" -c 'import sys; sys.exit(sys.version_info < (3,12))' || fail 'Python >= 3.12 is required.'
# Discover only local installations; an explicit kit always wins.
if [[ -z "$qt_root" ]]; then
    for candidate in qmake6 qmake; do
        if command -v "$candidate" >/dev/null 2>&1; then
            qt_root="$("$candidate" -query QT_INSTALL_PREFIX)"; break
        fi
    done
fi
if [[ -z "$qt_root" ]]; then
    qt_root="$("$python_bin" - "$HOME/Qt" <<'PY'
import pathlib, re, sys
kits = []
for kit in pathlib.Path(sys.argv[1]).glob('*/macos'):
    if re.fullmatch(r'6\.\d+\.\d+', kit.parent.name) and (kit/'bin/qmake').is_file():
        kits.append((tuple(map(int, kit.parent.name.split('.'))), kit))
print(max(kits)[1] if kits else '')
PY
)"
fi
[[ -d "$qt_root" ]] || fail 'Qt was not found. Pass --qt /path/to/Qt/6.x.x/macos.'
qt_root="$(cd -- "$qt_root" && pwd -P)"
[[ -x "$qt_root/bin/qmake" && -x "$qt_root/bin/macdeployqt" && -f "$qt_root/lib/cmake/Qt6/Qt6Config.cmake" ]] || fail 'Choose an installed macOS desktop Qt kit.'
export QT_ROOT_DIR="$qt_root"
export QT_VERSION="$("$qt_root/bin/qmake" -query QT_VERSION)"
"$python_bin" - "$QT_VERSION" <<'PY'
import re, sys
if not re.fullmatch(r'6\.\d+\.\d+', sys.argv[1]) or tuple(map(int, sys.argv[1].split('.'))) < (6,8,0):
    sys.exit('Qt 6.8 or newer Qt 6 is required.')
PY
qt_tools="$(dirname -- "$(dirname -- "$qt_root")")/Tools"
for tool_dir in "$qt_tools/CMake/CMake.app/Contents/bin" "$qt_tools/CMake/bin" "$qt_tools/Ninja"; do
    if [[ -d "$tool_dir" ]]; then export PATH="$tool_dir:$PATH"; fi
done
export PATH="$qt_root/bin:$PATH"
for tool in cmake ninja xcrun codesign ditto lipo; do
    command -v "$tool" >/dev/null 2>&1 || fail "Required installed tool not found: $tool"
done
xcrun --find clang++ >/dev/null
xcrun --sdk macosx --show-sdk-path >/dev/null
architecture="$(uname -m)"
case "$architecture" in
    arm64) platform=macos-arm64 ;;
    x86_64) platform=macos-x64 ;;
    *) fail "Unsupported architecture: $architecture" ;;
esac
[[ "$("$python_bin" -c 'import platform; print(platform.machine())')" == "$architecture" ]] || fail 'Python and the shell must use the same native architecture.'
lipo "$qt_root/lib/QtCore.framework/Versions/A/QtCore" -verify_arch "$architecture" >/dev/null || fail "Qt does not contain $architecture. Choose a matching macOS kit."
if [[ -z "$deployment_target" ]]; then
    case "$QT_VERSION" in 6.11.*) deployment_target=13.0 ;; *) deployment_target=14.4 ;; esac
fi
[[ "$deployment_target" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || fail 'Invalid macOS deployment target.'
export MACOS_ARCHITECTURE="$architecture" MACOS_DEPLOYMENT_TARGET="$deployment_target"
# Keep dependency/compiler caches separate from the existing developer/CI builds.
deps_dir="$repo_dir/_deps/local/$platform/macos-$deployment_target"
build_dir="$repo_dir/build/local/$platform-qt$QT_VERSION"
output_dir="$repo_dir/artifacts/local/$platform"
stage_root="$repo_dir/build/local-stage/$platform"
if [[ -z "$version" ]]; then
    version="$("$python_bin" - "$output_dir" "$stage_root" "$platform" <<'PY'
import pathlib, re, sys
pattern = re.compile(r'TrePDF-0\.0\.([1-9][0-9]*)-' + re.escape(sys.argv[3]) + r'(?:\.zip(?:\.sha256)?)?$')
numbers = [0]
for directory in sys.argv[1:3]:
    for path in pathlib.Path(directory).glob('TrePDF-*'):
        if match := pattern.fullmatch(path.name): numbers.append(int(match[1]))
print(f'0.0.{max(numbers) + 1}')
PY
)"
fi
[[ "$version" =~ ^0\.0\.[1-9][0-9]*$ ]] || fail 'Version must be 0.0.x, with x >= 1.'
stage_dir="$stage_root/TrePDF-$version-$platform"
archive="$output_dir/TrePDF-$version-$platform.zip"
[[ ! -e "$archive" && ! -e "$archive.sha256" && ! -e "$stage_dir" ]] || fail "Version $version already has output. Choose another --version."
printf 'Qt: %s (%s)\nPlatform: %s, macOS >= %s\nVersion: %s\nBuild: %s\nPackage: %s\n' "$QT_VERSION" "$qt_root" "$platform" "$deployment_target" "$version" "$build_dir" "$archive"
if ((check_only)); then printf 'Local tool checks passed. No downloads or build performed.\n'; exit 0; fi
# A shared build/dependency cache must not be mutated by concurrent runs.
mkdir -p "$repo_dir/build/local"
lock_dir="$repo_dir/build/local/.$platform.lock"
mkdir "$lock_dir" 2>/dev/null || fail "Another $platform build is active (lock: $lock_dir). If interrupted, remove that empty lock directory."
trap 'rmdir "$lock_dir" 2>/dev/null || true' EXIT
[[ ! -e "$archive" && ! -e "$stage_dir" ]] || fail 'Output appeared while acquiring the build lock; run again for the next version.'
cd -- "$repo_dir"
# These helpers also run in CI; do not append to a caller's GitHub environment files.
unset GITHUB_ENV GITHUB_STEP_SUMMARY
if ((refresh)) || [[ ! -f "$deps_dir/manifest.json" ]]; then
    "$python_bin" scripts/ci/prepare_dependencies.py --platform "$platform" --directory "$deps_dir"
fi
# Read data as data, never source/eval the manifest as shell commands.
roots="$("$python_bin" - "$deps_dir/manifest.json" "$platform" <<'PY'
import json, pathlib, sys
manifest = json.loads(pathlib.Path(sys.argv[1]).read_text())
if manifest['platform'] != sys.argv[2]: sys.exit('Dependency platform mismatch; use --refresh-dependencies.')
for name in ('PDFium_ROOT', 'QPDF_ROOT', 'OCR_ROOT'):
    value = manifest['roots'][name]
    if '\n' in value or '\r' in value or not pathlib.Path(value).is_dir():
        sys.exit(f'Invalid cached {name}; use --refresh-dependencies.')
    print(value)
PY
)"
# Preserve each dependency path, including spaces.
root_paths=()
while IFS= read -r item; do root_paths+=("$item"); done <<< "$roots"
cmake -S "$repo_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTREPDF_BUILD_WEB=OFF \
    -DTREPDF_DEPLOY_RUNTIME=ON "-DTREPDF_VERSION=$version" \
    "-DQt6_DIR=$qt_root/lib/cmake/Qt6" "-DCMAKE_PREFIX_PATH=$qt_root" \
    "-DPDFium_ROOT=${root_paths[0]}" "-DQPDF_ROOT=${root_paths[1]}" "-DOCR_ROOT=${root_paths[2]}" \
    "-DCMAKE_OSX_ARCHITECTURES=$architecture" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$deployment_target" \
    -DCMAKE_INSTALL_BINDIR=bin -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$build_dir" --target trepdf --parallel "$parallel"
cmake --install "$build_dir" --config Release --prefix "$stage_dir"
"$python_bin" scripts/ci/package_uat.py --platform "$platform" --version "$version" \
    --stage "$stage_dir" --dependencies "$deps_dir" --output "$output_dir"
printf '\nBuild complete.\nApp: %s/trepdf.app\nZIP: %s\nChecksum: %s.sha256\n' "$stage_dir" "$archive" "$archive"
