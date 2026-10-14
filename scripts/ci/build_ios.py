#!/usr/bin/env python3
"""Configure an isolated iPad build, archive it, and optionally export a signed IPA."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile

from ios_signing import signing_assets


def run(*args: str) -> None:
    subprocess.run(list(map(str, args)), check=True)


def verify_app(app: Path, bundle_id: str, version: str, signed: bool) -> None:
    info = plistlib.loads((app / "Info.plist").read_bytes())
    if info.get("CFBundleIdentifier") != bundle_id or info.get("CFBundleShortVersionString") != version:
        raise RuntimeError("Archived iPad application has incorrect identity/version")
    if 2 not in info.get("UIDeviceFamily", []):
        raise RuntimeError("Archived application does not support iPad")
    framework = app / "Frameworks/PDFium.framework/PDFium"
    if not framework.is_file():
        raise RuntimeError("PDFium framework is missing from the archived app")
    for binary in (app / info["CFBundleExecutable"], framework):
        headers = subprocess.check_output(["xcrun", "vtool", "-show-build", str(binary)], text=True)
        if not re.search(r"platform\s+IOS\b", headers):
            raise RuntimeError(f"Wrong platform in archived binary: {binary.name}")
        links = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        for line in links.splitlines()[1:]:
            name = line.strip().split(" (", 1)[0]
            if not name.startswith(("@rpath/", "@loader_path/", "/usr/lib/", "/System/Library/")):
                raise RuntimeError(f"Nonportable runtime dependency: {name}")
    if signed:
        run("codesign", "--verify", "--deep", "--strict", app)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-ios", type=Path, required=True)
    parser.add_argument("--qt-host", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=Path("build/ipad"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/ipad"))
    parser.add_argument("--version", default="0.0.1")
    parser.add_argument("--bundle-id", default="com.treoffice.trepdf")
    parser.add_argument("--team-id", default="")
    parser.add_argument("--configure-only", action="store_true",
                        help="Generate an Xcode project to sign/run locally, including with a Personal Team")
    parser.add_argument("--signing", choices=("unsigned", "development", "ad-hoc", "app-store-connect"),
                        default="unsigned")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("Version must use major.minor.patch")
    if not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", args.bundle_id):
        parser.error("Bundle ID must be a reverse-DNS identifier")
    if args.team_id and not re.fullmatch(r"[A-Z0-9]{10}", args.team_id):
        parser.error("Apple team ID must have 10 uppercase letters/digits")
    repo = Path(__file__).resolve().parents[2]
    manifest = json.loads((args.dependencies / "manifest.json").read_text())
    if manifest["platform"] != "ios-device-arm64":
        parser.error("Device builds require ios-device-arm64 dependencies")
    args.build = args.build.resolve()
    args.output = args.output.resolve()
    roots = manifest["roots"]
    # Qt 6.12 supports iOS 18+, including when reusing older PDF dependencies.
    deployment_target = max(
        manifest["deployment_target"], "18.0",
        key=lambda value: tuple(map(int, value.split("."))))
    configure = [str(args.qt_ios.resolve() / "bin/qt-cmake"), "-S", str(repo), "-B", str(args.build),
                 "-G", "Xcode", f"-DQT_HOST_PATH={args.qt_host.resolve()}",
                 "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_SYSROOT=iphoneos",
                 f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}",
                 "-DBUILD_TESTING=OFF", "-DTREPDF_DEPLOY_RUNTIME=OFF",
                 f"-DTREPDF_VERSION={args.version}", f"-DTREPDF_IOS_BUNDLE_ID={args.bundle_id}",
                 f"-DTREPDF_IOS_TEAM_ID={args.team_id}",
                 f"-DTREPDF_IOS_LICENSE_DIR={args.dependencies.resolve() / 'licenses'}",
                 f"-DPDFium_ROOT={roots['PDFium_ROOT']}", f"-DQPDF_ROOT={roots['QPDF_ROOT']}"]
    run(*configure)
    if args.configure_only:
        print(f"Open {args.build / 'TrePDF.xcodeproj'}; choose your Team and connected iPad, then Run.")
        return
    name = f"TrePDF-{args.version}-ipad"
    args.output = args.output / f"{name}-{args.signing}"
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.build / f"{name}.xcarchive"
    if archive.exists():
        # Only our named archive inside the selected build folder is regenerated.
        shutil.rmtree(archive)
    with signing_assets(args.bundle_id, args.signing) as assets:
        signing = ["CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"]
        if assets:
            signing = ["CODE_SIGN_STYLE=Manual", f"DEVELOPMENT_TEAM={assets['team']}",
                       f"CODE_SIGN_IDENTITY={assets['identity']}",
                       f"PROVISIONING_PROFILE_SPECIFIER={assets['uuid']}"]
        run("xcodebuild", "-project", args.build / "TrePDF.xcodeproj", "-scheme", "trepdf",
            "-configuration", "Release", "-destination", "generic/platform=iOS", "-jobs", "3",
            "-archivePath", archive, "archive", *signing)
        app = archive / "Products/Applications/trepdf.app"
        verify_app(app, args.bundle_id, args.version, bool(assets))
        package = args.output / f"{name}-unsigned.xcarchive.zip"
        if assets:
            options = args.build / "ExportOptions.plist"
            methods = {"development": "debugging", "ad-hoc": "release-testing",
                       "app-store-connect": "app-store-connect"}
            options.write_bytes(plistlib.dumps({
                "method": methods[args.signing], "destination": "export", "teamID": assets["team"],
                "signingStyle": "manual", "signingCertificate": assets["identity"],
                "provisioningProfiles": {args.bundle_id: assets["uuid"]},
                "manageAppVersionAndBuildNumber": False,
            }))
            with tempfile.TemporaryDirectory(prefix="trepdf-ipa-") as exported:
                run("xcodebuild", "-exportArchive", "-archivePath", archive,
                    "-exportOptionsPlist", options, "-exportPath", exported)
                ipas = list(Path(exported).glob("*.ipa"))
                if len(ipas) != 1:
                    raise RuntimeError("Xcode did not export exactly one IPA")
                package = args.output / f"{name}-{args.signing}.ipa"
                shutil.copy2(ipas[0], package)
                with tempfile.TemporaryDirectory(prefix="trepdf-ipa-check-") as restored:
                    run("ditto", "-x", "-k", package, restored)
                    verify_app(Path(restored) / "Payload/trepdf.app", args.bundle_id, args.version, True)
        else:
            run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", archive, package)
    metadata = {"version": args.version, "platform": "ios-device-arm64", "signing": args.signing,
                "bundle_id": args.bundle_id, "commit": os.environ.get("GITHUB_SHA", "local"),
                "dependencies": manifest["versions"], "pdf_javascript": False}
    (args.output / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n")
    shutil.copytree(args.dependencies / "licenses", args.output / "licenses", dirs_exist_ok=True)
    with package.open("rb") as stream:
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    Path(str(package) + ".sha256").write_text(f"{checksum}  {package.name}\n")
    text = ("Unsigned archive: cannot be installed directly. Build/sign locally with Xcode as documented."
            if not assets else "Signed IPA exported. Install only using the distribution method/profile selected.")
    (args.output / "START-HERE.txt").write_text(text + "\nSee docs/ipad-build.md in the source repository.\n")
    print(f"Package: {package}\n{text}")


if __name__ == "__main__":
    main()
