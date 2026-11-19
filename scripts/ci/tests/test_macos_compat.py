"""Check old-OS dependency selection and fail-closed package validation."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macos_compat
import prepare_dependencies as dependencies


def commands(minimum="12.0", platform="1"):
    return (f"Load command 0\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n"
            f" platform {platform}\n    minos {minimum}\n      sdk 15.0\n")


class MacOSCompatibilityTests(unittest.TestCase):
    def test_load_command_formats(self):
        for name in ("1", "MACOS", "macos"):
            self.assertEqual(macos_compat.minimum_macos(commands(platform=name)), "12.0")
        self.assertEqual(macos_compat.minimum_macos(
            "Load command 0\n cmd LC_VERSION_MIN_MACOSX\n version 10.15\n sdk 12.0\n"), "10.15")
        self.assertLess(macos_compat.version_tuple("12.9"), macos_compat.version_tuple("12.10"))

    def test_missing_or_non_macos_target_is_rejected(self):
        for text in ("", commands(platform="2"), commands().replace("minos 12.0", "unknown")):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                macos_compat.minimum_macos(text)

    def test_newer_dependency_is_rejected(self):
        with patch.object(macos_compat.subprocess, "run"), \
                patch.object(macos_compat.subprocess, "check_output", return_value=commands("13.0")):
            with self.assertRaisesRegex(RuntimeError, "above package target 12.0"):
                macos_compat.verify_binary(Path("libpdfium.dylib"), "arm64", "12.0")

    def test_wrong_architecture_is_rejected(self):
        with patch.object(macos_compat.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "lipo")), \
                patch.object(macos_compat.subprocess, "check_output") as inspect:
            with self.assertRaises(subprocess.CalledProcessError):
                macos_compat.verify_binary(Path("lib.dylib"), "x86_64", "12.0")
            inspect.assert_not_called()

    @unittest.skipIf(os.name == "nt", "Symlink fixture requires Unix permissions")
    def test_bundle_checks_nested_code_once_and_skips_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / "Frameworks/libpdfium.dylib"
            library.parent.mkdir()
            library.write_bytes(b"\xcf\xfa\xed\xfe" + b"fixture")
            (root / "LICENSE").write_text("plain text")
            (root / "alias.dylib").symlink_to(library)
            with patch.object(macos_compat, "verify_binary") as verify:
                macos_compat.verify_tree(root, "arm64", "12.0")
                verify.assert_called_once_with(library, "arm64", "12.0")

    def test_pdfium_selection_preserves_current_platforms(self):
        for platform in ("macos-arm64", "macos-x64"):
            for minimum, expected in (("12.0", "151.0.7920.0"), ("13.0", "157.0.8100.0"), ("14.4", "157.0.8100.0")):
                with self.subTest(platform=platform, minimum=minimum), \
                        patch.dict(os.environ, {"MACOS_DEPLOYMENT_TARGET": minimum}):
                    version, base, name, digest = dependencies.pdfium_package(platform)
                    self.assertEqual(version, expected)
                    self.assertIn("mac-" + ("arm64" if platform.endswith("arm64") else "x64"), name)
                    self.assertEqual(len(digest), 64)
                    self.assertIn(version.split(".")[2], base)
        for platform in ("windows-x64", "linux-x64"):
            self.assertEqual(dependencies.pdfium_package(platform)[0], dependencies.PDFIUM_VERSION)

    def test_unsupported_older_target_is_rejected(self):
        with patch.dict(os.environ, {"MACOS_DEPLOYMENT_TARGET": "11.0"}):
            with self.assertRaisesRegex(RuntimeError, "require macOS 12"):
                dependencies.pdfium_package("macos-arm64")

    def test_macos12_rebuilds_qpdf_for_both_architectures(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for platform in ("macos-arm64", "macos-x64"):
                with self.subTest(platform=platform), \
                        patch.dict(os.environ, {"MACOS_DEPLOYMENT_TARGET": "12.0"}), \
                        patch.object(dependencies, "checked_in_qpdf") as bundled, \
                        patch.object(dependencies, "download", return_value=root / "source.tar.gz"), \
                        patch.object(dependencies, "extract"), \
                        patch.object(dependencies, "copy_notices"), \
                        patch.object(dependencies, "macos_qpdf_options", return_value=["-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0"]) as options, \
                        patch.object(dependencies, "run") as run:
                    dependencies.prepare_qpdf(platform, root, root / "repo")
                    bundled.assert_not_called()
                    options.assert_called_once_with(root, platform)
                    self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0", run.call_args_list[0].args)
                    run.assert_any_call("install_name_tool", "-id", "@rpath/libqpdf.dylib", root / "qpdf/lib/libqpdf.dylib")

    def test_modern_arm64_keeps_bundled_qpdf(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with patch.dict(os.environ, {"MACOS_DEPLOYMENT_TARGET": "13.0"}), \
                    patch.object(dependencies, "checked_in_qpdf", return_value=root / "bundled") as bundled, \
                    patch.object(dependencies.shutil, "copytree"), \
                    patch.object(dependencies, "copy_notices"), \
                    patch.object(dependencies, "run") as build:
                dependencies.prepare_qpdf("macos-arm64", root, root / "repo")
                bundled.assert_called_once()
                build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
