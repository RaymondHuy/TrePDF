#!/usr/bin/env python3
"""Build the pinned, static desktop OCR package; ordinary app builds stay offline.

Requires CMake, Ninja and a native C/C++ toolchain. On Windows use the matching
MSVC developer shell, or --windows-toolchain mingw with gcc/g++ on PATH.
Supply --zlib-root built for the selected Windows compiler.
Use --offline with a populated cache for reproducible disconnected rebuilds.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import platform
import shutil
import subprocess
import tarfile
import urllib.request

TESSERACT_VERSION = "5.5.3"
LEPTONICA_VERSION = "1.87.0"
MODEL_REVISION = "87416418657359cb625c412a48b6e1d6d41c29bd"
PACKAGES = {
    "tesseract.tar.gz": (
        f"https://github.com/tesseract-ocr/tesseract/archive/refs/tags/{TESSERACT_VERSION}.tar.gz",
        "9218e62793116d42a9f6d14cd9348518b27f382096eea3d0f2d1a24616bb5884"),
    "leptonica.tar.gz": (
        f"https://github.com/DanBloomberg/leptonica/releases/download/{LEPTONICA_VERSION}/leptonica-{LEPTONICA_VERSION}.tar.gz",
        "c73363397f96eb1295602bf44d708a994ad42046c791bf03ea0505d829bdb6a7"),
    "eng.traineddata": (
        f"https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{MODEL_REVISION}/eng.traineddata",
        "7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2"),
    "vie.traineddata": (
        f"https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{MODEL_REVISION}/vie.traineddata",
        "79df64caf7bcfb2a27df5042ecb6121e196eada34da774956995747636d5bfa1"),
    "tessdata-LICENSE": (
        f"https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{MODEL_REVISION}/LICENSE",
        "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"),
}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


class SecureRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise RuntimeError("Refusing non-HTTPS dependency redirect")
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def fetch(name, cache, offline):
    url, checksum = PACKAGES[name]
    path = cache / name
    if not path.exists():
        if offline:
            raise RuntimeError(f"Offline cache is missing {name}")
        temporary = path.with_suffix(path.suffix + ".part")
        opener = urllib.request.build_opener(SecureRedirect())
        with opener.open(url, timeout=120) as response, temporary.open("wb") as out:
            shutil.copyfileobj(response, out, 1024 * 1024)
        if digest(temporary) != checksum:
            temporary.unlink()
            raise RuntimeError(f"Checksum mismatch for {name}")
        temporary.replace(path)
    if digest(path) != checksum:
        raise RuntimeError(f"Checksum mismatch for {name}")
    return path


def extract(archive, destination):
    # These releases contain only regular files/directories. Reject links,
    # devices and escaping paths before extracting, also on Python 3.9.
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            relative = PurePosixPath(member.name)
            if relative.is_absolute() or PureWindowsPath(member.name).drive or "\\" in member.name or ".." in relative.parts:
                raise RuntimeError(f"Unsafe archive path: {member.name}")
            target = (destination / member.name).resolve()
            if not target.is_relative_to(destination.resolve()) or not (
                    member.isfile() or member.isdir()):
                raise RuntimeError(f"Unsafe archive entry: {member.name}")
        source.extractall(destination)


def run(*arguments):
    subprocess.run([str(value) for value in arguments], check=True)


def build(args):
    prefix = args.prefix.resolve()
    work = args.work.resolve()
    cache = work / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    for name in PACKAGES:
        fetch(name, cache, args.offline)
    sources = work / "sources"
    sources.mkdir(exist_ok=True)
    for name in ("tesseract", "leptonica"):
        extract(cache / f"{name}.tar.gz", sources)

    common = [f"-DCMAKE_BUILD_TYPE={args.build_type}", "-DBUILD_SHARED_LIBS=OFF",
              "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DCMAKE_INSTALL_LIBDIR=lib"]
    if args.ninja:
        common.append(f"-DCMAKE_MAKE_PROGRAM={args.ninja}")
    if args.zlib_root:
        common.append(f"-DZLIB_ROOT={args.zlib_root.resolve()}")
    if platform.system() == "Darwin":
        common += [f"-DCMAKE_OSX_ARCHITECTURES={args.arch or platform.machine()}",
                   f"-DCMAKE_OSX_DEPLOYMENT_TARGET={args.macos_target}"]
    elif platform.system() == "Windows":
        if args.windows_toolchain == "mingw":
            gcc, gxx = shutil.which("gcc"), shutil.which("g++")
            if not gcc or not gxx:
                raise RuntimeError("MinGW gcc and g++ are required on PATH")
            common += [f"-DCMAKE_C_COMPILER={gcc}", f"-DCMAKE_CXX_COMPILER={gxx}"]
        else:
            common += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
                       "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"]

    lept_source = sources / f"leptonica-{LEPTONICA_VERSION}"
    tess_source = sources / f"tesseract-{TESSERACT_VERSION}"
    lept_build = work / "build-leptonica"
    lept_install = work / "leptonica-install"
    run(args.cmake, "-S", lept_source, "-B", lept_build, "-G", "Ninja", *common,
        f"-DCMAKE_INSTALL_PREFIX={lept_install}", "-DSW_BUILD=OFF", "-DBUILD_PROG=OFF",
        "-DENABLE_ZLIB=ON", "-DENABLE_PNG=OFF", "-DENABLE_JPEG=OFF",
        "-DENABLE_TIFF=OFF", "-DENABLE_WEBP=OFF", "-DENABLE_OPENJPEG=OFF",
        "-DENABLE_GIF=OFF", "-DSTRICT_CONF=ON")
    run(args.cmake, "--build", lept_build, "--parallel", args.jobs)
    run(args.cmake, "--install", lept_build, "--config", args.build_type)

    tess_build = work / "build-tesseract"
    run(args.cmake, "-S", tess_source, "-B", tess_build, "-G", "Ninja", *common,
        f"-DCMAKE_PREFIX_PATH={lept_install}", "-DBUILD_TRAINING_TOOLS=OFF",
        "-DBUILD_TESTS=OFF", "-DOPENMP_BUILD=OFF", "-DGRAPHICS_DISABLED=ON",
        "-DDISABLED_LEGACY_ENGINE=ON", "-DDISABLE_TIFF=ON", "-DDISABLE_ARCHIVE=ON",
        "-DDISABLE_CURL=ON", "-DENABLE_NATIVE=OFF", "-DENABLE_LTO=OFF",
        "-DCMAKE_CXX_FLAGS=-DTESSERACT_DISABLE_DEBUG_FONTS")
    run(args.cmake, "--build", tess_build, "--target", "libtesseract", "--parallel", args.jobs)

    # Keep only the public headers and static libraries, not absolute-path CMake
    # exports, CLI executables or build-tree artifacts.
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copytree(lept_install / "include", prefix / "include", dirs_exist_ok=True)
    tess_headers = prefix / "include/tesseract"
    tess_headers.mkdir(parents=True, exist_ok=True)
    for header in (tess_source / "include/tesseract").glob("*.h"):
        shutil.copy2(header, tess_headers / header.name)
    shutil.copy2(tess_build / "include/tesseract/version.h", tess_headers / "version.h")
    suffix = ".lib" if platform.system() == "Windows" and args.windows_toolchain == "msvc" else ".a"
    for source, pattern, name in ((lept_install / "lib", "*leptonica*", "leptonica"),
                                  (tess_build, "*tesseract*", "tesseract")):
        libraries = list(source.glob(pattern + suffix))
        if not libraries and name == "leptonica" and platform.system() == "Windows" and args.windows_toolchain == "mingw":
            # Leptonica 1.87 assigns a .dll suffix to every MinGW target,
            # including static archives. Normalize it only after verification.
            libraries = list(source.glob(pattern + ".dll"))
        if len(libraries) != 1:
            raise RuntimeError(f"Expected one static {name} library, found {libraries}")
        with libraries[0].open("rb") as library:
            if library.read(8) != b"!<arch>\n":
                raise RuntimeError(f"Expected a static archive: {libraries[0]}")
        filename = name + suffix if suffix == ".lib" else "lib" + name + suffix
        shutil.copy2(libraries[0], prefix / "lib" / filename)
    data = prefix / "share/tessdata"
    data.mkdir(parents=True, exist_ok=True)
    for name in ("eng.traineddata", "vie.traineddata"):
        shutil.copy2(cache / name, data / name)
    shutil.copy2(tess_source / "tessdata/pdf.ttf", data / "pdf.ttf")
    notices = prefix / "licenses"
    notices.mkdir(exist_ok=True)
    for source, name in ((lept_source / "leptonica-license.txt", "leptonica.txt"),
                         (tess_source / "LICENSE", "tesseract.txt"),
                         (cache / "tessdata-LICENSE", "tessdata.txt")):
        shutil.copy2(source, notices / name)
    manifest = {"tesseract": TESSERACT_VERSION, "leptonica": LEPTONICA_VERSION,
                "models": MODEL_REVISION, "build_type": args.build_type, "system": platform.system(),
                "architecture": args.arch or platform.machine(),
                "windows_toolchain": args.windows_toolchain if platform.system() == "Windows" else None,
                "macos_target": args.macos_target if platform.system() == "Darwin" else None}
    (prefix / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    files = [p for directory in ("lib", "include", "share", "licenses")
             for p in (prefix / directory).rglob("*") if p.is_file()]
    (prefix / "SHA256SUMS").write_text("".join(
        f"{digest(p)}  {p.relative_to(prefix).as_posix()}\n" for p in sorted(files)))
    print(f"OCR package ready: {prefix}")


if __name__ == "__main__":
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=repo / "third_party/ocr")
    parser.add_argument("--work", type=Path, default=repo / "_deps/ocr")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja")
    parser.add_argument("--zlib-root", type=Path)
    parser.add_argument("--windows-toolchain", choices=("msvc", "mingw"), default="msvc",
                        help="Windows compiler family; MinGW requires gcc/g++ on PATH")
    parser.add_argument("--arch", default=os.environ.get("MACOS_ARCHITECTURE"))
    parser.add_argument("--macos-target", default=os.environ.get("MACOS_DEPLOYMENT_TARGET") or "13.0")
    parser.add_argument("--build-type", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--jobs", default="4")
    parser.add_argument("--offline", action="store_true")
    build(parser.parse_args())
