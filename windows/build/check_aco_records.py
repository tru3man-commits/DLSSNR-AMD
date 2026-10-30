#!/usr/bin/env python3
"""Reject a package whose ACO network SPIR-V has no matching machine code."""
import argparse
from pathlib import Path


def check(shaders: Path, records: Path) -> int:
    programs = sorted(shaders.glob("g_*.spv"))
    if not programs:
        raise ValueError(f"no network shaders in {shaders}")
    missing = []
    for program in programs:
        # Same FNV seed and byte order as nr::pal::hash.
        value = 1469598103934665603
        for byte in program.read_bytes():
            value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        record = records / f"{value:016x}.nrp"
        if not record.is_file() or record.stat().st_size == 0:
            missing.append(f"{program.name}: {record.name}")
    if missing:
        raise ValueError("missing ACO records:\n" + "\n".join(missing))
    return len(programs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shaders", type=Path)
    parser.add_argument("records", type=Path)
    args = parser.parse_args()
    try:
        print(f"PASS: {check(args.shaders, args.records)} ACO shaders have matching records")
    except ValueError as error:
        parser.exit(1, f"{error}\n")
