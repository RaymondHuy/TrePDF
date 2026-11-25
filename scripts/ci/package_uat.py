#!/usr/bin/env python3
"""Verify a deployed UAT application, add metadata, and optionally archive it."""

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

from app_version import app_version
from macos_compat import verify_tree, version_tuple
from macos_signing import notarize, signing_assets, take_credentials


MACOS_ARCHITECTURES = {
    "macos-arm64": ("arm64",),
    "macos-x64": ("x86_64",),
    "macos-universal": ("arm64", "x86_64"),
}


def verify_macos_bundle(app: Path) -> None:
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)


def sign_macos_bundle(app: Path, developer_id: list[str] | None = None) -> None:
    # Deployment changes Mach-O load commands and adds bundle resources after
    # linking. Seal nested code first, then its containing bundles, then the app.
    mach_o_magic = {
        b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
    }
    code = []
    for path in app.rglob("*"):
        if path.is_symlink():
            continue
        if path.is_dir() and path.suffix in {".framework", ".app", ".xpc", ".bundle"}:
            code.append(path)
        elif path.is_file():
            with path.open("rb") as stream:
                if stream.read(4) in mach_o_magic:
                    code.append(path)
    for path in sorted(code, key=lambda item: (-len(item.parts), str(item))) + [app]:
        options = [*developer_id, "--options", "runtime"] if developer_id else ["--sign", "-", "--timestamp=none"]
        if developer_id and path == app:
            entitlements = Path(__file__).resolve().parents[2] / "platform/macos/DeveloperID.entitlements"
            options += ["--entitlements", str(entitlements)]
        subprocess.run(["codesign", "--force", *options, str(path)], check=True)
    verify_macos_bundle(app)


