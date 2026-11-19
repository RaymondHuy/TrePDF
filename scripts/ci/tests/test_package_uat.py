"""Packaging checks with a synthetic deployed app; no native executable is run."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
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
        for platform in ("macos-arm64", "macos-x64", "linux-x64"):
            with self.subTest(platform=platform):
                arguments = self.arguments.copy()
                arguments[2] = platform
                with self.assertRaises(SystemExit) as error:
                    self.run_package("--no-archive", arguments=arguments)
                self.assertEqual(error.exception.code, 2)
        self.assertFalse(self.summary.exists())
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
