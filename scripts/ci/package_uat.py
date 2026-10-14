#!/usr/bin/env python3
"""Check the deployed UAT application and archive it with its build metadata."""

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


def verify_macos_bundle(app: Path) -> None:
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)


def sign_macos_bundle(app: Path) -> None:
    # Deployment changes Mach-O load commands and adds bundle resources after
    # linking. Seal nested code first, then its containing bundles, then the app.
    # Ad-hoc signing preserves integrity for UAT; it is not Developer ID/notarization.
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
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(path)],
                       check=True)
    verify_macos_bundle(app)


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
    environment.update(QT_QPA_PLATFORM="cocoa" if target == "macos-arm64" else "offscreen",
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
    parser.add_argument("--platform", choices=("windows-x64", "macos-arm64", "linux-x64"),
                        required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--variant", default="",
                        help="Optional archive suffix, for example qt6.11")
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"0\.0\.[1-9][0-9]*", args.version):
        parser.error("UAT version must have the form 0.0.x, with x >= 1")
    if args.variant and not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", args.variant):
        parser.error("Package variant must contain only lowercase letters, digits, dots or hyphens")
    package = args.platform + (f"-{args.variant}" if args.variant else "")
    stage = args.stage.resolve()
    manifest = json.loads((args.dependencies / "manifest.json").read_text(encoding="utf-8"))
    if manifest["platform"] != args.platform:
        parser.error("Dependency manifest does not match the package platform")
    executable = stage / {
        "windows-x64": "bin/trepdf.exe",
        "macos-arm64": "trepdf.app/Contents/MacOS/trepdf",
        "linux-x64": "bin/trepdf",
    }[args.platform]
    if not executable.is_file():
        parser.error(f"Deployed application is missing: {executable}")

    if args.platform == "linux-x64":
        launcher = stage / "TrePDF.sh"
        launcher.write_text(
            '#!/bin/sh\n'
            'trepdf_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
            'export LD_LIBRARY_PATH="$trepdf_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            'exec "$trepdf_dir/bin/trepdf" "$@"\n', encoding="utf-8")
        launcher.chmod(0o755)
        executable = launcher

    if args.platform == "macos-arm64":
        sign_macos_bundle(stage / "trepdf.app")
        (stage / "START-HERE.txt").write_text(
            "TrePDF UAT for macOS (Apple Silicon)\n\n"
            "This build has an ad-hoc integrity signature, without Apple notarization.\n"
            "After verifying the archive SHA-256, extract it and open trepdf.app.\n"
            "If macOS blocks it as an unidentified developer, approve this app in\n"
            "System Settings > Privacy & Security > Open Anyway.\n"
            "If signature verification fails or it is reported as damaged, do not\n"
            "override that warning; download a newly built package.\n\n"
            "Verify the extracted app: codesign --verify --deep --strict trepdf.app\n"
            "https://support.apple.com/en-us/102445\n", encoding="utf-8")
    else:
        smoke_test(executable, args.platform)
    (stage / "VERSION").write_text(args.version + "\n", encoding="utf-8")
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
    if args.platform == "macos-arm64":
        metadata["macos_signing"] = "ad-hoc; not notarized"
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

    args.output.mkdir(parents=True, exist_ok=True)
    archive_base = args.output.resolve() / f"TrePDF-{args.version}-{package}"
    if args.platform == "macos-arm64":
        archive = Path(str(archive_base) + ".zip")
        # Preserve .app permissions and framework symlinks inside the ZIP.
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
                        str(stage), str(archive)], check=True)
        # Test what a recipient extracts, including the resource seal and symlinks.
        with tempfile.TemporaryDirectory(prefix="trepdf-archive-check-") as temporary:
            subprocess.run(["ditto", "-x", "-k", str(archive), temporary], check=True)
            restored_app = Path(temporary) / stage.name / "trepdf.app"
            verify_macos_bundle(restored_app)
            smoke_test(restored_app / "Contents/MacOS/trepdf", args.platform)
    else:
        archive = Path(shutil.make_archive(str(archive_base),
                       "zip" if args.platform == "windows-x64" else "gztar",
                       root_dir=stage.parent, base_dir=stage.name))
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    checksum = f"{digest.hexdigest()}  {archive.name}\n"
    Path(str(archive) + ".sha256").write_text(checksum, encoding="utf-8")
    print(checksum, end="", flush=True)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(f"### TrePDF {args.version} — {package}\n\n"
                         f"Package: `{archive.name}`\n\n"
                         "Release build, runtime deployment and startup smoke check passed.\n")


if __name__ == "__main__":
    main()
