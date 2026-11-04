#!/usr/bin/env python3
"""Build the reviewed FontSource snapshot from an immutable local Git commit.

Only Python's standard library is needed. The final version listed for each
family in catalog.json is selected. Read committed blobs, never working-tree
files; verify provenance files, face metadata, static sfnt structure and hashes.
Run the Qt external-face regression before accepting a new snapshot: Qt's
family/style, embedding and glyph validation remains authoritative.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unicodedata


REPOSITORY = "https://github.com/tre-base/FontSource"
MAX_METADATA_BYTES = 2 * 1024 * 1024
MAX_FONT_BYTES = 16 * 1024 * 1024
MAX_FAMILIES = 2048
STYLES = ("regular", "italic", "bold", "bolditalic")
KEY = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,63}")
FILENAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]*")
SHA256 = re.compile(r"[0-9a-f]{64}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def valid_text(value, maximum):
    return (isinstance(value, str) and 0 < len(value.encode("utf-16-le")) // 2 <= maximum
            and value == value.strip()
            and not any(unicodedata.category(char) == "Cc" for char in value))


def no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def decode_json(data):
    result = json.loads(data, object_pairs_hook=no_duplicate_keys)
    require(isinstance(result, dict), "expected a JSON object")
    return result


class Commit:
    def __init__(self, repository, revision):
        require(re.fullmatch(r"[0-9a-f]{40}", revision), "revision must be a full lowercase commit SHA")
        self.repository = repository
        self.revision = revision
        require(self.git("cat-file", "-t", revision).strip() == b"commit", "revision must identify a commit")

    def git(self, *arguments):
        result = subprocess.run(["git", "-C", str(self.repository), *arguments],
                                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        require(result.returncode == 0, result.stderr.decode("utf-8", errors="replace").strip())
        return result.stdout

    def read(self, path, maximum):
        parts = path.split("/")
        require(all(FILENAME.fullmatch(part) and part not in (".", "..") for part in parts),
                "unsafe repository path: " + path)
        entry = self.git("ls-tree", self.revision, "--", path).decode("utf-8").strip()
        require(entry.startswith("100644 blob ") or entry.startswith("100755 blob "),
                "expected a regular committed file: " + path)
        specification = self.revision + ":" + path
        size = int(self.git("cat-file", "-s", specification))
        require(0 < size <= maximum, "invalid committed file size: " + path)
        data = self.git("show", specification)
        require(len(data) == size, "incomplete committed file: " + path)
        return data


def inspect_font(data, family, style):
    require(12 <= len(data) <= MAX_FONT_BYTES, "invalid font size")
    signature, count = struct.unpack_from(">4sH", data)
    require(signature in (b"\x00\x01\x00\x00", b"OTTO"), "unsupported font container")
    directory_end = 12 + count * 16
    require(0 < count <= 256 and directory_end <= len(data), "invalid sfnt directory")
    tables = {}
    for index in range(count):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + index * 16)
        require(tag not in tables and offset >= directory_end and offset + length <= len(data),
                "invalid sfnt table bounds")
        tables[tag] = data[offset:offset + length]
    require({b"head", b"OS/2", b"cmap", b"name", b"maxp", b"hhea", b"hmtx"} <= tables.keys(),
            "missing required font table")
    require(not ({b"fvar", b"CFF2", b"COLR", b"SVG ", b"CBDT", b"sbix", b"EBDT"} & tables.keys()),
            "variable/color/bitmap font is not supported")
    if signature == b"OTTO":
        require(b"CFF " in tables and not ({b"glyf", b"loca"} & tables.keys()), "invalid CFF font")
    else:
        require({b"glyf", b"loca"} <= tables.keys() and b"CFF " not in tables, "invalid TrueType font")
    os2, head = tables[b"OS/2"], tables[b"head"]
    require(len(os2) >= 64 and len(head) >= 54, "incomplete font metrics")
    version = struct.unpack_from(">H", os2, 0)[0]
    weight = struct.unpack_from(">H", os2, 4)[0]
    fs_type = struct.unpack_from(">H", os2, 8)[0]
    permissions = fs_type & 0x000F
    require(not (fs_type & 0x0301) and not (version >= 2 and fs_type & 0xFEF0)
            and (permissions == 0 or (bool(permissions & 8) if version <= 2 else permissions == 8)),
            "font embedding/editing is not permitted")
    selection = struct.unpack_from(">H", os2, 62)[0]
    expected_bold, expected_italic = style in ("bold", "bolditalic"), style in ("italic", "bolditalic")
    require(bool(selection & 0x20) == expected_bold and bool(selection & 1) == expected_italic
            and weight == (700 if expected_bold else 400), "font style differs from manifest")
    name = tables[b"name"]
    require(len(name) >= 6, "missing font names")
    _, name_count, strings = struct.unpack_from(">HHH", name)
    require(6 + name_count * 12 <= len(name) and strings >= 6 + name_count * 12, "invalid name records")
    families = set()
    for index in range(name_count):
        platform, _, _, name_id, length, offset = struct.unpack_from(">HHHHHH", name, 6 + index * 12)
        require(strings + offset + length <= len(name), "invalid font name bounds")
        if platform in (0, 3) and name_id in (1, 16):
            families.add(name[strings + offset:strings + offset + length].decode("utf-16-be").casefold())
    require(family.casefold() in families, "font family differs from manifest")
    return "static-opentype-cff" if signature == b"OTTO" else "static-truetype"


def build_catalog(repository, revision):
    commit = Commit(repository, revision)
    catalog = decode_json(commit.read("catalog.json", MAX_METADATA_BYTES))
    require(type(catalog.get("schemaVersion")) is int and catalog["schemaVersion"] == 1
            and catalog.get("repository") == REPOSITORY, "unsupported source catalogue")
    entries = catalog.get("families")
    require(isinstance(entries, list) and 0 < len(entries) <= MAX_FAMILIES, "invalid family list")
    families, names, keys, paths = [], set(), set(), set()
    for entry in entries:
        require(isinstance(entry, dict), "invalid family record")
        name, key = entry.get("family"), entry.get("familyKey")
        require(valid_text(name, 128) and isinstance(key, str) and len(key) <= 128
                and KEY.fullmatch(key), "invalid family name/key")
        require(name.casefold() not in names and key not in keys, "duplicate family name/key")
        names.add(name.casefold())
        keys.add(key)
        versions = entry.get("versions")
        require(isinstance(versions, list) and 0 < len(versions) <= 256, "invalid versions")
        seen_versions = set()
        for release in versions:
            require(isinstance(release, dict), "invalid version record")
            version = release.get("version")
            require(isinstance(version, str) and VERSION.fullmatch(version)
                    and version not in seen_versions, "invalid/duplicate version")
            seen_versions.add(version)
            require(release.get("manifest") == f"fonts/{key}/{version}/manifest.json", "invalid manifest path")
        release = versions[-1]
        version = release["version"]
        directory = f"fonts/{key}/{version}/"
        manifest = decode_json(commit.read(release["manifest"], MAX_METADATA_BYTES))
        require(type(manifest.get("schemaVersion")) is int and manifest["schemaVersion"] == 1
                and manifest.get("family") == name and manifest.get("familyKey") == key
                and manifest.get("version") == version, "manifest identity differs from catalogue")
        license_info = manifest.get("license")
        require(isinstance(license_info, dict) and valid_text(license_info.get("name"), 1024), "missing license")
        for filename in (license_info.get("file"), manifest.get("authorsFile")):
            require(isinstance(filename, str) and FILENAME.fullmatch(filename), "invalid license/authors path")
            commit.read(directory + filename, MAX_METADATA_BYTES)
        coverage = manifest.get("coverage")
        require(isinstance(coverage, dict) and coverage.get("ascii") is True
                and type(coverage.get("vietnamese")) is bool, "invalid declared coverage")
        source_faces = manifest.get("faces")
        require(isinstance(source_faces, dict) and 0 < len(source_faces) <= 4
                and set(source_faces) <= set(STYLES), "invalid face list")
        faces = {}
        for style in STYLES:
            if style not in source_faces:
                continue
            face = source_faces[style]
            require(isinstance(face, dict), "invalid face")
            filename = face.get("file")
            require(isinstance(filename, str) and len(directory + filename) <= 512
                    and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*\.(?:ttf|otf)", filename), "invalid font filename")
            path = directory + filename
            require(path not in paths, "duplicate face path")
            paths.add(path)
            expected_weight = 700 if style in ("bold", "bolditalic") else 400
            expected_style = "italic" if style in ("italic", "bolditalic") else "normal"
            require(face.get("weight") == expected_weight and face.get("style") == expected_style,
                    "incorrect manifest weight/style")
            digest, size = face.get("sha256"), face.get("bytes")
            require(isinstance(digest, str) and SHA256.fullmatch(digest)
                    and type(size) is int and 12 <= size <= MAX_FONT_BYTES, "invalid face size/digest")
            data = commit.read(path, MAX_FONT_BYTES)
            require(len(data) == size and hashlib.sha256(data).hexdigest() == digest, "face size/hash mismatch: " + path)
            require(inspect_font(data, name, style) == manifest.get("format"), "font format differs from manifest")
            faces[style] = {"path": path, "bytes": size, "sha256": digest}
        families.append({"name": name, "key": key, "version": version, "license": license_info["name"],
                         "source": REPOSITORY + "/tree/" + revision + "/" + directory.rstrip("/"),
                         "vietnamese": coverage["vietnamese"], "faces": faces})
    result = {"schemaVersion": 1, "repository": REPOSITORY, "revision": revision,
              "families": sorted(families, key=lambda family: family["key"])}
    encoded = (json.dumps(result, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    require(len(encoded) <= MAX_METADATA_BYTES, "generated catalogue exceeds runtime bound")
    return encoded


def write_catalog(output, data):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        temporary.replace(output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True, type=Path, help="local FontSource Git checkout")
    parser.add_argument("--revision", required=True, help="full 40-character published commit SHA")
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[2] / "src/assets/fonts/fontsource-catalog.json")
    args = parser.parse_args()
    try:
        data = build_catalog(args.repository, args.revision)
        write_catalog(args.output, data)
    except (ValueError, OSError, UnicodeError, struct.error) as error:
        parser.exit(1, f"FontSource catalogue unchanged: {error}\n")
    print(f"Wrote {args.output} from {args.revision}")


if __name__ == "__main__":
    main()
