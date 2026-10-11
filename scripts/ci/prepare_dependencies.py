#!/usr/bin/env python3
"""Prepare checksum-pinned native dependencies for the manual UAT build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile


PDFIUM_VERSION = "157.0.8086.0"
PDFIUM_BASE = "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8086/"
PDFIUM_PACKAGES = {
    "windows-x64": (
        "pdfium-v8-win-x64.tgz",
        "cb8f1e0e0c0dd74cbb17e8ec11d8969c4b95638fd3e66fd1c7e4a684d28a0d46",
    ),
    "macos-arm64": (
        "pdfium-v8-mac-arm64.tgz",
        "0c71e67f1e65ac00d92365c92b57acd3e11ae9b2c1866977872eaecc49344938",
    ),
    "linux-x64": (
        "pdfium-v8-linux-x64.tgz",
        "8e3efa4fe6784ed80fd58ef3c839b072ddcd79b16880706892a008f8a2da730b",
    ),
}
QPDF_VERSION = "12.4.1"
QPDF_BASE = f"https://github.com/qpdf/qpdf/releases/download/v{QPDF_VERSION}/"
QPDF_WINDOWS_SHA256 = "3cd016cd433ef7232e42f4c13348a49cc14907a3c7278ef4f99120593126f7a6"
QPDF_SOURCE_SHA256 = "f045aa277be2356ff53a89a8622945958291177d2483afc20ede7c8a8cd3873c"
ZLIB_VERSION = "1.3.1"
ZLIB_URL = f"https://github.com/madler/zlib/releases/download/v{ZLIB_VERSION}/zlib-{ZLIB_VERSION}.tar.gz"
ZLIB_SHA256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
QPDF_NOTICES = ("LICENSE.txt", "NOTICE.md", "Artistic-2.0")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(path: Path, expected: str) -> None:
    if sha256(path) != expected:
        raise RuntimeError(f"SHA-256 mismatch: {path}; refusing to use this dependency")


class HTTPSRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise RuntimeError(f"Refusing non-HTTPS redirect: {newurl}")
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def download(url: str, expected: str, directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / url.rsplit("/", 1)[1]
    if destination.exists():
        verify(destination, expected)
        return destination
    temporary = destination.with_name(destination.name + ".part")
    opener = urllib.request.build_opener(HTTPSRedirectHandler())
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "TrePDF-UAT/1"})
            with opener.open(request, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output, length=1024 * 1024)
            break
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            temporary.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(2 ** attempt)
    verify(temporary, expected)
    temporary.replace(destination)
    print(f"Verified {destination.name}", flush=True)
    return destination


def safe_member(name: str, destination: Path) -> Path:
    # Reject Windows drive/UNC paths even when checking an archive on Unix.
    relative = PurePosixPath(name)
    if (relative.is_absolute() or PureWindowsPath(name).drive
            or "\\" in name or ".." in relative.parts):
        raise RuntimeError(f"Unsafe archive path: {name}")
    target = destination.joinpath(*relative.parts)
    if not target.resolve().is_relative_to(destination.resolve()):
        raise RuntimeError(f"Archive path escapes its destination: {name}")
    return target


def extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                target = safe_member(member.filename, destination)
                if stat.S_ISLNK(member.external_attr >> 16):
                    raise RuntimeError(f"Unexpected ZIP symlink: {member.filename}")
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(member) as data, target.open("wb") as output:
                        shutil.copyfileobj(data, output)
    else:
        with tarfile.open(archive, "r:*") as source:
            for member in source.getmembers():
                safe_member(member.name, destination)
            # Python's data filter rejects escaping links/devices but preserves
            # legitimate relative library symlinks used by Linux packages.
            source.extractall(destination, filter="data")


def run(*arguments: str | Path) -> None:
    command = [str(argument) for argument in arguments]
    print("Running:", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True)


def copy_notices(source: Path, destination: Path, names: tuple[str, ...]) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for name in names:
        shutil.copy2(source / name, destination / name)


def checked_in_qpdf(repo: Path) -> Path:
    source = repo / "third_party/qpdf"
    if (source / "VERSION").read_text(encoding="utf-8").strip() != QPDF_VERSION:
        raise RuntimeError("Bundled qpdf version differs from the UAT dependency pin")
    for line in (source / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        expected, name = line.split(maxsplit=1)
        verify(safe_member(name.lstrip("*"), source), expected)
    return source


def prepare_qpdf(target: str, directory: Path, repo: Path) -> Path:
    if target == "macos-arm64":
        prefix = directory / "qpdf"
        shutil.copytree(checked_in_qpdf(repo), prefix, dirs_exist_ok=True, symlinks=True)
        notices = prefix
    elif target == "windows-x64":
        archive = download(
            QPDF_BASE + f"qpdf-{QPDF_VERSION}-msvc64.zip",
            QPDF_WINDOWS_SHA256, directory / "downloads")
        extract(archive, directory / "qpdf-package")
        prefix = directory / "qpdf-package" / f"qpdf-{QPDF_VERSION}-msvc64"
        # The Windows ZIP omits standalone notices. The repository contains
        # unchanged upstream notices for this same pinned release.
        notices = checked_in_qpdf(repo)
    else:
        archive = download(
            QPDF_BASE + f"qpdf-{QPDF_VERSION}.tar.gz",
            QPDF_SOURCE_SHA256, directory / "downloads")
        extract(archive, directory / "sources")
        source = directory / "sources" / f"qpdf-{QPDF_VERSION}"
        build = directory / "build-qpdf"
        prefix = directory / "qpdf"
        run("cmake", "-S", source, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
            "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_SHARED_LIBS=ON",
            "-DBUILD_STATIC_LIBS=OFF", "-DBUILD_DOC=OFF", "-DINSTALL_MANUAL=OFF",
            "-DINSTALL_EXAMPLES=OFF", "-DENABLE_QTC=OFF", "-DUSE_IMPLICIT_CRYPTO=OFF",
            "-DREQUIRE_CRYPTO_NATIVE=ON", "-DREQUIRE_CRYPTO_OPENSSL=OFF",
            "-DREQUIRE_CRYPTO_GNUTLS=OFF", "-DDEFAULT_CRYPTO=native",
            "-DUSE_INSECURE_RANDOM=OFF", "-DSKIP_OS_SECURE_RANDOM=OFF", "-DZOPFLI=OFF")
        run("cmake", "--build", build, "--target", "libqpdf", "--parallel", "4")
        for component in ("lib", "dev"):
            run("cmake", "--install", build, "--component", component)
        notices = source
    copy_notices(notices, directory / "licenses/qpdf", QPDF_NOTICES)
    return prefix


def prepare_zlib(directory: Path) -> Path:
    archive = download(ZLIB_URL, ZLIB_SHA256, directory / "downloads")
    extract(archive, directory / "sources")
    source = directory / "sources" / f"zlib-{ZLIB_VERSION}"
    build = directory / "build-zlib"
    prefix = directory / "zlib"
    run("cmake", "-S", source, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DZLIB_BUILD_EXAMPLES=OFF",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
    run("cmake", "--build", build, "--target", "zlibstatic", "--parallel", "4")
    (prefix / "include").mkdir(parents=True, exist_ok=True)
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "zlib.h", prefix / "include/zlib.h")
    shutil.copy2(build / "zconf.h", prefix / "include/zconf.h")
    shutil.copy2(build / "zlibstatic.lib", prefix / "lib/zlibstatic.lib")
    copy_notices(source, directory / "licenses/zlib", ("LICENSE", "README"))
    return prefix


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=PDFIUM_PACKAGES, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Python 3.12 or newer is required for safe archive extraction")
    expected_system, expected_machines = {
        "windows-x64": ("Windows", {"amd64", "x86_64"}),
        "macos-arm64": ("Darwin", {"arm64", "aarch64"}),
        "linux-x64": ("Linux", {"amd64", "x86_64"}),
    }[args.platform]
    if platform.system() != expected_system or platform.machine().lower() not in expected_machines:
        parser.error(f"{args.platform} requires a matching native OS and architecture")

    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    package, checksum = PDFIUM_PACKAGES[args.platform]
    archive = download(PDFIUM_BASE + package, checksum,
                       directory / "downloads" / f"pdfium-{PDFIUM_VERSION}")
    pdfium = directory / "pdfium"
    extract(archive, pdfium)
    copy_notices(pdfium, directory / "licenses/pdfium", ("LICENSE",))
    shutil.copytree(pdfium / "licenses", directory / "licenses/pdfium/third-party",
                    dirs_exist_ok=True)
    roots = {"PDFium_ROOT": str(pdfium),
             "QPDF_ROOT": str(prepare_qpdf(args.platform, directory, repo))}
    versions = {"pdfium": PDFIUM_VERSION, "qpdf": QPDF_VERSION}
    if args.platform == "windows-x64":
        roots["ZLIB_ROOT"] = str(prepare_zlib(directory))
        versions["zlib"] = ZLIB_VERSION
    manifest = {
        "platform": args.platform,
        "roots": roots,
        "versions": versions,
        "license_dirs": {name: str(directory / "licenses" / name) for name in versions},
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if env_file := os.environ.get("GITHUB_ENV"):
        with open(env_file, "a", encoding="utf-8") as stream:
            for name, value in roots.items():
                if "\n" in value or "\r" in value:
                    raise RuntimeError("Dependency paths must not contain newlines")
                stream.write(f"{name}={value}\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
