"""Validate the TrePDF version passed to CMake and package filenames."""

import argparse
import re


def app_version(value: str) -> str:
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}", value):
        raise argparse.ArgumentTypeError("use major.minor.patch without leading zeros, e.g. 1.2.3")
    return value


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, type=app_version)
    print(f"TrePDF version: {parser.parse_args().version}")
