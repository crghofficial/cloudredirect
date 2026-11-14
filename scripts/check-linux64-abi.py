#!/usr/bin/env python3
"""Check the ELF headers, imported symbol versions and injected CLI entry point."""

import argparse
from pathlib import Path
import re
import struct
import subprocess


def check_elf(path):
    with Path(path).open("rb") as file:
        header = file.read(64)
    if (len(header) != 64 or header[:7] != b"\x7fELF\x02\x01\x01"
            or struct.unpack_from("<H", header, 18)[0] != 62):
        raise ValueError(f"{path} must be a little-endian x86-64 ELF")


def check_versions(path):
    versions = subprocess.check_output(["readelf", "-V", str(path)], text=True)
    for prefix, limit in {"GLIBC": (2, 31), "GLIBCXX": (3, 4, 28), "CXXABI": (1, 3, 12)}.items():
        found = [tuple(map(int, version.split("."))) for version in
                 re.findall(r"Name: " + prefix + r"_([0-9.]+)", versions)]
        if found and max(found) > limit:
            raise ValueError(f"{path} requires {prefix}_{max(found)}, exceeding {limit}")
        if found:
            print(f"{path.name}: {prefix}_{'.'.join(map(str, max(found)))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--cli", type=Path)
    args = parser.parse_args()
    try:
        for path in [args.library] + ([args.cli] if args.cli else []):
            check_elf(path)
            check_versions(path)
        symbols = subprocess.check_output(
            ["nm", "-D", "--defined-only", str(args.library)], text=True)
        if not re.search(r"\bCloudRedirect_CliMain$", symbols, re.MULTILINE):
            raise ValueError("Missing exported CloudRedirect_CliMain")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ABI check failed: {error}\n")
    print("Steam Runtime x86-64 ABI check passed")


if __name__ == "__main__":
    main()
