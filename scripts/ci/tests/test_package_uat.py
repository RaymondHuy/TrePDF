"""Packaging checks with a synthetic deployed app; no native executable is run."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import call, patch
import zipfile


SCRIPT = Path(__file__).resolve().parents[1] / "package_uat.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("package_uat", SCRIPT)
package_uat = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package_uat)


class WindowsPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="trepdf-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.stage = self.root / "stage/TrePDF-0.0.1-windows-x64"
        self.output = self.root / "output"
        self.dependencies = self.root / "dependencies"
        self.summary = self.root / "summary.md"
        self.exe = self.stage / "bin/trepdf.exe"
        self.exe.parent.mkdir(parents=True)
        self.exe.write_bytes(b"synthetic Windows executable")
        (self.exe.parent / "Qt6Core.dll").write_bytes(b"synthetic Qt runtime")
        ocr = self.dependencies / "ocr"
        for directory in (ocr / "share/tessdata", self.exe.parent / "tessdata"):
            directory.mkdir(parents=True)
            for name in ("eng.traineddata", "vie.traineddata", "pdf.ttf"):
                (directory / name).write_bytes(name.encode())
        notices = self.dependencies / "licenses/pdfium"
        notices.mkdir(parents=True)
        (notices / "LICENSE").write_text("fixture dependency license", encoding="utf-8")
        (self.dependencies / "manifest.json").write_text(json.dumps({
            "platform": "windows-x64",
            "roots": {"OCR_ROOT": str(ocr)},
            "versions": {"pdfium": "fixture"},
        }), encoding="utf-8")
        self.arguments = [str(SCRIPT), "--platform", "windows-x64", "--version", "0.0.1",
                          "--stage", str(self.stage), "--dependencies", str(self.dependencies),
                          "--output", str(self.output)]
        self.environment = patch.dict(os.environ, {
            "GITHUB_STEP_SUMMARY": str(self.summary),
            "GITHUB_SHA": "fixture-commit",
            "QT_VERSION": "fixture-qt",
        }, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def run_package(self, *options, arguments=None):
        argv = (self.arguments if arguments is None else arguments) + list(options)
        native_open = Path.open

        def windows_text_open(path, mode="r", buffering=-1, encoding=None, errors=None, newline=None):
            # Exercise Windows' default text translation even on macOS/Linux.
            # An explicit newline argument must still be respected.
            if "b" not in mode and newline is None and any(flag in mode for flag in "wax"):
                newline = "\r\n"
            return native_open(path, mode, buffering, encoding, errors, newline)

        with patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()), \
                patch.object(Path, "open", windows_text_open):
            package_uat.main()

    def test_folder_retains_runtime_metadata_and_licenses_without_archiving(self):
        with patch.object(package_uat, "smoke_test") as smoke, \
                patch.object(package_uat.shutil, "make_archive") as archive:
            self.run_package("--no-archive")
        smoke.assert_called_once_with(self.exe, "windows-x64")
        archive.assert_not_called()
        self.assertEqual(self.exe.read_bytes(), b"synthetic Windows executable")
        self.assertTrue((self.exe.parent / "Qt6Core.dll").is_file())
        self.assertTrue((self.exe.parent / "tessdata/vie.traineddata").is_file())
        self.assertEqual((self.stage / "VERSION").read_bytes(), b"0.0.1\n")
        metadata = json.loads((self.stage / "build-info.json").read_text())
        self.assertEqual(metadata["version"], "0.0.1")
        self.assertEqual(metadata["platform"], "windows-x64")
        self.assertEqual(metadata["commit"], "fixture-commit")
        self.assertEqual(metadata["dependencies"], {"pdfium": "fixture"})
        self.assertEqual((self.stage / "licenses/pdfium/LICENSE").read_text(),
                         "fixture dependency license")
        self.assertTrue(any((self.stage / "licenses/libxlsxwriter").iterdir()))
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.rglob("*.zip")), [])
        self.assertEqual(list(self.root.rglob("*.sha256")), [])
        self.assertIn("bin/trepdf.exe", self.summary.read_text())

    def test_startup_failure_still_stops_publication(self):
        with patch.object(package_uat, "smoke_test", side_effect=RuntimeError("startup failed")), \
                patch.object(package_uat.shutil, "make_archive") as archive:
            with self.assertRaisesRegex(RuntimeError, "startup failed"):
                self.run_package("--no-archive")
        archive.assert_not_called()
        self.assertFalse((self.stage / "VERSION").exists())
        self.assertFalse(self.summary.exists())
        self.assertFalse(self.output.exists())

    def test_changed_ocr_data_still_fails_validation(self):
        (self.exe.parent / "tessdata/eng.traineddata").write_bytes(b"damaged")
        with patch.object(package_uat, "smoke_test") as smoke:
            with self.assertRaisesRegex(RuntimeError, "Missing or changed bundled OCR data"):
                self.run_package("--no-archive")
        smoke.assert_not_called()
        self.assertFalse(self.summary.exists())
        self.assertFalse(self.output.exists())

    def test_archive_mode_remains_available_for_existing_callers(self):
        with patch.object(package_uat, "smoke_test"):
            self.run_package()
        archive = self.output / "TrePDF-0.0.1-windows-x64.zip"
        with zipfile.ZipFile(archive) as result:
            self.assertEqual(result.read(f"{self.stage.name}/VERSION"), b"0.0.1\n")
            self.assertIn(f"{self.stage.name}/bin/trepdf.exe", result.namelist())
            self.assertIn(f"{self.stage.name}/licenses/pdfium/LICENSE", result.namelist())
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(Path(str(archive) + ".sha256").read_text(),
                         f"{digest}  {archive.name}\n")

    def test_no_archive_rejects_non_windows_platforms(self):
        for platform in ("macos-arm64", "macos-x64", "macos-universal", "linux-x64"):
            with self.subTest(platform=platform):
                arguments = self.arguments.copy()
                arguments[2] = platform
                with self.assertRaises(SystemExit) as error:
                    self.run_package("--no-archive", arguments=arguments)
                self.assertEqual(error.exception.code, 2)
        self.assertFalse(self.summary.exists())
        self.assertFalse(self.output.exists())


class MacOSPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="trepdf-macos-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.stage = self.root / "TrePDF-1.2.3-macos-universal-qt6.11"
        self.app = self.stage / "trepdf.app"
        self.output = self.root / "output"
        self.dependencies = self.root / "dependencies"
        executable = self.app / "Contents/MacOS/trepdf"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"synthetic universal executable")
        (self.app / "Contents/Info.plist").write_bytes(
            plistlib.dumps({"LSMinimumSystemVersion": "13.0", "CFBundleShortVersionString": "1.2.3"}))
        self.ocr = self.dependencies / "ocr"
        for directory in (self.ocr / "share/tessdata", self.app / "Contents/Resources/tessdata"):
            directory.mkdir(parents=True)
            for name in ("eng.traineddata", "vie.traineddata", "pdf.ttf"):
                (directory / name).write_bytes(name.encode())
        (self.dependencies / "licenses").mkdir()
        self.arguments = [str(SCRIPT), "--platform", "macos-universal", "--version", "1.2.3",
                          "--variant", "qt6.11", "--stage", str(self.stage),
                          "--dependencies", str(self.dependencies), "--output", str(self.output)]
        self.set_platform("macos-universal")
        self.environment = patch.dict(os.environ, {
            "QT_VERSION": "6.11.0", "MACOS_DEPLOYMENT_TARGET": "13.0",
        }, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def set_platform(self, platform):
        self.arguments[2] = platform
        (self.dependencies / "manifest.json").write_text(json.dumps({
            "platform": platform,
            "roots": {"OCR_ROOT": str(self.ocr)},
            "versions": {"pdfium": "fixture"},
            "macos_deployment_target": "13.0",
        }), encoding="utf-8")

    def fake_ditto(self, command, *, check):
        self.assertTrue(check)
        if command[:2] == ["ditto", "-c"]:
            source, destination = map(Path, command[-2:])
            with zipfile.ZipFile(destination, "w") as archive:
                for path in source.rglob("*"):
                    if path.is_file():
                        archive.write(path, path.relative_to(source.parent))
        elif command[:2] == ["ditto", "-x"]:
            with zipfile.ZipFile(command[-2]) as archive:
                archive.extractall(command[-1])
        else:
            self.fail(f"Unexpected native command: {command}")

    def run_package(self, *options):
        with patch.object(sys, "argv", self.arguments + list(options)), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            package_uat.main()

    def test_rejects_stale_app_version_before_packaging(self):
        self.arguments[self.arguments.index("--version") + 1] = "1.2.4"
        with self.assertRaisesRegex(RuntimeError, "Built app version '1.2.3'.*'1.2.4'"):
            self.run_package("--macos-dmg")
        self.assertFalse(self.output.exists())

    def test_universal_verifies_both_slices_and_packages_dmg_with_metadata(self):
        def fake_dmg(stage, destination, version):
            self.assertEqual((stage, version), (self.stage, "1.2.3"))
            destination.write_bytes(b"synthetic disk image")

        with patch.object(package_uat, "verify_tree") as verify, \
                patch.object(package_uat, "sign_macos_bundle") as sign, \
                patch.object(package_uat, "verify_macos_bundle") as verify_signature, \
                patch.object(package_uat, "smoke_test") as smoke, \
                patch.object(package_uat.subprocess, "run", side_effect=self.fake_ditto), \
                patch.object(package_uat, "create_macos_dmg", side_effect=fake_dmg) as dmg:
            self.run_package("--macos-dmg")
        verify.assert_has_calls([call(self.app, "arm64", "13.0"),
                                 call(self.app, "x86_64", "13.0")])
        self.assertEqual(verify.call_count, 2)
        sign.assert_called_once_with(self.app)
        verify_signature.assert_called_once()
        smoke.assert_called_once()
        self.assertEqual(smoke.call_args.args[1], "macos-universal")
        self.assertEqual(smoke.call_args.args[0].name, "trepdf")
        self.assertIn(self.stage.name, str(smoke.call_args.args[0]))
        dmg.assert_called_once()
        metadata = json.loads((self.stage / "build-info.json").read_text())
        self.assertEqual(metadata["platform"], "macos-universal")
        self.assertEqual(metadata["macos_architectures"], ["arm64", "x86_64"])
        self.assertEqual(metadata["macos_deployment_target"], "13.0")
        self.assertEqual(metadata["qt"], "6.11.0")
        self.assertIn("Intel and Apple Silicon", (self.stage / "START-HERE.txt").read_text())
        for suffix in ("zip", "dmg"):
            archive = self.output / f"TrePDF-1.2.3-macos-universal-qt6.11.{suffix}"
            self.assertTrue(archive.is_file())
            checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
            self.assertEqual(Path(str(archive) + ".sha256").read_text(),
                             f"{checksum}  {archive.name}\n")

    def test_missing_intel_slice_stops_signing_and_archiving(self):
        with patch.object(package_uat, "verify_tree", side_effect=[
                None, RuntimeError("missing x86_64 slice")]) as verify, \
                patch.object(package_uat, "sign_macos_bundle") as sign, \
                patch.object(package_uat.subprocess, "run") as native, \
                patch.object(package_uat, "create_macos_dmg") as dmg:
            with self.assertRaisesRegex(RuntimeError, "missing x86_64"):
                self.run_package("--macos-dmg")
        self.assertEqual(verify.call_args.args[1], "x86_64")
        sign.assert_not_called()
        native.assert_not_called()
        dmg.assert_not_called()
        self.assertFalse(self.output.exists())
        self.assertFalse((self.stage / "build-info.json").exists())

    def test_notarized_downloads_include_stapled_app_and_final_dmg_checksum(self):
        ticket = self.app / "Contents/ticket-fixture"
        def staple(submission, target, auth):
            self.assertTrue(submission.is_file())
            if target == self.app:
                ticket.write_bytes(b"app ticket")
            else:
                self.assertEqual(target.read_bytes(), b"signed dmg")
                target.write_bytes(b"stapled dmg")

        def build_image(stage, destination, version, *, notarized_app):
            self.assertTrue(notarized_app)
            self.assertTrue(ticket.exists())
            destination.write_bytes(b"disk image")

        def native(command, *, check):
            if command[0] == "ditto":
                self.fake_ditto(command, check=check)
            elif command[:2] == ["codesign", "--force"]:
                Path(command[-1]).write_bytes(b"signed dmg")
            elif command[:3] == ["xcrun", "stapler", "validate"]:
                self.assertEqual((Path(command[-1]) / "Contents/ticket-fixture").read_bytes(), b"app ticket")
            else:
                self.assertIn(command[0], ("codesign", "spctl"))

        with patch.object(package_uat, "take_credentials", return_value={}), \
                patch.object(package_uat, "signing_assets", return_value=contextlib.nullcontext((["--sign", "id"], []))), \
                patch.object(package_uat, "verify_tree"), patch.object(package_uat, "sign_macos_bundle") as sign, \
                patch.object(package_uat, "notarize", side_effect=staple) as submit, \
                patch.object(package_uat, "create_macos_dmg", side_effect=build_image), \
                patch.object(package_uat, "verify_macos_bundle"), patch.object(package_uat, "smoke_test"), \
                patch.object(package_uat.subprocess, "run", side_effect=native):
            self.run_package("--macos-notarize", "--macos-dmg")
        sign.assert_called_once_with(self.app, ["--sign", "id"])
        self.assertEqual(submit.call_count, 2)
        checksum = next(self.output.glob("*.dmg.sha256"))
        self.assertEqual(checksum.read_text().split()[0], hashlib.sha256(b"stapled dmg").hexdigest())
        self.assertNotIn("Open Anyway", (self.stage / "START-HERE.txt").read_text())

    def test_single_architecture_packages_keep_their_existing_checks(self):
        for platform, architecture in (("macos-arm64", "arm64"), ("macos-x64", "x86_64")):
            with self.subTest(platform=platform):
                self.set_platform(platform)
                with patch.object(package_uat, "verify_tree") as verify, \
                        patch.object(package_uat, "sign_macos_bundle"), \
                        patch.object(package_uat, "verify_macos_bundle"), \
                        patch.object(package_uat, "smoke_test"), \
                        patch.object(package_uat.subprocess, "run", side_effect=self.fake_ditto):
                    self.run_package()
                verify.assert_called_once_with(self.app, architecture, "13.0")
                metadata = json.loads((self.stage / "build-info.json").read_text())
                self.assertEqual(metadata["macos_architectures"], [architecture])

    def test_universal_smoke_uses_cocoa_without_runner_library_paths(self):
        with patch.object(package_uat.subprocess, "Popen") as launch, \
                patch.dict(os.environ, {"QT_PLUGIN_PATH": "/runner/qt/plugins",
                                        "DYLD_LIBRARY_PATH": "/runner/dependencies"}), \
                contextlib.redirect_stdout(io.StringIO()):
            process = launch.return_value
            process.wait.side_effect = [subprocess.TimeoutExpired("trepdf", 12), 0]
            process.poll.return_value = None
            package_uat.smoke_test(self.app / "Contents/MacOS/trepdf", "macos-universal")
        environment = launch.call_args.kwargs["env"]
        self.assertEqual(environment["QT_QPA_PLATFORM"], "cocoa")
        self.assertNotIn("QT_PLUGIN_PATH", environment)
        self.assertNotIn("DYLD_LIBRARY_PATH", environment)
        process.terminate.assert_called_once()


if __name__ == "__main__":
    unittest.main()
