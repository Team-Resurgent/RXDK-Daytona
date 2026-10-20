#!/usr/bin/env python3
"""Write the original Xbox build's RXDK projects.

  rxdk.project.json           the title: platform/xboxog/src/main.cpp, the
                              runtime, i960 and trace sources
  gamecode/rxdk.project.json  the generated game code (i960, TGP, sound),
                              from the user's own ROM set: its own library,
                              at -Os, so runtime and frontend changes never
                              recompile it
  libs/softfloat, libs/ymfm   the pinned dependencies scripts/setup.py
                              fetched into extern/

Every project is built with the Dreamcast port's runtime definitions
(platform/dreamcast/Makefile GAME_DEFS, without M2_DC_SPEED: that one
composes the tile layers in the PVR's 16-bit formats).
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXTERN = ROOT / "extern"

RUNTIME = ["cpu", "tgp", "m2_tgp_board", "geo", "raster", "video", "m2_board", "game_loop", "zip", "rom_import",
           "archive", "m2_replay_bus", "lockstep", "snd_cpu", "multipcm", "sound_board", "native_sample_mixer",
           "native_sound_sequencer", "native_sound_engine", "enhance", "comm_board"]
I960 = ["isa", "decode", "reach", "fp"]
LIBRARIES = ["libd3d8.lib", "libxgraphics.lib", "libdsound.lib", "libxapi.lib", "libcpp.lib", "libc.lib",
             "libkernel.lib"]
# The generated floating point is validated without fast-math or contraction.
FLOAT_FLAGS = ["-fno-fast-math", "-ffp-contract=off"]
SOFTFLOAT_PUBLIC = ["SOFTFLOAT_FAST_INT64", "LITTLEENDIAN=1", "THREAD_LOCAL="]


def rel(path: Path, base: Path) -> str:
    return Path(os.path.relpath(path, base)).as_posix()


def write(directory: Path, project: dict) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "rxdk.project.json").write_text(json.dumps(project, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--set", default="daytona", choices=["daytona", "daytona93"])
    parser.add_argument("--build-dir", type=Path, help="the desktop build the game code was generated in "
                        "(default: build-daytona for daytona, build for daytona93)")
    args = parser.parse_args()
    build = (args.build_dir or ROOT / ("build-daytona" if args.set == "daytona" else "build")).resolve()
    defines = [f'M2_ROMSET="{args.set}"', "M2_DC_MEMORY", "M2_DC_SPIN_SKIP", "NDEBUG", "NOMINMAX"] + SOFTFLOAT_PUBLIC

    softfloat = HERE / "libs/softfloat"
    listed = (ROOT / "cmake/softfloat_sources.cmake").read_text()
    sources = [EXTERN / "softfloat/source" / name for name in re.findall(r"\$\{SOFTFLOAT_DIR\}/source/(\S+\.c)", listed)]
    if not sources:
        raise SystemExit("run scripts/setup.py first: no SoftFloat sources listed")
    write(softfloat, {
        "name": "softfloat", "type": "library", "configuration": "release",
        "sources": [rel(p, softfloat) for p in sources],
        "includePaths": [rel(ROOT / "cmake/softfloat-portable", softfloat),
                         rel(EXTERN / "softfloat/source/8086-SSE", softfloat)],
        "publicIncludePaths": [rel(EXTERN / "softfloat/source/include", softfloat)],
        "defines": ["SOFTFLOAT_ROUND_ODD", "SOFTFLOAT_FAST_DIV32TO16", "SOFTFLOAT_FAST_DIV64TO32"] + SOFTFLOAT_PUBLIC,
        "compileFlags": FLOAT_FLAGS + ["-w"], "outputDir": "out"})

    ymfm = HERE / "libs/ymfm"
    write(ymfm, {
        "name": "ymfm", "type": "library", "configuration": "release",
        "sources": [rel(EXTERN / f"ymfm/src/{name}.cpp", ymfm) for name in ("ymfm_opn", "ymfm_adpcm", "ymfm_ssg")],
        "publicIncludePaths": [rel(EXTERN / "ymfm/src", ymfm)],
        "defines": ["NDEBUG"], "compileFlags": ["-w"], "cppStandard": "c++20", "outputDir": "out"})

    gamecode = HERE / "gamecode"
    gen = build / "gen" / args.set
    tgp, snd = build / "gen" / f"{args.set}_tgp/tgp_gen.cpp", build / "gen" / f"{args.set}_snd/snd_gen.cpp"
    code = sorted(gen.glob("*.cpp")) + [tgp, snd]
    missing = [str(p) for p in (tgp, snd) if not p.exists()] + ([] if len(code) > 2 else [str(gen)])
    if missing:
        raise SystemExit("generate the game code first (scripts/setup.py); missing: " + ", ".join(missing))
    write(gamecode, {
        "name": "gamecode", "type": "library", "configuration": "release",
        "sources": [rel(p, gamecode) for p in code],
        "includePaths": [rel(ROOT / "src", gamecode)],
        "defines": defines,
        # -Os after RXDK's -O3: on the Dreamcast 2.5 MB of code instead of
        # 6.7 MB at -O2, and memory is what the Xbox is short of.
        "compileFlags": FLOAT_FLAGS + ["-Os", "-Wno-unused-label"],
        "cppStandard": "c++20", "outputDir": "out"})

    sources = [HERE / "src/main.cpp"] + [ROOT / f"src/runtime/{n}.cpp" for n in RUNTIME] + \
              [ROOT / f"src/i960/{n}.cpp" for n in I960] + [ROOT / "src/trace/trace.cpp"]
    write(HERE, {
        "name": "daytona", "configuration": "release",
        "sources": [rel(p, HERE) for p in sources],
        "includePaths": [rel(ROOT / "src", HERE), rel(HERE / "src", HERE)],
        "projectReferences": ["gamecode", "libs/softfloat", "libs/ymfm"],
        "libraries": LIBRARIES,
        "defines": defines,
        "compileFlags": FLOAT_FLAGS,
        "cppStandard": "c++20", "outputDir": "out",
        "deployPaths": ["data=game/data"],
        "imageBuild": {"testName": "Daytona USA"}})
    print(f"Wrote the Xbox projects: {len(code)} game code sources")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
