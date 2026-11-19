"""Validate the architecture and deployment target of deployed Mach-O files."""

from pathlib import Path
import re
import subprocess


MACH_O_MAGIC = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}


def version_tuple(value: str) -> tuple[int, int, int]:
    if not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", value):
        raise ValueError(f"Invalid macOS version: {value!r}")
    parts = tuple(int(part) for part in value.split("."))
    return (*parts, 0) if len(parts) == 2 else parts


def minimum_macos(load_commands: str) -> str:
    versions = []
    for command in re.split(r"(?m)^Load command \d+\s*$", load_commands):
        if re.search(r"\bcmd LC_BUILD_VERSION\b", command):
            # otool versions print either the numeric platform or its name.
            if not re.search(r"\bplatform\s+(?:1|MACOS|macos)\s*$", command, re.M):
                raise RuntimeError("Mach-O slice does not target macOS")
            match = re.search(r"\bminos\s+(\d+\.\d+(?:\.\d+)?)\b", command)
        elif re.search(r"\bcmd LC_VERSION_MIN_MACOSX\b", command):
            match = re.search(r"\bversion\s+(\d+\.\d+(?:\.\d+)?)\b", command)
        else:
            continue
        if not match:
            raise RuntimeError("Missing minimum OS in Mach-O load command")
        versions.append(match.group(1))
    if not versions:
        raise RuntimeError("Mach-O slice has no macOS deployment target")
    return max(versions, key=version_tuple)


def verify_binary(path: Path, architecture: str, target: str) -> str:
    subprocess.run(["lipo", str(path), "-verify_arch", architecture], check=True)
    commands = subprocess.check_output(
        ["otool", "-arch", architecture, "-l", str(path)], text=True)
    minimum = minimum_macos(commands)
    if version_tuple(minimum) > version_tuple(target):
        raise RuntimeError(
            f"{path} requires macOS {minimum}, above package target {target}")
    return minimum


def verify_tree(root: Path, architecture: str, target: str) -> None:
    version_tuple(target)
    count = 0
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) not in MACH_O_MAGIC:
                continue
        verify_binary(path, architecture, target)
        count += 1
    if not count:
        raise RuntimeError(f"No Mach-O files found in {root}")
    print(f"Verified {count} Mach-O files: {architecture}, macOS <= {target}", flush=True)
