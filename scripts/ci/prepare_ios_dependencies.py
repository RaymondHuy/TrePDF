#!/usr/bin/env python3
"""Prepare checksum-pinned PDFium and static qpdf libraries for iPad builds."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys

from prepare_dependencies import (
    PDFIUM_BASE, PDFIUM_VERSION, QPDF_BASE, QPDF_NOTICES, QPDF_SOURCE_SHA256,
    QPDF_VERSION, copy_notices, download, extract, run,
)


# The upstream iOS V8 binaries enable BrowserEngineKit JIT. Ordinary iPad apps
# cannot request those browser-engine entitlements, so iPad uses non-V8 PDFium.
# Device and simulator remain separate even though both architectures are arm64.
PDFIUM_PACKAGES = {
    "ios-device-arm64": (
        "iphoneos", "pdfium-ios-device-arm64.tgz",
        "9bd1a444edab9f9610cd15beb30af0fa2c18357317117da2e9c292543a267139",
    ),
    "ios-simulator-arm64": (
        "iphonesimulator", "pdfium-ios-simulator-arm64.tgz",
        "b66dcd38508cb39fbd76fbf8cf9210feddb8960653bfbaf2e670592fc799d119",
    ),
}
JPEG_VERSION = "3.1.2"
JPEG_URL = (
    "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/"
    f"{JPEG_VERSION}/libjpeg-turbo-{JPEG_VERSION}.tar.gz"
)
JPEG_SHA256 = "8f0012234b464ce50890c490f18194f913a7b1f4e6a03d6644179fa0f867d0cf"


def sdk_path(name: str) -> Path:
    return Path(subprocess.check_output(
        ["xcrun", "--sdk", name, "--show-sdk-path"], text=True).strip())


def verify_native_platform(binary: Path, sdk: str, deployment_target: str) -> None:
    arch = subprocess.check_output(["lipo", "-archs", str(binary)], text=True).strip()
    if arch != "arm64":
        raise RuntimeError(f"Expected arm64 in {binary}, found {arch}")
    load_commands = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    expected = "2" if sdk == "iphoneos" else "7"
    platforms = set(re.findall(r"^\s*platform\s+(\d+)\s*$", load_commands, re.MULTILINE))
    if platforms != {expected}:
        raise RuntimeError(f"{binary} does not exclusively target {sdk}: {platforms}")
    minimums = re.findall(r"^\s*minos\s+([\d.]+)\s*$", load_commands, re.MULTILINE)
    version = lambda value: tuple((list(map(int, value.split("."))) + [0, 0])[:3])
    if any(version(minimum) > version(deployment_target) for minimum in minimums):
        raise RuntimeError(f"{binary} requires a newer iOS version than {deployment_target}")


def prepare_pdfium(directory: Path, target: str, deployment_target: str) -> Path:
    sdk, package, checksum = PDFIUM_PACKAGES[target]
    archive = download(PDFIUM_BASE + package, checksum,
                       directory / "downloads" / f"pdfium-{PDFIUM_VERSION}")
    prefix = directory / "pdfium"
    extract(archive, prefix)
    binary = prefix / "lib/libpdfium.dylib"
    verify_native_platform(binary, sdk, deployment_target)

    # iOS embeds frameworks, not loose third-party dylibs. Always derive this
    # framework from the freshly extracted, checksum-verified upstream bytes.
    framework = prefix / "lib/PDFium.framework"
    if framework.exists():
        shutil.rmtree(framework)
    framework.mkdir(parents=True, exist_ok=True)
    executable = framework / "PDFium"
    shutil.copy2(binary, executable)
    run("install_name_tool", "-id", "@rpath/PDFium.framework/PDFium", executable)
    info = {
        "CFBundleExecutable": "PDFium",
        "CFBundleIdentifier": "com.treoffice.pdfium",
        "CFBundleName": "PDFium",
        "CFBundlePackageType": "FMWK",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleShortVersionString": ".".join(PDFIUM_VERSION.split(".")[:3]),
        "CFBundleVersion": ".".join(PDFIUM_VERSION.split(".")[2:]),
        "CFBundleSupportedPlatforms": ["iPhoneOS" if sdk == "iphoneos" else "iPhoneSimulator"],
        "MinimumOSVersion": deployment_target,
    }
    with (framework / "Info.plist").open("wb") as stream:
        plistlib.dump(info, stream)
    copy_notices(prefix, directory / "licenses/pdfium", ("LICENSE",))
    shutil.copytree(prefix / "licenses", directory / "licenses/pdfium/third-party",
                    dirs_exist_ok=True)
    return prefix


def prepare_qpdf(directory: Path, sdk: Path, deployment_target: str,
                 parallel: int) -> Path:
    prefix = directory / "qpdf"
    common = (
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_SYSTEM_PROCESSOR=arm64",
        f"-DCMAKE_OSX_SYSROOT={sdk}",
        "-DCMAKE_OSX_ARCHITECTURES=arm64",
        "-DCMAKE_MACOSX_BUNDLE=OFF",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
        f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
    )
    jpeg_archive = download(JPEG_URL, JPEG_SHA256, directory / "downloads")
    extract(jpeg_archive, directory / "sources")
    jpeg_source = directory / "sources" / f"libjpeg-turbo-{JPEG_VERSION}"
    jpeg_build = directory / "build-jpeg"
    run("cmake", "--fresh", "-S", jpeg_source, "-B", jpeg_build, *common,
        "-DENABLE_SHARED=OFF", "-DENABLE_STATIC=ON", "-DWITH_TURBOJPEG=OFF",
        "-DWITH_TOOLS=OFF", "-DWITH_TESTS=OFF", "-DWITH_SIMD=ON")
    run("cmake", "--build", jpeg_build, "--target", "jpeg-static",
        "--parallel", str(parallel))
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    (prefix / "include").mkdir(parents=True, exist_ok=True)
    shutil.copy2(jpeg_build / "libjpeg.a", prefix / "lib/libjpeg.a")
    for header in ("jpeglib.h", "jmorecfg.h"):
        shutil.copy2(jpeg_source / "src" / header, prefix / "include" / header)
    shutil.copy2(jpeg_build / "jconfig.h", prefix / "include/jconfig.h")
    copy_notices(jpeg_source, directory / "licenses/libjpeg-turbo",
                 ("LICENSE.md", "README.ijg"))

    qpdf_archive = download(QPDF_BASE + f"qpdf-{QPDF_VERSION}.tar.gz",
                            QPDF_SOURCE_SHA256, directory / "downloads")
    extract(qpdf_archive, directory / "sources")
    source = directory / "sources" / f"qpdf-{QPDF_VERSION}"
    build = directory / "build-qpdf"
    run("cmake", "--fresh", "-S", source, "-B", build, *common,
        "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DBUILD_TESTING=OFF",
        "-DBUILD_DOC=OFF", "-DINSTALL_MANUAL=OFF", "-DINSTALL_EXAMPLES=OFF",
        "-DENABLE_QTC=OFF", "-DUSE_IMPLICIT_CRYPTO=OFF",
        "-DREQUIRE_CRYPTO_NATIVE=ON", "-DREQUIRE_CRYPTO_OPENSSL=OFF",
        "-DREQUIRE_CRYPTO_GNUTLS=OFF", "-DDEFAULT_CRYPTO=native",
        "-DUSE_INSECURE_RANDOM=OFF", "-DSKIP_OS_SECURE_RANDOM=OFF", "-DZOPFLI=OFF",
        f"-DJPEG_INCLUDE_DIR={prefix / 'include'}",
        f"-DJPEG_LIBRARY={prefix / 'lib/libjpeg.a'}",
        f"-DZLIB_INCLUDE_DIR={sdk / 'usr/include'}",
        f"-DZLIB_LIBRARY={sdk / 'usr/lib/libz.tbd'}")
    run("cmake", "--build", build, "--target", "libqpdf", "--parallel", str(parallel))
    run("cmake", "--install", build, "--component", "dev")
    copy_notices(source, directory / "licenses/qpdf", QPDF_NOTICES)
    return prefix


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=PDFIUM_PACKAGES, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--deployment-target", default="17.0")
    parser.add_argument("--parallel", type=int, default=4)
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Python 3.12 or newer is required for safe archive extraction")
    if platform.system() != "Darwin":
        parser.error("iOS cross compilation requires macOS with Xcode installed")
    if not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", args.deployment_target):
        parser.error("Deployment target must have the form 17.0 or 17.0.1")
    if tuple(int(part) for part in args.deployment_target.split(".")) < (17, 0):
        parser.error("The pinned PDFium binaries require iOS 17.0 or newer")
    if args.parallel < 1:
        parser.error("--parallel must be positive")
    for tool in ("cmake", "ninja", "xcrun", "otool", "lipo", "install_name_tool"):
        if not shutil.which(tool):
            parser.error(f"Required tool not found on PATH: {tool}")
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    sdk_name = PDFIUM_PACKAGES[args.platform][0]
    sdk = sdk_path(sdk_name)
    roots = {
        "PDFium_ROOT": str(prepare_pdfium(directory, args.platform, args.deployment_target)),
        "QPDF_ROOT": str(prepare_qpdf(directory, sdk, args.deployment_target, args.parallel)),
    }
    for name in ("libqpdf.a", "libjpeg.a"):
        verify_native_platform(Path(roots["QPDF_ROOT"]) / "lib" / name,
                               sdk_name, args.deployment_target)
    manifest = {
        "platform": args.platform,
        "sdk": sdk_name,
        "architecture": "arm64",
        "deployment_target": args.deployment_target,
        "pdf_javascript": False,
        "roots": roots,
        "versions": {"pdfium": PDFIUM_VERSION, "qpdf": QPDF_VERSION,
                     "libjpeg-turbo": JPEG_VERSION},
        "license_dirs": {
            name: str(directory / "licenses" / name)
            for name in ("pdfium", "qpdf", "libjpeg-turbo")
        },
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                               encoding="utf-8")
    if env_file := os.environ.get("GITHUB_ENV"):
        with open(env_file, "a", encoding="utf-8") as stream:
            for name, value in roots.items():
                if "\n" in value or "\r" in value:
                    raise RuntimeError("Dependency paths must not contain newlines")
                stream.write(f"{name}={value}\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
