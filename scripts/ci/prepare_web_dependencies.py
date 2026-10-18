#!/usr/bin/env python3
"""Fetch the isolated PDFium JavaScript/WASM engine used by the web preview."""

import argparse
import json
import os
from pathlib import Path

from prepare_dependencies import (
    PDFIUM_BASE, PDFIUM_VERSION, copy_notices, download, extract,
)


PDFIUM_WEB_SHA256 = "0d68fef9aca2c69f20717c697f90f4fcb7d66a7a30307ad527769d18707394c8"


def prepare(directory: Path) -> Path:
    archive = download(PDFIUM_BASE + "pdfium-wasm.tgz", PDFIUM_WEB_SHA256,
                       directory / "downloads")
    prefix = directory / "pdfium"
    extract(archive, prefix)
    version_fields = dict(line.split("=", 1) for line in
                          (prefix / "VERSION").read_text(encoding="utf-8").splitlines()
                          if "=" in line)
    actual_version = ".".join(version_fields.get(key, "")
                              for key in ("MAJOR", "MINOR", "BUILD", "PATCH"))
    if actual_version != PDFIUM_VERSION:
        raise RuntimeError(f"Unexpected web PDFium version: {actual_version}")
    for name in ("pdfium.js", "pdfium.wasm"):
        if not (prefix / "lib" / name).is_file():
            raise RuntimeError(f"Missing web engine asset: {name}")
    if (prefix / "lib/pdfium.wasm").read_bytes()[:8] != b"\x00asm\x01\x00\x00\x00":
        raise RuntimeError("PDFium engine is not a WebAssembly module")
    copy_notices(prefix, directory / "licenses/pdfium", ("LICENSE",))
    copy_notices(prefix / "licenses", directory / "licenses/pdfium/third-party",
                 tuple(path.name for path in sorted((prefix / "licenses").iterdir())
                       if path.is_file()))
    (directory / "web-dependencies.json").write_text(json.dumps({
        "pdfium_version": PDFIUM_VERSION,
        "pdfium_archive_sha256": PDFIUM_WEB_SHA256,
        "pdfium_javascript": False,
        "pdfium_xfa": False,
        "runtime": "isolated-web-worker",
    }, indent=2) + "\n", encoding="utf-8")
    return prefix / "lib"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    root = prepare(args.directory.resolve())
    if os.environ.get("GITHUB_ENV"):
        with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
            output.write(f"PDFIUM_WEB_ROOT={root}\n")
    print(f"PDFIUM_WEB_ROOT={root}")


if __name__ == "__main__":
    main()
