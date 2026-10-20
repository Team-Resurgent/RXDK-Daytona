#!/usr/bin/env python3
"""Stage the user's ROM images for the original Xbox (deployed to D:\\data).

The images are the desktop build's verified import (m2import, every file
checked against the ROM set; build-daytona/rom_cache/daytona for Revision A).

The five regions the runtime pages (src/rom.h) are 48 MB, more than a 64 MB
console can hold, but they compress to about 12 MB: each is written as
<name>.z, its bytes in independently deflated 16 KB blocks the Xbox unpacks
as the game first reads them, so nothing is read from the disc during play.
A region is cut after its last nonzero block: the importer's zero padding is
not stored, and reads past the cut return zeros, as the padding did.

  <name>.z   "M2Z1", then little-endian u32s: the block size, the region's
             full size, the number of blocks stored (n), n + 1 offsets of the
             blocks' data from the end of this table; then the data. A block
             whose data is the block size long is stored as it is.

With --nvram DIR, settings saved by the desktop game (ioboard_eeprom.bin,
backup_ram.bin: a single cabinet set in test mode) go along as the Xbox's
starting settings.
"""
from __future__ import annotations

import argparse
import shutil
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BLOCK = 16384
PAGED = ["program", "main_data", "polygons", "textures", "copro_data"]  # rt::RomRegion order
WHOLE = ["copro_tables", "sound_program", "pcm1", "pcm2"]


def packed(data: bytes) -> bytes:
    end = len(data)
    while end >= BLOCK and not any(data[end - BLOCK:end]):
        end -= BLOCK
    blocks = [data[i:i + BLOCK] for i in range(0, end, BLOCK)]
    chunks = []
    for block in blocks:
        z = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw deflate: src/inflate.h
        out = z.compress(block) + z.flush()
        # Not smaller, or a short last block: kept as it is (a stored block
        # is exactly BLOCK long, so a short one must be compressed).
        chunks.append(block if len(out) >= len(block) and len(block) == BLOCK else out)
    offsets = [0]
    for chunk in chunks:
        offsets.append(offsets[-1] + len(chunk))
    header = b"M2Z1" + struct.pack(f"<3I{len(offsets)}I", BLOCK, len(data), len(chunks), *offsets)
    return header + b"".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--set", default="daytona", choices=["daytona", "daytona93"])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--nvram", type=Path, help="the desktop game's saved settings folder")
    parser.add_argument("--output", type=Path, default=HERE / "game/data")
    args = parser.parse_args()
    build = (args.build_dir or ROOT / ("build-daytona" if args.set == "daytona" else "build")).resolve()
    images = build / "rom_cache" / args.set
    if not (images / "program.bin").exists():
        raise SystemExit(f"no imported ROM images in {images}: run scripts/setup.py with your ROM set first")

    args.output.mkdir(parents=True, exist_ok=True)
    # (Earlier layouts: the regions uncompressed, and their sizes.)
    for stale in [f"{name}.bin" for name in PAGED] + ["regions.txt"]:
        (args.output / stale).unlink(missing_ok=True)
    total = 0
    for name in PAGED + WHOLE:
        data = (images / f"{name}.bin").read_bytes()
        target = args.output / (f"{name}.z" if name in PAGED else f"{name}.bin")
        if name in PAGED:
            if len(data) % BLOCK:
                raise SystemExit(f"{name}.bin is not whole 16 KB blocks")
            stored = packed(data)
            print(f"  {target.name}: {len(data) / 1048576:.2f} MB in {len(stored) / 1048576:.2f} MB")
            data = stored
        else:
            print(f"  {target.name}: {len(data) / 1048576:.2f} MB")
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
        total += len(data)
    if args.nvram:
        for name in ("ioboard_eeprom.bin", "backup_ram.bin"):
            shutil.copyfile(args.nvram / name, args.output / name)
        print(f"  starting settings from {args.nvram}")
    print(f"Staged {total / 1048576:.1f} MB for the Xbox: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