def create_macos_dmg(stage: Path, destination: Path, version: str, *, notarized_app=False) -> None:
    # Author Finder metadata directly so CI does not need a running Finder.
    try:
        from dmgbuild import build_dmg
        from ds_store import DSStore
    except ImportError as error:
        raise RuntimeError(
            "DMG packaging requires: python -m pip install -r "
            "scripts/ci/requirements-macos-dmg.txt") from error
    from importlib.metadata import distribution

    locations = {"Tre PDF.app": (140, 120), "Applications": (500, 120),
                 "Read Me": (320, 320)}
    with tempfile.TemporaryDirectory(prefix="trepdf-dmg-") as temporary:
        # Keep the signed app and ZIP staging tree unchanged. Ancillary files
        # remain accessible without cluttering the installation row.
        support = Path(temporary) / "Read Me"
        support.mkdir()
        for item in sorted(stage.iterdir()):
            if item.name != "Tre PDF.app":
                subprocess.run(["ditto", str(item), str(support / item.name)], check=True)
        # The built-in background is redistributed with the image.
        package = distribution("dmgbuild")
        notice = next(path for path in package.files if path.name == "LICENSE")
        license_directory = support / "licenses/dmgbuild"
        license_directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(package.locate_file(notice), license_directory / "LICENSE")
        # Do not set hide_extensions: it adds FinderInfo to the signed .app.
        build_dmg(str(destination), f"Tre PDF {version}", settings={
            "format": "UDZO",
            "filesystem": "HFS+",
            "files": [str(stage / "Tre PDF.app"), str(support)],
            "symlinks": {"Applications": "/Applications"},
            "background": "builtin-arrow",
            "window_rect": ((100, 100), (640, 420)),
            "default_view": "icon-view",
            "include_icon_view_settings": True,
            "arrange_by": None,
            "grid_spacing": 80,
            "icon_size": 128,
            "text_size": 16,
            "icon_locations": locations,
            "show_toolbar": False,
            "show_sidebar": False,
            "show_status_bar": False,
            "show_pathbar": False,
            "show_tab_view": False,
        })
        subprocess.run(["hdiutil", "verify", str(destination)], check=True)
        mount = Path(temporary) / "mounted"
        mount.mkdir()
        subprocess.run(["hdiutil", "attach", str(destination), "-readonly", "-nobrowse",
                        "-mountpoint", str(mount)], check=True)
        try:
            verify_macos_bundle(mount / "Tre PDF.app")
            if notarized_app:
                subprocess.run(["xcrun", "stapler", "validate", str(mount / "Tre PDF.app")], check=True)
            shortcut = mount / "Applications"
            if not shortcut.is_symlink() or os.readlink(shortcut) != "/Applications":
                raise RuntimeError("DMG is missing its Applications shortcut")
            # Validate the actual image, including copies made by dmgbuild.
            for source in support.rglob("*"):
                target = mount / "Read Me" / source.relative_to(support)
                if source.is_file() and (not target.is_file() or hashlib.sha256(
                        source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest()):
                    raise RuntimeError(f"DMG is missing or changed: Read Me/{source.relative_to(support)}")
            if not (mount / ".background.tiff").is_file():
                raise RuntimeError("DMG is missing its installation background")
            with DSStore.open(str(mount / ".DS_Store"), "r") as store:
                if store["."]["icvp"]["backgroundType"] != 2:
                    raise RuntimeError("DMG background is not configured")
                for name, position in locations.items():
                    if tuple(store[name]["Iloc"]) != position:
                        raise RuntimeError(f"DMG icon position is incorrect: {name}")
        finally:
            subprocess.run(["hdiutil", "detach", str(mount)], check=True)


def smoke_test(executable: Path, target: str) -> None:
    # Do not let a runner's Qt/dependency paths hide missing deployed libraries.
    environment = os.environ.copy()
    for key in list(environment):
        if (key.startswith(("QT_", "QML", "DYLD_"))
                or key in {"LD_LIBRARY_PATH", "LD_PRELOAD"}):
            environment.pop(key)
    if target == "windows-x64":
        system_root = Path(environment["SYSTEMROOT"])
        environment["PATH"] = os.pathsep.join(
            str(path) for path in (system_root / "System32", system_root))
    else:
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    # macdeployqt ships Cocoa; its default deployment does not include offscreen.
    environment.update(QT_QPA_PLATFORM="cocoa" if target in MACOS_ARCHITECTURES else "offscreen",
                       QT_QUICK_BACKEND="software")
    with tempfile.TemporaryDirectory(prefix="trepdf-uat-") as temporary:
        # Also launch outside the install tree to catch working-directory assumptions.
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen([str(executable)], cwd=temporary,
                                       env=environment, stdout=log, stderr=log)
            try:
                try:
                    result = process.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    print("Deployed application stayed running for 12 seconds.", flush=True)
                else:
                    raise RuntimeError(f"Application exited during startup (code {result})")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                log.seek(0)
                output = log.read().decode("utf-8", errors="replace")
                print(output, flush=True)
            if re.search(r"failed to load component|module .+ is not installed|"
                         r"cannot load library|could not load.+platform plugin|"
                         r"unsupported image format",
                         output, re.IGNORECASE):
                raise RuntimeError("Deployed application reported a QML/plugin loading error")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("windows-x64", *MACOS_ARCHITECTURES, "linux-x64"),
                        required=True)
    parser.add_argument("--version", type=app_version, required=True)
    parser.add_argument("--variant", default="",
                        help="Optional archive suffix, for example qt6.11")
    parser.add_argument("--macos-dmg", action="store_true",
                        help="Also create and verify a drag-to-install macOS DMG")
    parser.add_argument("--macos-notarize", action="store_true",
                        help="Sign with Developer ID and notarize the app and DMG with Apple")
    parser.add_argument("--no-archive", action="store_true",
                        help="Verify the deployed Windows application folder without creating a ZIP")
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.variant and not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", args.variant):
        parser.error("Package variant must contain only lowercase letters, digits, dots or hyphens")
    if args.macos_dmg and args.platform not in MACOS_ARCHITECTURES:
        parser.error("--macos-dmg requires a macOS platform")
    if args.no_archive and args.platform != "windows-x64":
        parser.error("--no-archive requires the windows-x64 platform")
    if args.macos_notarize and not args.macos_dmg:
        parser.error("--macos-notarize requires --macos-dmg")
    credentials = take_credentials() if args.macos_notarize else None
    package = args.platform + (f"-{args.variant}" if args.variant else "")
    stage = args.stage.resolve()
    manifest = json.loads((args.dependencies / "manifest.json").read_text(encoding="utf-8"))
    if manifest["platform"] != args.platform:
        parser.error("Dependency manifest does not match the package platform")
    executable = stage / {
        "windows-x64": "bin/Tre PDF.exe",
        "macos-arm64": "Tre PDF.app/Contents/MacOS/Tre PDF",
        "macos-x64": "Tre PDF.app/Contents/MacOS/Tre PDF",
        "macos-universal": "Tre PDF.app/Contents/MacOS/Tre PDF",
        "linux-x64": "bin/trepdf",
    }[args.platform]
    if not executable.is_file():
        parser.error(f"Deployed application is missing: {executable}")

    ocr_data = (executable.parent.parent / "Resources/tessdata"
                if args.platform in MACOS_ARCHITECTURES else executable.parent / "tessdata")
    original_data = Path(manifest["roots"]["OCR_ROOT"]) / "share/tessdata"
    for filename in ("eng.traineddata", "vie.traineddata", "pdf.ttf"):
        deployed = ocr_data / filename
        if not deployed.is_file() or hashlib.sha256(deployed.read_bytes()).digest() != hashlib.sha256(
                (original_data / filename).read_bytes()).digest():
            raise RuntimeError(f"Missing or changed bundled OCR data: {filename}")

    if args.platform == "linux-x64":
        launcher = stage / "TrePDF.sh"
        launcher.write_text(
            '#!/bin/sh\n'
            'trepdf_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
            'export LD_LIBRARY_PATH="$trepdf_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            'exec "$trepdf_dir/bin/trepdf" "$@"\n', encoding="utf-8")
        launcher.chmod(0o755)
        executable = launcher

    if args.platform in MACOS_ARCHITECTURES:
        deployment_target = manifest.get("macos_deployment_target") or os.environ.get("MACOS_DEPLOYMENT_TARGET")
        if not deployment_target:
            raise RuntimeError("Missing macOS deployment target; prepare dependencies again")
        configured_target = os.environ.get("MACOS_DEPLOYMENT_TARGET", deployment_target)
        if version_tuple(configured_target) != version_tuple(deployment_target):
            raise RuntimeError("Dependency and application macOS deployment targets differ")
        with (stage / "Tre PDF.app/Contents/Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        for key in ("CFBundleName", "CFBundleDisplayName", "CFBundleExecutable"):
            if info.get(key) != "Tre PDF":
                raise RuntimeError(f"macOS {key} must be 'Tre PDF', found {info.get(key)!r}")
        bundle_version = info.get("CFBundleShortVersionString")
        if bundle_version != args.version:
            raise RuntimeError(f"Built app version {bundle_version!r} differs from package version {args.version!r}")
        app_target = info.get("LSMinimumSystemVersion", "")
        if version_tuple(app_target) != version_tuple(deployment_target):
            raise RuntimeError("Bundle minimum macOS version does not match its deployment target")
        for architecture in MACOS_ARCHITECTURES[args.platform]:
            verify_tree(stage / "Tre PDF.app", architecture, deployment_target)
        if not args.macos_notarize:
            sign_macos_bundle(stage / "Tre PDF.app")
        processor = {
            "macos-arm64": "Apple Silicon",
            "macos-x64": "Intel",
            "macos-universal": "Intel and Apple Silicon — Universal",
        }[args.platform]
        if args.macos_notarize:
            signing_note = "Signed with Developer ID Application and notarized by Apple."
            first_launch = ""
        else:
            signing_note = "This build has an ad-hoc integrity signature, without Apple notarization."
            first_launch = ("If macOS blocks it as an unidentified developer, approve this app in\n"
                            "System Settings > Privacy & Security > Open Anyway.\n")
        (stage / "START-HERE.txt").write_text(
            f"TrePDF UAT for macOS ({processor})\n\n"
            f"Minimum macOS: {deployment_target}\n"
            f"Qt: {os.environ.get('QT_VERSION', 'local')}\n\n"
            f"{signing_note}\n"
            "After verifying the package SHA-256, open the DMG and drag Tre PDF.app\n"
            "to Applications, or extract the ZIP and open Tre PDF.app.\n"
            f"{first_launch}"
            "If signature verification fails or it is reported as damaged, do not\n"
            "override that warning; download a newly built package.\n\n"
            'Verify the app: codesign --verify --deep --strict "/path/to/Tre PDF.app"\n'
            "https://support.apple.com/en-us/102445\n", encoding="utf-8")
    else:
        smoke_test(executable, args.platform)
    (stage / "VERSION").write_text(args.version + "\n", encoding="utf-8", newline="\n")
    metadata = {
        "version": args.version,
        "platform": args.platform,
        "variant": args.variant,
        "commit": os.environ.get("GITHUB_SHA", "local"),
        "run_number": os.environ.get("GITHUB_RUN_NUMBER"),
        "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "qt": os.environ.get("QT_VERSION", "local"),
        "dependencies": manifest["versions"],
    }
    if args.platform in MACOS_ARCHITECTURES:
        metadata["macos_signing"] = ("Developer ID Application; notarized" if args.macos_notarize
                                     else "ad-hoc; not notarized")
        metadata["macos_deployment_target"] = deployment_target
        metadata["macos_architectures"] = list(MACOS_ARCHITECTURES[args.platform])
    (stage / "build-info.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    shutil.copytree(args.dependencies / "licenses", stage / "licenses", dirs_exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    for filename in ("License.txt", "LICENSE.txt", "LICENSE"):
        notice = repo / "third_party/libxlsxwriter" / filename
        if notice.is_file():
            destination = stage / "licenses/libxlsxwriter"
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(notice, destination / filename)

    if args.no_archive:
        print(f"Application folder ready: {stage}\nRun: {executable}", flush=True)
        if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(summary, "a", encoding="utf-8") as stream:
                stream.write(f"### TrePDF {args.version} — {package}\n\n")
                stream.write(f"- Deployed application folder; launch `{executable.relative_to(stage).as_posix()}`.\n")
                stream.write("- No package ZIP or archive checksum was created.\n\n")
                stream.write("Release build, runtime deployment and startup smoke check passed.\n")
        return

    args.output.mkdir(parents=True, exist_ok=True)
    archive_base = args.output.resolve() / f"TrePDF-{args.version}-{package}"
    if args.macos_dmg:
        dmg = Path(str(archive_base) + ".dmg")
        if args.macos_notarize:
            app = stage / "Tre PDF.app"
            with signing_assets(credentials) as (identity, auth):
                sign_macos_bundle(app, identity)
                with tempfile.TemporaryDirectory(prefix="trepdf-notary-") as temporary:
                    submission = Path(temporary) / "trepdf.zip"
                    subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
                                    str(app), str(submission)], check=True)
                    notarize(submission, app, auth)
                create_macos_dmg(stage, dmg, args.version, notarized_app=True)
                subprocess.run(["codesign", "--force", *identity, str(dmg)], check=True)
                notarize(dmg, dmg, auth)
            subprocess.run(["codesign", "--verify", "--strict", str(dmg)], check=True)
            subprocess.run(["spctl", "--assess", "--type", "open", "--context",
                            "context:primary-signature", str(dmg)], check=True)
        else:
            create_macos_dmg(stage, dmg, args.version)
    if args.platform in MACOS_ARCHITECTURES:
        archive = Path(str(archive_base) + ".zip")
        # Preserve .app permissions and framework symlinks inside the ZIP.
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
                        str(stage), str(archive)], check=True)
        # Test what a recipient extracts, including the resource seal and symlinks.
        with tempfile.TemporaryDirectory(prefix="trepdf-archive-check-") as temporary:
            subprocess.run(["ditto", "-x", "-k", str(archive), temporary], check=True)
            restored_app = Path(temporary) / stage.name / "Tre PDF.app"
            verify_macos_bundle(restored_app)
            if args.macos_notarize:
                subprocess.run(["xcrun", "stapler", "validate", str(restored_app)], check=True)
                subprocess.run(["spctl", "--assess", "--type", "execute", str(restored_app)], check=True)
            smoke_test(restored_app / "Contents/MacOS/Tre PDF", args.platform)
    else:
        archive = Path(shutil.make_archive(str(archive_base),
                       "zip" if args.platform == "windows-x64" else "gztar",
                       root_dir=stage.parent, base_dir=stage.name))
    archives = [archive]
    if args.macos_dmg:
        archives.append(dmg)
    for archive in archives:
        digest = hashlib.sha256()
        with archive.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        checksum = f"{digest.hexdigest()}  {archive.name}\n"
        Path(str(archive) + ".sha256").write_text(checksum, encoding="utf-8")
        print(checksum, end="", flush=True)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(f"### TrePDF {args.version} — {package}\n\n")
            for archive in archives:
                stream.write(f"- `{archive.name}` and `{archive.name}.sha256`\n")
            stream.write("\nRelease build, runtime deployment and startup smoke check passed.\n")
            if args.macos_dmg:
                stream.write("DMG verification, Finder layout and mounted app signature check passed.\n")


if __name__ == "__main__":
    main()
