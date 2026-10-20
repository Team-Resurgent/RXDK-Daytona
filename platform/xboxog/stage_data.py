#!/usr/bin/env python3
"""Stage the user's ROM images for the original Xbox (deployed to D:\\data).

The images are the desktop build's verified import (m2import, every file
checked against the ROM set; build-daytona/rom_cache/daytona for Revision A).
The five regions the runtime pages (src/rom.h) are cut after their last
nonzero 4 KB page: the importer's zero padding is not stored, and reads past
the cut return zeros, as the padding did. regions.txt records each region's
full size. With --nvram DIR, settings saved by the desktop game
(ioboard_eeprom.bin, backup_ram.bin: a single cabinet set in test mode) go
along as the Xbox's starting settings.
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PAGE = 4096
PAGED = ["program", "main_data", "polygons", "textures", "copro_data"]  # rt::RomRegion order
WHOLE = ["copro_tables", "sound_program", "pcm1", "pcm2"]


def trimmed(data: bytes) -> bytes:
    end = len(data)
    while end >= PAGE and not any(data[end - PAGE:end]):
        end -= PAGE
    return data[:-(-end // PAGE) * PAGE]


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
    total, table = 0, []
    for name in PAGED + WHOLE:
        data = (images / f"{name}.bin").read_bytes()
        if name in PAGED:
            if len(data) % PAGE:
                raise SystemExit(f"{name}.bin is not whole pages")
            table.append(f"{name} {len(data)}")
            data = trimmed(data)
        target = args.output / f"{name}.bin"
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
        total += len(data)
        print(f"  {name}.bin: {len(data) / 1048576:.2f} MB")
    (args.output / "regions.txt").write_text("\n".join(table) + "\n")
    if args.nvram:
        for name in ("ioboard_eeprom.bin", "backup_ram.bin"):
            shutil.copyfile(args.nvram / name, args.output / name)
        print(f"  starting settings from {args.nvram}")
    print(f"Staged {total / 1048576:.1f} MB for the Xbox: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
