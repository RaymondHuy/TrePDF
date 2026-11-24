#!/usr/bin/env python3
"""Transfer native macOS dependencies and merge their Intel/Apple Silicon slices."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from macos_compat import verify_binary, version_tuple


ROOTS = {"PDFium_ROOT": "pdfium", "QPDF_ROOT": "qpdf", "OCR_ROOT": "ocr"}
PLATFORMS = {"macos-arm64": "arm64", "macos-x64": "x86_64"}
LIBRARIES = (
    "pdfium/lib/libpdfium.dylib", "qpdf/lib/libqpdf.dylib",
    "ocr/lib/libtesseract.a", "ocr/lib/libleptonica.a",
)
MODELS = ("eng.traineddata", "vie.traineddata", "pdf.ttf")
REQUIRED_HEADERS = (
    "pdfium/include/fpdfview.h", "qpdf/include/qpdf/qpdf-c.h",
    "ocr/include/tesseract/baseapi.h", "ocr/include/leptonica/allheaders.h",
)


def load_manifest(directory: Path, expected_platform: str | None = None) -> dict:
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("platform") not in PLATFORMS or (
            expected_platform and manifest["platform"] != expected_platform):
        raise RuntimeError(f"Unexpected dependency platform in {directory}")
    version_tuple(manifest["macos_deployment_target"])
    if not isinstance(manifest.get("versions"), dict) or not all(
            name in manifest["versions"]
            for name in ("pdfium", "qpdf", "tesseract", "leptonica", "tessdata_fast")):
        raise RuntimeError(f"Incomplete dependency versions in {directory}")
    return manifest


def contained_file(path: Path, root: Path) -> Path:
    # Materialize internal file aliases at stable names. Escaping aliases and
    # directory symlinks must never pull unrelated runner files into an artifact.
    if not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
        raise RuntimeError(f"Missing or escaping dependency file: {path}")
    return path


def files_in(directory: Path, root: Path) -> dict[str, Path]:
    if directory.is_symlink() or not directory.is_dir():
        raise RuntimeError(f"Missing or symlinked dependency directory: {directory}")
    result = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() and path.is_dir():
            raise RuntimeError(f"Symlinked dependency directory: {path}")
        if not path.is_dir():
            result[path.relative_to(directory).as_posix()] = contained_file(path, root)
    return result


def selected_files(directory: Path, manifest: dict, *, portable: bool) -> dict[str, Path]:
    selected = {}
    for variable, name in ROOTS.items():
        configured = Path(manifest["roots"][variable])
        if portable and configured.as_posix() != name:
            raise RuntimeError(f"Non-portable dependency root: {variable}")
        root = configured if configured.is_absolute() else directory / configured
        if not root.resolve().is_relative_to(directory.resolve()):
            raise RuntimeError(f"Dependency root escapes its directory: {root}")
        for relative, source in files_in(root / "include", directory).items():
            # The checked-in qpdf include tree contains a source CMakeLists.txt
            # that an installed Intel package does not contain.
            if name == "qpdf" and (not relative.startswith("qpdf/")
                                   or source.suffix not in {".h", ".hh"}):
                continue
            selected[f"{name}/include/{relative}"] = source
        for library in LIBRARIES:
            if library.startswith(name + "/"):
                selected[library] = contained_file(root / Path(library).relative_to(name), directory)
        if name == "ocr":
            # CMake installs these notices inside the .app, independently of the
            # complete set of dependency licenses distributed alongside the app.
            for relative, source in files_in(root / "licenses", directory).items():
                selected[f"ocr/licenses/{relative}"] = source
            for model in MODELS:
                relative = f"ocr/share/tessdata/{model}"
                selected[relative] = contained_file(root / "share/tessdata" / model, directory)
    for relative in REQUIRED_HEADERS:
        if relative not in selected:
            raise RuntimeError(f"Missing public header: {relative}")
    notices = files_in(directory / "licenses", directory)
    for name in manifest["versions"]:
        if "/" in name or name in {"", ".", ".."} or not any(
                path.startswith(name + "/") for path in notices):
            raise RuntimeError(f"Missing or invalid license directory: {name}")
    selected.update({f"licenses/{relative}": source for relative, source in notices.items()})
    return selected


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def pack(dependencies: Path, output: Path) -> None:
    dependencies, output = dependencies.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(dependencies):
        raise RuntimeError("Archive output must be new and outside its dependency directory")
    manifest = load_manifest(dependencies)
    selected = selected_files(dependencies, manifest, portable=False)
    portable = {**manifest, "roots": ROOTS.copy(),
                "license_dirs": {name: f"licenses/{name}" for name in manifest["versions"]}}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".trepdf-deps-pack-", dir=output.parent) as temporary:
        temporary = Path(temporary)
        write_json(temporary / "manifest.json", portable)
        archive = temporary / "dependencies.tar.gz"
        # Only selected files enter the tar. Versioned dylib symlinks become
        # self-contained regular files; no build-tree paths survive the transfer.
        with tarfile.open(archive, "w:gz", dereference=True) as stream:
            stream.add(temporary / "manifest.json", arcname="manifest.json")
            for relative, source in sorted(selected.items()):
                stream.add(source, arcname=relative, recursive=False)
        if output.exists():
            raise RuntimeError(f"Archive output already exists: {output}")
        archive.replace(output)
    print(f"Portable dependencies: {output}", flush=True)


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def merge_library(arm64: Path, x64: Path, output: Path, target: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".trepdf-lipo-", dir=output.parent) as temporary:
        slices = []
        for source, architecture in ((arm64, "arm64"), (x64, "x86_64")):
            verify_binary(source, architecture, target)
            architectures = subprocess.check_output(["lipo", "-archs", str(source)], text=True).split()
            thin = Path(temporary) / architecture
            if architectures == [architecture]:
                shutil.copy2(source, thin)
            else:
                subprocess.run(["lipo", str(source), "-thin", architecture, "-output", str(thin)],
                               check=True)
            slices.append(str(thin))
        subprocess.run(["lipo", "-create", *slices, "-output", str(output)], check=True)
    if set(subprocess.check_output(["lipo", "-archs", str(output)], text=True).split()) != {
            "arm64", "x86_64"}:
        raise RuntimeError(f"Merged library is not universal: {output}")
    for architecture in PLATFORMS.values():
        verify_binary(output, architecture, target)


def merge(arm64: Path, x64: Path, output: Path) -> None:
    arm64, x64, output = arm64.resolve(), x64.resolve(), output.resolve()
    paths = (arm64, x64, output)
    if output.exists() or any(a.is_relative_to(b) for a in paths for b in paths if a != b) \
            or len(set(paths)) != 3:
        raise RuntimeError("Dependency inputs/output must be disjoint; output must not exist")
    manifests = [load_manifest(root, expected) for root, expected in
                 ((arm64, "macos-arm64"), (x64, "macos-x64"))]
    target = manifests[0]["macos_deployment_target"]
    if version_tuple(target) != version_tuple(manifests[1]["macos_deployment_target"]):
        raise RuntimeError("Native dependency deployment targets differ")
    versions = dict(manifests[0]["versions"])
    for name, value in manifests[1]["versions"].items():
        if name in versions and versions[name] != value:
            raise RuntimeError(f"Native dependency versions differ: {name}")
        versions[name] = value
    selections = [selected_files(root, manifest, portable=True)
                  for root, manifest in zip((arm64, x64), manifests)]
    common = [{name: source for name, source in selection.items()
               if name not in LIBRARIES and not name.startswith("licenses/")}
              for selection in selections]
    if common[0].keys() != common[1].keys():
        difference = sorted(common[0].keys() ^ common[1].keys())
        raise RuntimeError(f"Native public headers/models differ: {difference}")
    for name, source in common[0].items():
        if source.read_bytes() != common[1][name].read_bytes():
            raise RuntimeError(f"Native public header/model contents differ: {name}")
    roots = {variable: str(output / name) for variable, name in ROOTS.items()}
    if any("\n" in value or "\r" in value for value in roots.values()):
        raise RuntimeError("Dependency paths must not contain newlines")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".trepdf-universal-", dir=output.parent) as temporary:
        stage = Path(temporary) / "dependencies"
        stage.mkdir()
        for name, source in common[0].items():
            copy_file(source, stage / name)
        for name in LIBRARIES:
            merge_library(selections[0][name], selections[1][name], stage / name, target)
        notices = sorted({name for selection in selections for name in selection
                          if name.startswith("licenses/")})
        for name in notices:
            sources = [selection.get(name) for selection in selections]
            if all(sources) and sources[0].read_bytes() != sources[1].read_bytes():
                # Architecture-specific upstream notices are both redistributed.
                for platform, source in zip(PLATFORMS, sources):
                    notice = Path(name)
                    copy_file(source, stage / notice.parent / platform / notice.name)
            else:
                copy_file(sources[0] or sources[1], stage / name)
        manifest = {"platform": "macos-universal", "architectures": list(PLATFORMS.values()),
                    "macos_deployment_target": target, "roots": roots, "versions": versions,
                    "license_dirs": {name: str(output / "licenses" / name) for name in versions}}
        write_json(stage / "manifest.json", manifest)
        if output.exists():
            raise RuntimeError(f"Universal dependency output already exists: {output}")
        stage.replace(output)
    if env_file := os.environ.get("GITHUB_ENV"):
        with open(env_file, "a", encoding="utf-8") as stream:
            for name, value in roots.items():
                stream.write(f"{name}={value}\n")
    print(f"Universal dependencies ready: {output}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack_parser = commands.add_parser("pack", help="Archive only portable build inputs")
    pack_parser.add_argument("--dependencies", type=Path, required=True)
    pack_parser.add_argument("--output", type=Path, required=True)
    merge_parser = commands.add_parser("merge", help="Combine native dependency slices")
    merge_parser.add_argument("--arm64", type=Path, required=True)
    merge_parser.add_argument("--x64", type=Path, required=True)
    merge_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "pack":
        pack(args.dependencies, args.output)
    else:
        merge(args.arm64, args.x64, args.output)


if __name__ == "__main__":
    main()
