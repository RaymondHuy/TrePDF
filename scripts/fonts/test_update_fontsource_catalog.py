#!/usr/bin/env python3
"""Regression checks for the offline, commit-pinned FontSource importer."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from update_fontsource_catalog import REPOSITORY, build_catalog, decode_json


class FontSourceCatalogTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="trepdf-fontsource-test-")
        self.addCleanup(self.temporary.cleanup)
        self.repository = Path(self.temporary.name) / "repository"
        self.repository.mkdir()
        self.git("init", "--quiet")
        self.directory = self.repository / "fonts/liberation-sans/2.1.5"
        self.directory.mkdir(parents=True)
        self.font = self.directory / "LiberationSans-Regular.ttf"
        fixture = Path(__file__).resolve().parents[2] / "tests/fixtures/fonts/liberation/LiberationSans-Regular.ttf"
        data = fixture.read_bytes()
        self.font.write_bytes(data)
        (self.directory / "LICENSE").write_text("Test license notice\n", encoding="utf-8")
        (self.directory / "AUTHORS").write_text("Test author notice\n", encoding="utf-8")
        self.manifest = {
            "schemaVersion": 1, "family": "Liberation Sans", "familyKey": "liberation-sans",
            "version": "2.1.5", "format": "static-truetype",
            "license": {"name": "SIL Open Font License 1.1", "file": "LICENSE"},
            "authorsFile": "AUTHORS", "coverage": {"ascii": True, "vietnamese": True},
            "faces": {"regular": {"file": self.font.name, "weight": 400, "style": "normal",
                                  "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}},
        }
        self.catalog = {
            "schemaVersion": 1, "repository": REPOSITORY,
            "families": [{"family": "Liberation Sans", "familyKey": "liberation-sans",
                          "versions": [{"version": "2.1.5", "manifest": "fonts/liberation-sans/2.1.5/manifest.json"}]}],
        }
        self.write_metadata()
        self.revision = self.commit()

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.repository), *arguments], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

    def commit(self):
        self.git("add", ".")
        self.git("-c", "user.name=FontSource Test", "-c", "user.email=font-test@example.invalid",
                 "commit", "--quiet", "-m", "fixture")
        return self.git("rev-parse", "HEAD").decode().strip()

    def write_metadata(self):
        (self.repository / "catalog.json").write_text(json.dumps(self.catalog), encoding="utf-8")
        (self.directory / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_generates_deterministic_snapshot(self):
        data = build_catalog(self.repository, self.revision)
        self.assertEqual(data, build_catalog(self.repository, self.revision))
        catalog = json.loads(data)
        self.assertEqual(catalog["revision"], self.revision)
        family = catalog["families"][0]
        self.assertEqual(family["name"], "Liberation Sans")
        self.assertEqual(family["source"], REPOSITORY + "/tree/" + self.revision + "/fonts/liberation-sans/2.1.5")
        self.assertEqual(family["faces"]["regular"]["sha256"], self.manifest["faces"]["regular"]["sha256"])

    def test_ignores_dirty_working_tree(self):
        expected = build_catalog(self.repository, self.revision)
        self.font.write_bytes(b"not a font")
        (self.repository / "catalog.json").write_text("bad JSON", encoding="utf-8")
        self.assertEqual(build_catalog(self.repository, self.revision), expected)

    def test_rejects_mutable_or_abbreviated_revision(self):
        for revision in ("HEAD", "main", self.revision[:8], self.revision + "\n"):
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                build_catalog(self.repository, revision)

    def test_rejects_committed_hash_mismatch(self):
        data = bytearray(self.font.read_bytes())
        data[-1] ^= 1
        self.font.write_bytes(data)
        revision = self.commit()
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            build_catalog(self.repository, revision)

    def test_rejects_manifest_traversal(self):
        self.catalog["families"][0]["versions"][0]["manifest"] = "fonts/../manifest.json"
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "manifest path"):
            build_catalog(self.repository, self.commit())

    def test_rejects_symlink_notice(self):
        notice = self.directory / "LICENSE"
        notice.unlink()
        notice.symlink_to("AUTHORS")
        with self.assertRaisesRegex(ValueError, "regular committed file"):
            build_catalog(self.repository, self.commit())

    def test_rejects_missing_notice(self):
        (self.directory / "AUTHORS").unlink()
        with self.assertRaisesRegex(ValueError, "regular committed file"):
            build_catalog(self.repository, self.commit())

    def test_rejects_duplicate_family(self):
        self.catalog["families"].append(self.catalog["families"][0])
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "duplicate family"):
            build_catalog(self.repository, self.commit())

    def test_rejects_style_mismatch(self):
        face = self.manifest["faces"].pop("regular")
        face["weight"] = 700
        self.manifest["faces"]["bold"] = face
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "font style differs"):
            build_catalog(self.repository, self.commit())

    def test_rejects_lfs_pointer_even_when_hash_matches(self):
        data = b"version https://git-lfs.github.com/spec/v1\noid sha256:" + b"0" * 64 + b"\nsize 123\n"
        self.font.write_bytes(data)
        self.manifest["faces"]["regular"].update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "unsupported font container"):
            build_catalog(self.repository, self.commit())

    def test_rejects_duplicate_json_keys(self):
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            decode_json(b'{"schemaVersion": 1, "schemaVersion": 2}')

    def test_failed_update_preserves_existing_output(self):
        output = Path(self.temporary.name) / "existing-catalog.json"
        output.write_bytes(b"previous snapshot\n")
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("update_fontsource_catalog.py")),
                                 "--repository", str(self.repository), "--revision", "main",
                                 "--output", str(output)], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b"previous snapshot\n")


if __name__ == "__main__":
    unittest.main()
