#!/usr/bin/env python3
"""Build Daytona USA for the original Xbox from your own ROM set, in one go.

  python platform/xboxog/build.py --rom PATH [PATH ...]

PATH is your MAME ROM set: a .zip or .7z, or a folder of its files. A clone
set and its parent may be given together (folders or zips), and are merged.
The program ROMs say which game it is: daytona (Revision A, 1994) or
daytona93 (Deluxe '93).

The result, in platform/xboxog/dist/:

  Daytona USA/      default.xbe and data/: copy the folder to the Xbox's
                    hard disk (e.g. E:\\Games\\Daytona USA) and launch it
  Daytona USA.iso   the same as a disc image (xemu, or burn/extract)

Both contain your ROM set's data and code generated from it: they are for
you, never to share.

Steps (each is skipped when its output is already up to date):
  1. the ROM set into roms/<set>.zip;
  2. scripts/setup.py's toolchain check and pinned sources (extern/);
  3. the desktop build of the set: the ROM import (every file checked) and
     the game code recompiled to C++ (scripts/recompile.py);
  4. Revision A only: its factory settings are a linked twin cabinet that
     waits for a second machine, so the desktop runs it through test mode
     (single_cabinet.txt) and keeps the settings as the Xbox's first ones;
  5. stage_data.py and gen_project.py: the Xbox data and RXDK projects;
  6. the RXDK build (rxdk build), then the dist/ folder and disc image.

Needs: Python 3.10+, Git, CMake and Visual Studio 2022 with C++ (as
scripts/setup.py), and RXDK v1.3.8 or newer (RXDK_CLI or RXDK may point at
a particular one).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import setup  # noqa: E402  (scripts/setup.py: fetch(), configure_and_build())

TITLE = "Daytona USA"
# The program ROMs that tell the two sets apart (scripts/setup.py ROM_REJECTED).
SETS = {"daytona": "epr-16722a.12", "daytona93": "epr-16530a.12"}
MIN_RXDK = (1, 3, 8)  # libdsound before 1.3.8 called libc's sin/pow as __stdcall


def say(message: str) -> None:
    print(f"\n== {message}", flush=True)


def run(cmd: list, **kw) -> None:
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    if subprocess.run([str(c) for c in cmd], **kw).returncode:
        sys.exit(f"build: failed: {Path(str(cmd[0])).name} (its errors are above)")


def set_of(names: set[str]) -> str:
    found = [s for s, program in SETS.items() if program in names]
    if len(found) != 1:
        sys.exit("build: this is not a Daytona USA set this port can use. It needs MAME's daytona (Revision A: "
                 "epr-16722a.12) or daytona93 (Deluxe '93: epr-16530a.12); other sets have different programs.")
    return found[0]


def import_rom(paths: list[Path]) -> tuple[str, Path]:
    """The ROM set at roms/<set>.zip (or .7z): copied, or zipped from folders."""
    paths = [p.expanduser().resolve() for p in paths]
    for p in paths:
        if not p.exists():
            sys.exit(f"build: {p} does not exist")
    if len(paths) == 1 and paths[0].suffix.lower() == ".7z":
        # (Its names cannot be listed here: the set is the file's name.)
        romset = paths[0].stem.lower()
        if romset not in SETS:
            sys.exit("build: name a .7z ROM set daytona.7z or daytona93.7z, or give a .zip or folder")
        target = ROOT / "roms" / f"{romset}.7z"
        if target != paths[0]:
            target.parent.mkdir(exist_ok=True)
            shutil.copyfile(paths[0], target)
        return romset, target
    files: dict[str, bytes] = {}
    for p in paths:  # later paths win: give a parent set first, then its clone
        if p.is_dir():
            for f in sorted(p.iterdir()):
                if f.is_file():
                    files[f.name.lower()] = f.read_bytes()
        elif zipfile.is_zipfile(p):
            with zipfile.ZipFile(p) as z:
                for info in z.infolist():
                    if not info.is_dir():
                        files[Path(info.filename).name.lower()] = z.read(info)
        else:
            sys.exit(f"build: {p} is not a .zip, .7z or folder")
    romset = set_of(set(files))
    target = ROOT / "roms" / f"{romset}.zip"
    digest = hashlib.sha256(b"".join(n.encode() + hashlib.sha256(d).digest() for n, d in sorted(files.items())))
    stamp = ROOT / "roms" / f".{romset}.contents"
    if not (target.exists() and stamp.exists() and stamp.read_text() == digest.hexdigest()):
        target.parent.mkdir(exist_ok=True)
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in sorted(files.items()):
                z.writestr(name, data)
        stamp.write_text(digest.hexdigest())
        print(f"Wrote {target.relative_to(ROOT)} ({len(files)} files)")
    return romset, target


def recompile(romset: str, rom: Path) -> Path:
    """The desktop build of the set, with its imported ROM images and generated code."""
    build = ROOT / ("build" if romset == "daytona93" else f"build-{romset}")
    stamp = build / ".xboxog-rom"
    rom_hash = hashlib.sha256(rom.read_bytes()).hexdigest()
    if (build / "gen" / romset).is_dir() and stamp.exists() and stamp.read_text() == rom_hash:
        print(f"{build.relative_to(ROOT)}: game code already generated from this ROM set")
        return build
    say("Fetching the pinned sources (scripts/setup.py)")
    run([sys.executable, ROOT / "scripts/setup.py", "--no-build"], cwd=ROOT)
    say(f"Configuring the desktop build of {romset}")
    setup.configure_and_build(str(build), romset)
    # A different ROM set in the same place: import it again.
    shutil.rmtree(build / "rom_cache" / romset, ignore_errors=True)
    say("Checking your ROM set and recompiling the game code (scripts/recompile.py)")
    r = subprocess.run([sys.executable, str(ROOT / "scripts/recompile.py"), "--set", romset,
                        "--build-dir", build.name], cwd=ROOT)
    if r.returncode == 3:  # recompile.py's ROM_REJECTED_EXIT
        sys.exit(setup.ROM_REJECTED.format(set=romset))
    if r.returncode:
        sys.exit(setup.BUILD_FAILED)
    stamp.write_text(rom_hash)
    return build


def single_cabinet(build: Path, romset: str) -> Path | None:
    """Revision A's settings for one cabinet, from a desktop run through test mode."""
    if romset != "daytona":
        return None
    nvram = build / "xboxog-nvram"
    script = HERE / "single_cabinet.txt"
    if (nvram / "ioboard_eeprom.bin").exists() and (nvram / "ioboard_eeprom.bin").stat().st_mtime > script.stat().st_mtime:
        return nvram
    m2run = next((p for p in (build / "Release/m2run.exe", build / "m2run.exe", build / "m2run") if p.exists()), None)
    if not m2run:
        sys.exit(f"build: no m2run in {build}: the desktop build did not finish")
    say("Setting a single cabinet in test mode (Revision A's factory settings are a linked twin)")
    nvram.mkdir(exist_ok=True)
    run([m2run, build / "rom_cache" / romset, "3600", "--inputs", script, "--save-nvram", nvram],
        cwd=ROOT, stdout=subprocess.DEVNULL)
    return nvram


