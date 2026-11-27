"""Exercise the same native dependency transfer used between macOS runners."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macos_universal as universal


class UniversalDependenciesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def transfer(self, platform, *, compile_libraries=False):
        native = self.root / platform
        native.mkdir()
        versions = dict.fromkeys(("pdfium", "qpdf", "leptonica", "tesseract", "tessdata_fast"), "1")
        if platform == "macos-x64":
            versions["libjpeg-turbo"] = "1"
        files = [*universal.REQUIRED_HEADERS, "qpdf/include/CMakeLists.txt", "downloads/unused",
                 *[f"licenses/{name}/LICENSE" for name in versions],
                 *[f"ocr/share/tessdata/{name}" for name in universal.MODELS],
                 "ocr/licenses/tesseract.txt", "ocr/licenses/leptonica.txt", "ocr/licenses/tessdata.txt"]
        for relative in files:
            path = native / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(relative, encoding="utf-8")
        if compile_libraries:
            arch = universal.PLATFORMS[platform]
            source = native / "sample.c"
            source.write_text("int sample(void) { return 42; }\n")
            subprocess.run(["clang", "-target", f"{arch}-apple-macos13", "-c", source,
                            "-o", native / "sample.o"], check=True)
        for library in universal.LIBRARIES:
            path = native / library
            path.parent.mkdir(parents=True, exist_ok=True)
            if not compile_libraries:
                path.write_bytes(b"fixture")
            elif path.suffix == ".a":
                subprocess.run(["ar", "rcs", path, native / "sample.o"], check=True)
            else:
                subprocess.run(["clang", "-target", f"{arch}-apple-macos13", "-dynamiclib",
                                native / "sample.o", "-o", path], check=True)
        if os.name != "nt":
            qpdf = native / "qpdf/lib/libqpdf.dylib"
            qpdf.rename(qpdf.with_name("libqpdf.30.dylib"))
            qpdf.symlink_to("libqpdf.30.dylib")
        (native / "manifest.json").write_text(json.dumps({
            "platform": platform, "macos_deployment_target": "13.0", "versions": versions,
            "roots": {key: str(native / value) for key, value in universal.ROOTS.items()},
        }))
        archive = self.root / f"{platform}.tar.gz"
        universal.pack(native, archive)
        restored = self.root / f"restored-{platform}"
        with tarfile.open(archive) as stream:
            self.assertNotIn("downloads/unused", stream.getnames())
            self.assertNotIn("qpdf/include/CMakeLists.txt", stream.getnames())
            self.assertTrue(stream.getmember("qpdf/lib/libqpdf.dylib").isreg())
            stream.extractall(restored, filter="data")
        return restored

    def test_incompatible_models_stop_merge_before_exporting_roots(self):
        arm, intel = (self.transfer(name) for name in universal.PLATFORMS)
        (intel / "ocr/share/tessdata/eng.traineddata").write_bytes(b"different model")
        output = self.root / "universal"
        env_file = self.root / "env"
        with patch.dict(os.environ, GITHUB_ENV=str(env_file)):
            with self.assertRaisesRegex(RuntimeError, "eng.traineddata"):
                universal.merge(arm, intel, output)
        self.assertFalse(output.exists())
        self.assertFalse(env_file.exists())

    @unittest.skipUnless(sys.platform == "darwin", "requires Apple clang and lipo")
    def test_native_archives_produce_relocatable_universal_dependencies(self):
        arm, intel = (self.transfer(name, compile_libraries=True) for name in universal.PLATFORMS)
        output, env_file = self.root / "merged", self.root / "env"
        with patch.dict(os.environ, GITHUB_ENV=str(env_file)):
            universal.merge(arm, intel, output)
        manifest = json.loads((output / "manifest.json").read_text())
        self.assertEqual(manifest["platform"], "macos-universal")
        for variable, root in manifest["roots"].items():
            self.assertTrue(Path(root).is_dir())
            self.assertIn(f"{variable}={root}", env_file.read_text().splitlines())
        for name in universal.LIBRARIES:
            slices = subprocess.check_output(["lipo", "-archs", output / name], text=True).split()
            self.assertEqual(set(slices), {"arm64", "x86_64"})
        self.assertTrue((output / "ocr/licenses/tesseract.txt").is_file())
        self.assertTrue((output / "licenses/libjpeg-turbo/LICENSE").is_file())
