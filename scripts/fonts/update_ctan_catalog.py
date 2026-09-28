#!/usr/bin/env python3
"""Build TrePDF's downloadable-face catalog from CTAN's public inventory.

Requires fontTools (tested with 4.59.2). Downloads go only to --cache-dir;
the generated JSON contains metadata and relative URLs, never font binaries.
Package licenses are CTAN declarations, not a new per-file license opinion.
Runtime font validation remains authoritative on each actual download.
"""

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import logging
from pathlib import Path, PurePosixPath
import re
import struct
import urllib.error
import urllib.parse
import urllib.request
import unicodedata

from fontTools.ttLib import TTFont


API = "https://www.ctan.org/json/3.0/"
DEFAULT_MIRROR = "https://ftp.fau.de/ctan/"
MAX_FONT_BYTES = 16 * 1024 * 1024
MAX_METADATA_BYTES = 32 * 1024 * 1024
REQUIRED_TABLES = {"head", "OS/2", "cmap", "name", "maxp", "hhea", "hmtx"}
UNSUPPORTED_TABLES = {"fvar", "CFF2", "COLR", "SVG ", "CBDT"}
STYLE_NAMES = ("regular", "italic", "bold", "bolditalic")
LATIN = set(range(32, 127))
VIETNAMESE = set(range(0x1EA0, 0x1EFA)) | {
    0x0102, 0x0103, 0x0110, 0x0111, 0x0128, 0x0129, 0x0168, 0x0169,
    0x01A0, 0x01A1, 0x01AF, 0x01B0,
} | {ord(c) for c in "ÀÁÂÃÈÉÊÌÍÒÓÔÕÙÚÝàáâãèéêìíòóôõùúý"}


class HTTPSOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise ValueError("non-HTTPS redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class DownloadError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url, destination, maximum, offline=False):
    if destination.exists():
        if destination.stat().st_size > maximum:
            raise ValueError("cached download exceeds size limit")
        return destination.read_bytes()
    if offline:
        raise DownloadError("not in offline cache")
    request = urllib.request.Request(url, headers={"User-Agent": "TrePDF-CTAN-Catalog/1"})
    opener = urllib.request.build_opener(HTTPSOnly())
    last_error = None
    for _ in range(2):
        try:
            with opener.open(request, timeout=30) as response:
                if int(response.headers.get("Content-Length", "0")) > maximum:
                    raise ValueError("download exceeds size limit")
                data = response.read(maximum + 1)
            if len(data) > maximum:
                raise ValueError("download exceeds size limit")
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_suffix(destination.suffix + ".part")
            temporary.write_bytes(data)
            temporary.replace(destination)
            return data
        except (OSError, ValueError) as error:
            last_error = error
    raise DownloadError(str(last_error))


def inventory(data):
    result = []
    for line in gzip.decompress(data).decode("utf-8").splitlines():
        parts = [part.strip() for part in line.split("|", 2)]
        if len(parts) != 3 or not parts[2].lower().endswith((".otf", ".ttf")):
            continue
        raw_path = parts[2]
        if (len(raw_path) > 511 or any(part in (".", "..") for part in raw_path.split("/"))
                or not re.fullmatch(r"(?:[A-Za-z0-9_.+-]+/)*[A-Za-z0-9_.+-]+\.(?:otf|ttf)", raw_path, re.I)):
            continue
        result.append({"path": raw_path, "size": int(parts[1]), "modified": parts[0]})
    return sorted(result, key=lambda item: item["path"])


def package_candidates(files, keys):
    # A package's ancestor directory normally matches its CTAN key. Each hit is
    # confirmed against the canonical ctan.path returned by the package API.
    candidates = set()
    folded_keys = {key.casefold(): key for key in keys}
    for item in files:
        if item["path"].startswith("obsolete/"):
            continue
        for component in PurePosixPath(item["path"]).parts[:-1]:
            folded = component.casefold()
            # Historical archive folders may retain case or a suffix absent
            # from the package key, e.g. Asana-Math and fourier-GUT.
            possibilities = [folded] + [folded[:index] for index, char in enumerate(folded) if char == "-"]
            for candidate in possibilities:
                if candidate in folded_keys:
                    candidates.add(folded_keys[candidate])
    return sorted(candidates)


def reviewed_digests(header):
    text = header.read_text(encoding="utf-8")
    return {(int(size), digest) for size, digest in re.findall(
        r'\{(\d+),\s*"([0-9a-f]{64})"\}', text)}


def font_name(font):
    names = font["name"]
    legacy = names.getDebugName(1) or ""
    subfamily = names.getDebugName(2) or ""
    typographic = names.getDebugName(16)
    typographic_style = names.getDebugName(17)
    if typographic and (typographic_style is None or typographic_style.casefold() == subfamily.casefold()):
        return typographic, subfamily
    return legacy, subfamily


def inspect_font(data, reviewed):
    if len(data) < 12 or len(data) > MAX_FONT_BYTES:
        raise ValueError("invalid-size")
    signature, count = struct.unpack_from(">4sH", data)
    if signature not in (b"OTTO", b"\x00\x01\x00\x00"):
        raise ValueError("unsupported-container")
    directory_end = 12 + count * 16
    if not 0 < count <= 256 or directory_end > len(data):
        raise ValueError("invalid-directory")
    tables = {}
    for index in range(count):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + index * 16)
        tag = tag.decode("latin-1")
        if tag in tables or offset < directory_end or offset + length > len(data):
            raise ValueError("invalid-table-bounds")
        tables[tag] = (offset, length)
    if not REQUIRED_TABLES <= tables.keys():
        raise ValueError("missing-required-table")
    if UNSUPPORTED_TABLES & tables.keys():
        raise ValueError("variable-or-color-font")
    if signature == b"OTTO":
        if "CFF " not in tables or {"glyf", "loca"} & tables.keys():
            raise ValueError("invalid-outline-tables")
    elif not {"glyf", "loca"} <= tables.keys() or "CFF " in tables:
        raise ValueError("invalid-outline-tables")
    if tables["OS/2"][1] < 64 or tables["head"][1] < 54:
        raise ValueError("incomplete-metrics")
    with TTFont(io.BytesIO(data), lazy=True) as font:
        os2 = font["OS/2"]
        fs_type = os2.fsType
        permissions = fs_type & 0x000F
        exception = (os2.version == 3 and fs_type == 0x000C and signature == b"OTTO"
                     and (len(data), sha256(data)) in reviewed)
        permits = (not (fs_type & 1) and not (os2.version >= 2 and fs_type & 0xFEF0)
                   and (permissions == 0 or (bool(permissions & 8) if os2.version <= 2 else permissions == 8)))
        if (fs_type & 0x0300) or (not permits and not exception):
            raise ValueError("embedding-restricted")
        bold = bool(os2.fsSelection & 0x0020)
        italic = bool(os2.fsSelection & 0x0001)
        if (os2.usWeightClass >= 700) != bold:
            raise ValueError("inconsistent-weight-style")
        names = font["name"]
        legacy_family = names.getDebugName(1) or ""
        legacy_style = names.getDebugName(2) or ""
        typographic_family = names.getDebugName(16) or legacy_family
        typographic_style = names.getDebugName(17) or legacy_style
        if (typographic_family.casefold() != legacy_family.casefold()
                and typographic_style.casefold() != legacy_style.casefold()):
            raise ValueError("platform-dependent-extended-family")
        family, style = font_name(font)
        if (not family or len(family.encode("utf-16-le")) // 2 > 128 or family != family.strip()
                or any(unicodedata.category(character) == "Cc" for character in family)
                or re.match(r"^[A-Z]{6}\+", family)):
            raise ValueError("invalid-family")
        # Nonstandard subfamilies such as Light/Condensed must be identified by
        # their distinct legacy family, never folded into the regular face.
        normalized_style = re.sub(r"[\s_-]+", "", style.casefold())
        accepted_styles = {
            0: {"regular", "normal", "roman", "book", "medium", "plain", ""},
            1: {"italic", "oblique", "slanted", "bookitalic", "mediumitalic"},
            2: {"bold"},
            3: {"bolditalic", "boldoblique", "boldslanted"},
        }
        style_index = int(bold) * 2 + int(italic)
        if normalized_style not in accepted_styles[style_index]:
            raise ValueError("nonstandard-subfamily")
        cmap = {code for code, glyph in (font.getBestCmap() or {}).items() if glyph != ".notdef"}
        if not cmap:
            raise ValueError("missing-unicode-cmap")
        if not LATIN <= cmap:
            raise ValueError("incomplete-basic-ascii")
        return {
            "family": family, "style": STYLE_NAMES[style_index],
            "latin": LATIN <= cmap, "vietnamese": VIETNAMESE <= cmap,
            "sha256": sha256(data), "size": len(data),
            "fsType": fs_type, "reviewedEmbeddingException": exception,
            "postscriptName": font["name"].getDebugName(6) or "",
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("src/assets/fonts/ctan-catalog.json"))
    parser.add_argument("--report", type=Path, default=Path("docs/ctan-catalog-audit.json"))
    parser.add_argument("--mirror", default=DEFAULT_MIRROR)
    parser.add_argument("--inventory", type=Path)
    parser.add_argument("--packages", type=Path)
    parser.add_argument("--licenses", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--package", action="append", default=[], help="Limit initial/debug generation to these package IDs")
    parser.add_argument("--allow-partial", action="store_true", help="Publish despite network failures; recorded in provenance")
    args = parser.parse_args()
    if not 1 <= args.workers <= 16 or not args.mirror.startswith("https://"):
        parser.error("HTTPS mirror and 1..16 workers required")
    args.mirror = args.mirror.rstrip("/") + "/"
    cache = args.cache_dir

    def source_data(path, url, filename):
        return path.read_bytes() if path else fetch(
            url, cache / filename, MAX_METADATA_BYTES, args.offline)

    file_data = source_data(args.inventory, args.mirror + "FILES.byname.gz", "FILES.byname.gz")
    package_data = source_data(args.packages, API + "packages", "packages.json")
    license_data = source_data(args.licenses, API + "licenses", "licenses.json")
    files = inventory(file_data)
    keys = {item["key"] for item in json.loads(package_data)}
    licenses = {item["key"]: item for item in json.loads(license_data)}
    candidates = package_candidates(files, keys)
    if args.package:
        candidates = sorted(set(args.package))
    packages = []
    package_errors = []

    def load_package(key):
        data = fetch(API + "pkg/" + key, cache / "packages" / (key + ".json"),
                     1024 * 1024, args.offline)
        package = json.loads(data)
        package["metadataSha256"] = sha256(data)
        return package

    print(f"Inventory: {len(files)} fonts, {sum(item['size'] for item in files)} bytes; "
          f"{len(candidates)} candidate package records", flush=True)
    with ThreadPoolExecutor(max_workers=min(args.workers, 4)) as executor:
        pending = {executor.submit(load_package, key): key for key in candidates}
        for future in as_completed(pending):
            key = pending[future]
            try:
                packages.append(future.result())
            except (OSError, ValueError) as error:
                package_errors.append({"package": key, "error": str(error)})
            if (len(packages) + len(package_errors)) % 25 == 0:
                print(f"Package records: {len(packages)} okay, {len(package_errors)} failed", flush=True)

    packages.sort(key=lambda item: item["id"])
    if package_errors and not args.allow_partial:
        raise RuntimeError(f"{len(package_errors)} package fetches failed; previous catalog preserved: {package_errors[:3]}")
    matched = []
    excluded = []
    for item in files:
        path = item["path"]
        if not re.fullmatch(r"(?:[A-Za-z0-9_.+-]+/)*[A-Za-z0-9_.+-]+\.(?:otf|ttf)", path, re.I):
            excluded.append({"path": path, "reason": "unsupported-path-characters"})
            continue
        if path.startswith("obsolete/"):
            excluded.append({"path": path, "reason": "obsolete"})
            continue
        if item["size"] > MAX_FONT_BYTES:
            excluded.append({"path": path, "reason": "file-too-large"})
            continue
        matches = [package for package in packages if path.startswith(
            package.get("ctan", {}).get("path", "!").lstrip("/").rstrip("/") + "/")]
        if not matches:
            excluded.append({"path": path, "reason": "unmapped-package"})
            continue
        package = max(matches, key=lambda value: len(value["ctan"]["path"]))
        declared = package.get("license", [])
        if isinstance(declared, str):
            declared = [declared]
        if not declared or not all(licenses.get(key, {}).get("free") is True for key in declared):
            excluded.append({"path": path, "reason": "nonfree-or-unknown-package-license", "package": package["id"]})
            continue
        matched.append((item, package))
    reviewed = reviewed_digests(Path(__file__).resolve().parents[2] / "src/ReviewedCtanFonts.h")
    inspected = []
    download_errors = []

    def load_font(item, package):
        path = item["path"]
        data = fetch(args.mirror + urllib.parse.quote(path), cache / "files" / path,
                     MAX_FONT_BYTES, args.offline)
        face = inspect_font(data, reviewed)
        face.update({"path": path, "package": package["id"]})
        return face

    print(f"Inspecting {len(matched)} licensed fonts "
          f"({sum(item['size'] for item, _ in matched)} bytes before cache)", flush=True)
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        pending = {executor.submit(load_font, item, package): (item, package)
                   for item, package in matched}
        for index, future in enumerate(as_completed(pending), 1):
            item, package = pending[future]
            try:
                inspected.append(future.result())
            except DownloadError as error:
                download_errors.append({"path": item["path"], "error": str(error)})
            except Exception as error:
                reason = str(error)
                excluded.append({"path": item["path"], "reason": reason, "package": package["id"]})
            if index % 100 == 0 or index == len(matched):
                print(f"Font files: {index}/{len(matched)}, {len(inspected)} eligible", flush=True)
    if download_errors and not args.allow_partial:
        raise RuntimeError(f"{len(download_errors)} font fetches failed; previous catalog preserved: {download_errors[:3]}")

    by_family = defaultdict(list)
    for face in inspected:
        by_family[face["family"].casefold()].append(face)
    package_by_key = {item["id"]: item for item in packages}
    families = []
    chosen = []
    for _, faces in sorted(by_family.items()):
        selected = {}
        # Preserve the documented primary package and static OTF face when a
        # CTAN mirror contains duplicate copies of the same family/style.
        package_styles = defaultdict(set)
        for face in faces:
            package_styles[face["package"]].add(face["style"])
        primary_package = min(package_styles, key=lambda key: (-len(package_styles[key]), key))
        faces.sort(key=lambda face: (face["package"] != primary_package,
                                    not face["path"].lower().endswith(".otf"), face["path"]))
        for face in faces:
            if face["package"] != primary_package:
                excluded.append({"path": face["path"], "reason": "other-package-family-copy",
                                 "preferredPackage": primary_package})
                continue
            if face["style"] not in selected:
                selected[face["style"]] = face
                chosen.append(face)
            else:
                excluded.append({"path": face["path"], "reason": "duplicate-family-style",
                                 "preferred": selected[face["style"]]["path"],
                                 "identicalBytes": face["sha256"] == selected[face["style"]]["sha256"]})
        source_package = package_by_key[faces[0]["package"]]
        declared = source_package.get("license", [])
        if isinstance(declared, str):
            declared = [declared]
        subsets = []
        if all(face["latin"] for face in selected.values()):
            subsets.append("latin")
        if all(face["vietnamese"] for face in selected.values()):
            subsets.append("vietnamese")
        license_label = "; ".join(licenses[key]["name"] for key in sorted(declared))
        if source_package["id"] == "tex-gyre":
            license_label = "GUST Font License 1.0 / LPPL-1.3c-or-later"
        families.append({
            "name": faces[0]["family"],
            "license": license_label,
            "source": "https://ctan.org/pkg/" + source_package["id"],
            "subsets": subsets,
            "faces": {style: "/" + selected[style]["path"] for style in STYLE_NAMES if style in selected},
        })
    if not families:
        raise ValueError("No eligible families found; previous catalog preserved")
    provenance = {
        "inventorySource": args.mirror + "FILES.byname.gz",
        "inventorySha256": sha256(file_data),
        "packageApi": API,
        "packageListSha256": sha256(package_data),
        "licenseListSha256": sha256(license_data),
        "scope": "Direct .otf/.ttf files in CTAN FILES.byname; excludes archives and obsolete tree",
        "licenseEvidence": "All licenses declared for the mapped CTAN package are classified free by CTAN",
        "fontEvidence": "Downloaded full font bytes; static sfnt tables, name, OS/2 flags and Unicode cmap inspected with fontTools",
        "generatedDate": datetime.now(timezone.utc).date().isoformat(),
        "packageFilter": args.package,
        "partial": bool(package_errors or download_errors or args.package),
    }
    stats = {
        "inventoryFontFiles": len(files), "inventoryBytes": sum(item["size"] for item in files),
        "packageRecords": len(packages), "packageErrors": len(package_errors),
        "downloadErrors": len(download_errors),
        "licensedFontFiles": len(matched), "inspectedEligibleFaces": len(inspected),
        "families": len(families), "selectedFaces": len(chosen),
        "exclusions": dict(sorted(Counter(item["reason"] for item in excluded).items())),
    }
    result = {"version": 1, "provenance": provenance, "statistics": stats, "families": families}
    report = {"version": 1, "provenance": provenance, "statistics": stats,
              "packageErrors": sorted(package_errors, key=lambda item: item["package"]),
              "downloadErrors": sorted(download_errors, key=lambda item: item["path"]),
              "packages": [{"id": item["id"], "path": item.get("ctan", {}).get("path"),
                            "license": item.get("license"), "metadataSha256": item["metadataSha256"]}
                           for item in packages],
              "faces": sorted(chosen, key=lambda item: item["path"]),
              "excluded": sorted(excluded, key=lambda item: item["path"])}
    for target, value in ((args.output, result), (args.report, report)):
        encoded = (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode()
        if target == args.output and (len(encoded) > 2 * 1024 * 1024 or len(families) > 4096):
            raise ValueError("catalog exceeds runtime limits")
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        temporary.write_bytes(encoded)
        temporary.replace(target)
    print(json.dumps(stats, indent=2), flush=True)


if __name__ == "__main__":
    logging.getLogger("fontTools").setLevel(logging.ERROR)
    main()