def rxdk() -> tuple[Path, Path]:
    """The RXDK command line and the RXDK root whose SDK it builds with."""
    root = Path(os.environ.get("RXDK", r"C:\ProgramData\RXDK"))
    cli = os.environ.get("RXDK_CLI")
    candidates = [Path(cli)] if cli else [root / "engine/Rxdk.Cli.exe", root / "tools/Rxdk.Cli.exe"]
    if not cli and shutil.which("rxdk"):
        candidates.append(Path(shutil.which("rxdk")))
    found = next((c for c in candidates if c.exists()), None)
    if not found:
        sys.exit("build: RXDK not found. Install it (https://github.com/Team-Resurgent), or set RXDK to its folder "
                 "or RXDK_CLI to Rxdk.Cli.exe")
    version_file = root / "sdk/VERSION"
    if version_file.exists():
        text = version_file.read_text().strip().lstrip("v")
        try:
            version = tuple(int(x) for x in text.split(".")[:3])
        except ValueError:
            version = None
        if version and version < MIN_RXDK:
            sys.exit(f"build: RXDK {text} is too old (its DirectSound library corrupts the stack); "
                     f"update to v{'.'.join(map(str, MIN_RXDK))} or newer")
    return found, root


def package(romset: str, root: Path) -> None:
    built = HERE / "out/Build/daytona"
    if not (built / "default.xbe").exists():
        sys.exit(f"build: no {built / 'default.xbe'}: the RXDK build did not finish")
    dist = HERE / "dist"
    folder = dist / TITLE
    shutil.rmtree(folder, ignore_errors=True)
    shutil.copytree(built, folder)
    iso = dist / f"{TITLE}.iso"
    xdvdfs = root / "tools/xdvdfs.exe"
    if xdvdfs.exists():
        iso.unlink(missing_ok=True)
        run([xdvdfs, "pack", folder, iso], stdout=subprocess.DEVNULL)
    else:
        print(f"(no {xdvdfs}: the disc image was not made)")
    say("Done")
    print(f"\n{TITLE} ({romset}) for the original Xbox:\n\n  {folder}\n"
          f"      copy this folder to the Xbox's hard disk and launch default.xbe\n")
    if iso.exists():
        print(f"  {iso}\n      the same as a disc image (xemu)\n")
    print("These contain your ROM set's data and code generated from it: they are for you, never to share.\n"
          "Controls: stick steers, triggers accelerate and brake, D-pad up/down (or black/white) shift,\n"
          "A/B/X change the view, Y or Back adds a coin, Start starts.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", type=Path, nargs="+", required=True, help="your ROM set: .zip, .7z or folder(s)")
    args = parser.parse_args()

    say("Your ROM set")
    romset, rom = import_rom(args.rom)
    print(f"{rom.relative_to(ROOT)}: {romset}")
    cli, root = rxdk()  # before the long steps: fail early without RXDK
    build = recompile(romset, rom)
    nvram = single_cabinet(build, romset)

    say("The Xbox data and projects")
    stage = [sys.executable, HERE / "stage_data.py", "--set", romset, "--build-dir", build]
    run(stage + (["--nvram", nvram] if nvram else []))
    run([sys.executable, HERE / "gen_project.py", "--set", romset, "--build-dir", build])

    say(f"Building the Xbox title with RXDK ({cli})")
    run([cli, "build", "--project-root", HERE], cwd=HERE)
    package(romset, root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
