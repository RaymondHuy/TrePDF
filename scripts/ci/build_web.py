#!/usr/bin/env python3
"""Build the isolated Qt WebAssembly viewer and package only its static website."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile


REPO = Path(__file__).resolve().parents[2]
TARGET = "trepdf_web"


def run(*arguments: str | Path) -> None:
    command = [str(argument) for argument in arguments]
    print("Running:", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True)


def stage_site(build: Path, dependencies: Path, destination: Path, version: str) -> None:
    generated = build / "src/web"
    required = (f"{TARGET}.js", f"{TARGET}.wasm", "qtloader.js")
    for filename in required:
        if not (generated / filename).is_file():
            raise RuntimeError(f"Missing WebAssembly build output: {generated / filename}")
    manifest = json.loads((dependencies / "web-dependencies.json").read_text(encoding="utf-8"))
    runtime = dependencies / "pdfium/lib"
    for filename in ("pdfium.js", "pdfium.wasm"):
        if not (runtime / filename).is_file():
            raise RuntimeError(f"Missing PDFium browser runtime: {runtime / filename}")

    # Never turn a source checkout, SDK or build tree into a public document root.
    # Only files generated for the executable and the explicit worker are staged.
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and any(destination.iterdir()):
        marker = destination / "build-info.json"
        if not marker.is_file() or json.loads(marker.read_text())["platform"] != "web-wasm":
            raise RuntimeError(f"Refusing to replace a non-TrePDF website: {destination}")
    with tempfile.TemporaryDirectory(prefix=".trepdf-web-", dir=destination.parent) as temporary:
        site = Path(temporary) / "site"
        site.mkdir()
        shutil.copy2(REPO / "src/web/index.html", site / "index.html")
        for filename in required:
            shutil.copy2(generated / filename, site / filename)
        for filename in (f"{TARGET}.data", "qtlogo.svg"):
            if (generated / filename).is_file():
                shutil.copy2(generated / filename, site / filename)
        (site / "pdfium").mkdir()
        for filename in ("pdfium.js", "pdfium.wasm"):
            shutil.copy2(runtime / filename, site / "pdfium" / filename)
        shutil.copy2(REPO / "src/web/pdfium-worker.js", site / "pdfium/pdfium-worker.js")
        shutil.copytree(dependencies / "licenses", site / "licenses")
        shutil.copy2(REPO / "LICENSE", site / "licenses/TrePDF-LICENSE")
        (site / "licenses/Qt-NOTICE.txt").write_text(
            "This build statically links Qt 6.12.0 for WebAssembly.\n"
            "Qt sources and license information:\n"
            "https://download.qt.io/official_releases/qt/6.12/6.12.0/single/\n"
            "https://doc.qt.io/qt-6/licensing.html\n", encoding="utf-8")
        metadata = {
            "version": version, "platform": "web-wasm", "qt": "6.12.0",
            "emscripten": "5.0.5", "qt_threading": "singlethread",
            "commit": os.environ.get("GITHUB_SHA", "local"),
            "run_number": os.environ.get("GITHUB_RUN_NUMBER"),
            "dependencies": manifest,
        }
        (site / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        (site / "VERSION").write_text(version + "\n", encoding="utf-8")
        (site / "START-HERE.txt").write_text(
            "TrePDF Web UAT\n\n"
            "From this extracted folder, run:\n"
            "  python3 -m http.server 8000 --bind 127.0.0.1\n"
            "Then open http://127.0.0.1:8000 in a browser with WebGL 2.\n"
            "Do not open index.html through file://. Serve only this extracted folder.\n"
            "For hosting, upload the contents of this folder to an HTTPS static host.\n"
            "Serve .wasm as application/wasm. No backend or COOP/COEP headers are needed.\n\n"
            "This preview opens, displays and downloads the original PDF locally.\n"
            "It does not include editing, forms, OCR or persistent document storage.\n",
            encoding="utf-8")
        if destination.exists():
            shutil.rmtree(destination)
        site.replace(destination)


def archive_site(site: Path, output: Path, version: str) -> Path:
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"TrePDF-{version}-web-wasm.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in sorted(site.rglob("*")):
            if path.is_file():
                bundle.write(path, path.relative_to(site))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
    print(f"Website: {site}\nPackage: {archive}", flush=True)
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-cmake", type=Path, required=True,
                        help="qt-cmake inside Qt 6.12.0/wasm_singlethread/bin")
    parser.add_argument("--qt-host-path", type=Path, required=True,
                        help="Native desktop Qt 6.12.0 used for host tools")
    parser.add_argument("--build", type=Path, default=REPO / "build/web")
    parser.add_argument("--dependencies", type=Path, default=REPO / "_deps/web")
    parser.add_argument("--stage", type=Path, default=REPO / "build/web-site")
    parser.add_argument("--output", type=Path, default=REPO / "artifacts/web")
    parser.add_argument("--version", required=True)
    parser.add_argument("--parallel", type=int, default=3)
    args = parser.parse_args()
    if not re.fullmatch(r"0\.0\.[1-9][0-9]*", args.version):
        parser.error("UAT version must have the form 0.0.x, with x >= 1")
    if args.parallel < 1:
        parser.error("--parallel must be at least one")
    build, stage, output = args.build.resolve(), args.stage.resolve(), args.output.resolve()
    if (stage == REPO or REPO.is_relative_to(stage) or build.is_relative_to(stage)
            or output.is_relative_to(stage) or args.dependencies.resolve().is_relative_to(stage)):
        parser.error("The website must be a separate staging directory, not a parent of sources or build inputs")
    # The all_os Qt archive may not preserve executable bits on its shell wrapper.
    run("bash", args.qt_cmake.resolve(), "-S", REPO, "-B", build, "-G", "Ninja",
        "-DTREPDF_BUILD_WEB=ON", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        f"-DTREPDF_VERSION={args.version}", f"-DQT_HOST_PATH={args.qt_host_path.resolve()}")
    run("cmake", "--build", build, "--target", TARGET, "--parallel", str(args.parallel))
    stage_site(build, args.dependencies.resolve(), stage, args.version)
    archive = archive_site(stage, output, args.version)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(f"### TrePDF Web {args.version}\n\n"
                         f"Package: `{archive.name}`. Extract and serve its contents over HTTP(S).\n\n"
                         "Qt 6.12.0 single-threaded WebAssembly; PDFium runs in a separate Web Worker.\n")


if __name__ == "__main__":
    main()
