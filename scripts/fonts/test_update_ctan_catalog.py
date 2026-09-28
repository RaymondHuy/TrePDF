"""Offline regression checks for update_ctan_catalog.py; requires fontTools."""

import gzip
import io
from pathlib import Path
import unittest

from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables.DefaultTable import DefaultTable

import update_ctan_catalog as catalog


REPOSITORY = Path(__file__).resolve().parents[2]
TERMES = REPOSITORY / "tests/fixtures/fonts/tex-gyre/texgyretermes-regular.otf"


class CatalogFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = TERMES.read_bytes()
        cls.reviewed = catalog.reviewed_digests(REPOSITORY / "src/ReviewedCtanFonts.h")

    def changed_font(self, change):
        with TTFont(io.BytesIO(self.source), recalcTimestamp=False) as font:
            font["OS/2"].fsType = 0
            change(font)
            output = io.BytesIO()
            font.save(output)
            return output.getvalue()

    def test_reviewed_unchanged_font_remains_downloadable(self):
        face = catalog.inspect_font(self.source, self.reviewed)
        self.assertEqual(face["family"], "TeX Gyre Termes")
        self.assertEqual(face["style"], "regular")
        self.assertTrue(face["reviewedEmbeddingException"])
        self.assertTrue(face["latin"])
        self.assertTrue(face["vietnamese"])

    def test_embedding_exception_requires_exact_bytes(self):
        modified = bytearray(self.source)
        modified[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "embedding-restricted"):
            catalog.inspect_font(bytes(modified), self.reviewed)

    def test_installable_font_needs_no_exception(self):
        face = catalog.inspect_font(self.changed_font(lambda font: None), set())
        self.assertEqual(face["family"], "TeX Gyre Termes")
        self.assertFalse(face["reviewedEmbeddingException"])

    def test_embedding_restrictions_are_not_inferred_from_family(self):
        for fs_type in (0x0002, 0x0004, 0x000C, 0x0100, 0x0200):
            with self.subTest(fs_type=fs_type):
                data = self.changed_font(lambda font: setattr(font["OS/2"], "fsType", fs_type))
                with self.assertRaisesRegex(ValueError, "embedding-restricted"):
                    catalog.inspect_font(data, self.reviewed)

    def test_variable_face_is_not_advertised_as_static(self):
        def add_variable_table(font):
            table = DefaultTable("fvar")
            table.data = bytes(16)
            font["fvar"] = table

        with self.assertRaisesRegex(ValueError, "variable-or-color-font"):
            catalog.inspect_font(self.changed_font(add_variable_table), set())

    def test_missing_ascii_rejected_before_publishing(self):
        def remove_space(font):
            for table in font["cmap"].tables:
                if table.isUnicode():
                    table.cmap.pop(32, None)

        with self.assertRaises(ValueError):
            catalog.inspect_font(self.changed_font(remove_space), set())

    def test_inconsistent_bold_flag_is_rejected(self):
        def change_flag(font):
            font["OS/2"].fsSelection = (font["OS/2"].fsSelection | 0x20) & ~0x40
            font["head"].macStyle |= 1

        with self.assertRaisesRegex(ValueError, "inconsistent-weight-style"):
            catalog.inspect_font(self.changed_font(change_flag), set())

    def test_subset_name_not_promoted_to_complete_download(self):
        def subset_name(font):
            for record in font["name"].names:
                if record.nameID in (1, 16):
                    record.string = "ABCDEF+TrePDFSubset".encode(record.getEncoding())

        with self.assertRaisesRegex(ValueError, "invalid-family"):
            catalog.inspect_font(self.changed_font(subset_name), set())

    def test_face_name_keeps_distinct_condensed_family(self):
        font = TTFont()
        font["name"] = newTable("name")
        font["name"].names = []
        for name_id, text in ((1, "Example Condensed"), (2, "Regular"),
                              (16, "Example"), (17, "Condensed")):
            font["name"].setName(text, name_id, 3, 1, 0x409)
        self.assertEqual(catalog.font_name(font), ("Example Condensed", "Regular"))

    def test_platform_dependent_extended_family_is_not_published(self):
        def extended_family(font):
            values = {1: "Example Condensed", 2: "Regular", 16: "Example", 17: "Condensed"}
            for record in font["name"].names:
                if record.nameID in values:
                    record.string = values[record.nameID].encode(record.getEncoding())
            for name_id, text in values.items():
                font["name"].setName(text, name_id, 3, 1, 0x409)

        with self.assertRaisesRegex(ValueError, "platform-dependent-extended-family"):
            catalog.inspect_font(self.changed_font(extended_family), set())

    def test_equivalent_typographic_family_is_preferred(self):
        font = TTFont()
        font["name"] = newTable("name")
        font["name"].names = []
        for name_id, text in ((1, "ExampleFont"), (2, "Regular"),
                              (16, "Example Font"), (17, "Regular")):
            font["name"].setName(text, name_id, 3, 1, 0x409)
        self.assertEqual(catalog.font_name(font), ("Example Font", "Regular"))


class CatalogInventoryTests(unittest.TestCase):
    def test_inventory_preserves_exact_case_and_ignores_other_files(self):
        source = ("2026-09-28 | 12 | fonts/example/Zed.OTF\n"
                  "2026-09-28 | 13 | macros/example/Alpha.ttf\n"
                  "2026-09-28 | 14 | fonts/example/font.pfb\n")
        result = catalog.inventory(gzip.compress(source.encode()))
        self.assertEqual([item["path"] for item in result],
                         ["fonts/example/Zed.OTF", "macros/example/Alpha.ttf"])

    def test_inventory_rejects_unsafe_paths_before_downloading(self):
        paths = ("/absolute/font.ttf", "../escape.ttf", "fonts/../escape.ttf",
                 "fonts/%2e%2e/escape.ttf", "fonts/./font.ttf", "fonts//font.ttf",
                 "fonts/back\\slash.ttf", "fonts/has space.ttf")
        source = "".join(f"2026-09-28 | 12 | {path}\n" for path in paths)
        self.assertEqual(catalog.inventory(gzip.compress(source.encode())), [])


if __name__ == "__main__":
    unittest.main()
