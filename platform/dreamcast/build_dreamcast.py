#!/usr/bin/env python3
"""The Dreamcast port's build driver. Run it after the desktop build: setup,
then scripts/recompile.py --set daytona --build-dir build-daytona (Revision
A, 1994, the set with link play; the port is built from that set only). It only reads the host build's generated code and ROM
images; everything it makes goes under the host build directory's
dreamcast-* folders (git-ignored), and nothing here changes the main tree.

  build_dreamcast.py kos
  build_dreamcast.py selftest|videotest [--host-build-dir build-daytona]
                    [--dreamsdk C:/DreamSDK] [--flycast FLYCAST.EXE] [--no-run]
  build_dreamcast.py compile [--host-build-dir build-daytona] [--jobs N]
  build_dreamcast.py tools   [--host-build-dir build-daytona]
  build_dreamcast.py measure [--host-build-dir build-daytona]
                             [--nvram DIR] [--frames N] [--sound] [SCRIPT ...]

kos:     sets up KallistiOS 2.2.1 in extern/kos-dc (git-ignored) and builds
         it with -m4-single; the other Dreamcast commands do this first.
selftest: builds the Dreamcast floating-point self-test (no game code) as a
         bootable disc image, dreamcast/selftest.cdi, and runs it in Flycast
         (Windows; a portable copy in dreamcast/flycast, see
         scripts/flycast_run.py): exit 0 only when every check matches the
         PC's bits.
videotest: the display path, a 496x384 frame through the PVR each frame,
         likewise; prints the conversion and upload cost per frame.
compile: compiles the runtime and the generated game code for the SH-4 (no
         link yet) and reports their size.
tools:   builds the host measuring tools (platform/dreamcast/tools).
measure: runs romuse over input scripts (default: every scripts/inputs/*.txt
         except the test-mode ones) and summarises each with
         scripts/rom_usage.py; --sound also logs the sound commands.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def run(command, **kw):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kw)


def tool_path(build, name):
    exe = name + (".exe" if os.name == "nt" else "")
    for p in (build / exe, build / "Release" / exe):
        if p.is_file():
            return p
    sys.exit(f"build_dreamcast: {name} was not built in {build}")


def msys_path(p):
    """A Windows path as DreamSDK's shell names it: C:/x/y is /c/x/y."""
    p = Path(p).resolve().as_posix()
    return f"/{p[0].lower()}{p[2:]}" if len(p) > 1 and p[1] == ":" else p


# KallistiOS for this build: 2.2.1 (GitHub 857e4e69, 2025-08-30), the version
# DreamSDK R4's prebuilt toolchains were made with, in extern/kos-dc
# (git-ignored), built with -m4-single (a 64-bit double). DreamSDK's own KOS
# can be newer than its toolchain: with KOS master d458073c, every C++ program
# using libstdc++'s exception support stopped at startup on a mutex assertion
# (KOS's own cpp/concurrency example too).
KOS_DIR = ROOT / "extern" / "kos-dc"
KOS_COMMIT = "857e4e69"
KOS_PACKAGE = "opt/dreamsdk/packages/kallisti-offline-src.7z"  # under DreamSDK, the same KOS


def shell(args, command):
    """command in a shell with KOS's environment (extern/kos-dc). On Windows,
    DreamSDK's own login shell (its MSYS, its toolchain; a plain bash -c from
    Git Bash mixes two MSYS runtimes and hangs)."""
    env_kos = (f'source {msys_path(KOS_DIR)}/environ.sh 2>/dev/null; '
               'export PATH="$KOS_BASE/utils/build_wrappers:$PATH"; ')
    if os.name == "nt":
        bash = Path(args.dreamsdk) / "usr" / "bin" / "bash.exe"
        if not bash.is_file():
            sys.exit(f"build_dreamcast: DreamSDK not found at {args.dreamsdk} (--dreamsdk)")
        run([bash, "-lc", env_kos + command], env=dict(os.environ, MSYSTEM="MSYS", CHERE_INVOKING="1"))
    else:
        run(["bash", "-c", env_kos + command])


def setup_kos(args):
    """extern/kos-dc: KOS 2.2.1 unpacked (from DreamSDK's offline package, or
    cloned at KOS_COMMIT), an environ.sh for it, and its kernel and addons
    built. Its PC-side helper programs come from DreamSDK's KOS, already
    built (they do not depend on the KOS version)."""
    if (KOS_DIR / "lib" / "dreamcast" / "libkallisti.a").is_file():
        return
    if not (KOS_DIR / "Makefile").is_file():
        package = Path(args.dreamsdk) / KOS_PACKAGE
        if os.name == "nt" and package.is_file():
            seven = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "7-Zip" / "7z.exe"
            run([seven, "x", "-y", f"-o{KOS_DIR}", package])
        else:
            run(["git", "clone", "https://github.com/KallistiOS/KallistiOS.git", KOS_DIR])
            run(["git", "-C", KOS_DIR, "checkout", KOS_COMMIT])
    sample = (KOS_DIR / "doc" / "environ.sh.sample").read_text()
    cc_base = "/opt/toolchains/dc/sh-elf" if os.name == "nt" else os.environ.get("KOS_CC_BASE", "/opt/toolchains/dc/sh-elf")
    lines = []
    for line in sample.splitlines():
        if line.startswith("export KOS_BASE="):
            line = f'export KOS_BASE="{msys_path(KOS_DIR)}"'
        elif line.startswith("export KOS_CC_BASE="):
            line = f'export KOS_CC_BASE="{cc_base}"'
        elif line.startswith("export KOS_SH4_PRECISION="):
            line = 'export KOS_SH4_PRECISION="-m4-single"'
        lines.append(line)
    (KOS_DIR / "environ.sh").write_text("\n".join(lines) + "\n", newline="\n")
    if os.name == "nt":
        built = Path(args.dreamsdk) / "opt" / "toolchains" / "dc" / "kos" / "utils"
        for exe in built.rglob("*.exe"):
            dest = KOS_DIR / "utils" / exe.relative_to(built)
            if dest.parent.is_dir() and not dest.exists():
                dest.write_bytes(exe.read_bytes())
        shell(args, "make -C $KOS_BASE/kernel -j4 && make -C $KOS_BASE/addons -j4")
    else:
        shell(args, "make -C $KOS_BASE -j4")
    if not (KOS_DIR / "lib" / "dreamcast" / "libkallisti.a").is_file():
        sys.exit("build_dreamcast: KOS did not build (extern/kos-dc)")


def kos_make(args, target):
    """make in platform/dreamcast, with extern/kos-dc's KOS."""
    setup_kos(args)
    out = (ROOT / args.host_build_dir).resolve() / "dreamcast"
    out.mkdir(parents=True, exist_ok=True)
    fix_dependency_files(out)
    shell(args, f"make -C {msys_path(HERE)} OUT={msys_path(out)} {target}")
    return out


def fix_dependency_files(out):
    """The Makefile's FIXDEP turns the compiler's C:/ paths in a .d file into
    /c/ ones (make reads a drive colon as a rule's). A compile that fails
    still writes its .d, without FIXDEP: make then never matches that
    object's dependencies, and a changed header (game/inputs.h, so a
    --sample build) did not rebuild it. Fixed here before every make."""
    for dep in out.rglob("*.d"):
        text = dep.read_text(errors="replace")
        fixed = re.sub(r"\b([A-Za-z]):/", lambda m: f"/{m.group(1).lower()}/", text)
        if fixed != text:
            dep.write_text(fixed, newline="\n")


# Test programs: the markers that end a run (the first is a pass), the
# longest a run may take in Flycast, and the line its report starts at.
TESTS = {
    "selftest": (("SELFTEST PASS", "SELFTEST FAIL"), 30, "Daytona USA recomp"),
    "videotest": (("VIDEOTEST DONE",), 60, "VIDEOTEST"),
    "game": (("GAME DONE", "GAME FAILED"), 3600, "GAME"),  # a 6,000-frame race at ~4 frames/s in Flycast
}


def gen_dir(args):
    """The host recompile's generated code for Revision A."""
    gen = (ROOT / args.host_build_dir).resolve() / "gen"
    if not (gen / "daytona" / "gen_table.cpp").is_file():
        sys.exit(f"build_dreamcast: no generated code in {gen / 'daytona'}; run "
                 "scripts/recompile.py --set daytona --build-dir build-daytona first")
    return gen


# Input scripts tools/soundusage plays to find the samples the game uses (the
# Long course's own come from reva_long).
SOUND_SCRIPTS = ["scripts/inputs/race_basic.txt", "scripts/inputs/attract_long.txt",
                 "scripts/inputs/race_to_end.txt", "scripts/inputs/time_attack.txt",
                 "scripts/inputs/test_mode.txt", "platform/dreamcast/scripts/inputs/reva_long.txt",
                 "scripts/inputs/course_advanced.txt", "scripts/inputs/course_expert.txt"]


def sound_pack(args, host, nvram):
    """The sound pack (scripts/sound_pack.py) from the samples the game plays
    (tools/soundusage, run once: it plays the scripts on the desktop)."""
    out = host / "dreamcast"
    out.mkdir(parents=True, exist_ok=True)
    usage, pack = out / "sound_usage.txt", out / "sound.pak"
    images = host / "rom_cache" / "daytona"
    if not usage.is_file():
        tools = build_tools(args)
        exe = next((p for p in (tools / "Release" / "soundusage.exe", tools / "soundusage") if p.is_file()), None)
        if not exe:
            sys.exit("build_dreamcast: no soundusage tool")
        run([exe, images, usage, "--nvram", nvram, *[ROOT / s for s in SOUND_SCRIPTS]])
    if not pack.is_file() or pack.stat().st_mtime < usage.stat().st_mtime:
        run([sys.executable, HERE / "scripts" / "sound_pack.py", images, usage, pack])
    return pack


def run_test(args, name, markers, timeout, start):
    game = ""
    if name == "game":
        host = (ROOT / args.host_build_dir).resolve()
        nvram = Path(args.nvram) if args.nvram else Path(os.environ.get("APPDATA", "~")) / "daytona-recomp" / "daytona"
        if not (nvram / "ioboard_eeprom.bin").is_file():
            sys.exit(f"build_dreamcast: no saved settings in {nvram}: set a single cabinet in test mode with "
                     "build-daytona's daytona once (--nvram DIR)")
        # The i960 code with fewer lockstep checks and the TGP code with its
        # count in a local (scripts/fast_gen.py; it rewrites only the files
        # whose output changes).
        fast = host / "dreamcast" / "gen_fast" / args.set
        fast_tgp = host / "dreamcast" / "gen_fast" / f"{args.set}_tgp"
        run([sys.executable, HERE / "scripts" / "fast_gen.py", host / "gen" / args.set, fast])
        run([sys.executable, HERE / "scripts" / "fast_gen.py", host / "gen" / f"{args.set}_tgp", fast_tgp])
        pack = sound_pack(args, host, nvram)
        game = (f"-j{args.jobs} GEN={msys_path(host / 'gen')} GEN_I960={msys_path(fast)} GEN_TGP={msys_path(fast_tgp)} "
                f"ROMS={msys_path(host / 'rom_cache' / 'daytona')} NVRAM={msys_path(nvram)} "
                f"SOUND_PACK={msys_path(pack)} NATIVE_GEO={int(args.native_geo)} ")
        # The recorded input script, compiled in (game/inputs.h; empty: the
        # pad). Written only when it changes, so make rebuilds only then.
        text = (ROOT / args.inputs).read_text() if args.inputs else ""
        header = host / "dreamcast" / "game" / "inputs.h"
        content = ("// Written by build_dreamcast.py: the recorded input script (--inputs), or none,\n"
                   "// and whether to sample the game thread's PC (--sample).\n"
                   f"static const char kInputs[] = R\"INPUTS({text})INPUTS\";\n"
                   f"static const bool kSample = {'true' if args.sample else 'false'};\n"
                   f"static const int kDrawEvery = {args.draw_every};\n")
        header.parent.mkdir(parents=True, exist_ok=True)
        if not header.is_file() or header.read_text() != content:
            header.write_text(content, newline="\n")
        # --native-geo, for the Makefile's geo.o rule (rewritten only when it
        # changes, so make rebuilds geo.o only then).
        stamp = host / "dreamcast" / "game" / "native_geo.txt"
        setting = f"{int(args.native_geo)}\n"
        if not stamp.is_file() or stamp.read_text() != setting:
            stamp.write_text(setting, newline="\n")
        gen_dir(args)
    out = kos_make(args, f"{game}{name} {name}.cdi")
    if args.no_run:
        return
    sys.path.insert(0, str(HERE / "scripts"))
    import flycast_run
    flycast = Path(args.flycast) if args.flycast else flycast_run.DEFAULT_FLYCAST
    if not flycast.is_file():
        sys.exit(f"build_dreamcast: no Flycast at {flycast} (--flycast)")
    # The game with the pad (no --inputs) runs until Flycast is closed.
    play = name == "game" and not args.inputs
    text, seen = flycast_run.run(flycast, out / f"{name}.cdi", out / "flycast", markers,
                                 4 * 3600 if play else timeout)
    print(text[text.find(start):] if start in text else text)
    if play and seen != markers[1]:
        print("build_dreamcast: game closed")
        return
    if seen != markers[0]:
        sys.exit(f"build_dreamcast: {name} " + ("failed" if seen else "did not finish in Flycast"))


def build_tools(args):
    host = (ROOT / args.host_build_dir).resolve()
    gen = host / "gen"
    if not (gen / args.set / "gen_table.cpp").is_file():
        sys.exit(f"build_dreamcast: no generated code in {gen / args.set}; run "
                 f"scripts/recompile.py --set {args.set} --build-dir {args.host_build_dir} first")
    build = host / "dreamcast-tools"
    # The desktop build's compiled runtime and game code are linked as they
    # are, when complete; otherwise the tools project compiles them itself.
    run(["cmake", "-S", HERE / "tools", "-B", build, f"-DM2_ROMSET={args.set}", f"-DDAYTONA_GEN_ROOT={gen}",
         f"-DDAYTONA_HOST_BUILD={host}"])
    run(["cmake", "--build", build, "--config", "Release", "--parallel"])
    return build


def measure(args):
    build = build_tools(args)
    romuse = tool_path(build, "romuse")
    host = (ROOT / args.host_build_dir).resolve()
    images = host / "rom_cache" / args.set
    out = host / "dreamcast-measure"
    out.mkdir(exist_ok=True)
    scripts = [Path(s) for s in args.scripts] or sorted(
        p for p in (ROOT / "scripts" / "inputs").glob("*.txt") if not p.name.startswith("test_"))
    for script in scripts:
        name = script.stem
        # each script's own length ("frames N"), unless --frames is given
        frames = args.frames or next((int(l.split()[1]) for l in script.read_text().splitlines()
                                      if l.startswith("frames ")), 6000)
        cmd = [romuse, images, frames, "--out", out / f"{name}.txt", "--inputs", script]
        if args.nvram:
            cmd += ["--nvram", args.nvram]
        run(cmd)
        run([sys.executable, HERE / "scripts" / "rom_usage.py", out / f"{name}.txt", "--window", args.window])
        if args.sound:
            run(cmd[:3] + ["--out", out / f"{name}.nosound.txt", "--inputs", script, "--sound-log", out / f"{name}.sound.txt"]
                + (["--nvram", args.nvram] if args.nvram else []))
            run([sys.executable, HERE / "scripts" / "rom_usage.py", "--sound", out / f"{name}.sound.txt"])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["kos", *TESTS, "compile", "tools", "measure"])
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("scripts", nargs="*", help="input scripts for measure")
    ap.add_argument("--set", default="daytona", choices=["daytona"], help="Revision A only")
    ap.add_argument("--host-build-dir", default="build-daytona")
    ap.add_argument("--nvram", help="settings EEPROM and backup RAM to start from (the app's data folder)")
    ap.add_argument("--frames", type=int, default=0, help="default: each script's own frames line")
    ap.add_argument("--window", type=int, default=600, help="frames, for the peak-window figure")
    ap.add_argument("--sound", action="store_true")
    ap.add_argument("--dreamsdk", default="C:/DreamSDK", help="DreamSDK install (Windows)")
    ap.add_argument("--flycast", default=os.environ.get("FLYCAST"), help="flycast.exe (default: $FLYCAST, "
                    "or Downloads/flycast-win64-2.7)")
    ap.add_argument("--no-run", action="store_true", help="build only")
    ap.add_argument("--draw-every", type=int, default=2, choices=[1, 2, 3, 4],
                    help="game: frames per picture (1: every frame; default 2, the minimum wanted)")
    ap.add_argument("--sample", action="store_true",
                    help="game: sample the game thread's PC every KOS timer tick (about 10 ms) (scripts/pc_profile.py reads them)")
    ap.add_argument("--native-geo", action=argparse.BooleanOptionalAction, default=True,
                    help="game: the geometrizer's transforms by the SH-4's ftrv (picture only, not bit-exact; "
                         "default on, --no-native-geo for the desktop's arithmetic)")
    ap.add_argument("--inputs", help="game: a recorded input script (scripts/inputs) played instead of the pad")
    args = ap.parse_args(argv)
    if args.command == "kos":
        setup_kos(args)
    elif args.command == "compile":
        kos_make(args, f"-j{args.jobs} GEN={msys_path(gen_dir(args))} game-sizes")
    elif args.command in TESTS:
        run_test(args, args.command, *TESTS[args.command])
    elif args.command == "tools":
        build_tools(args)
    else:
        measure(args)


if __name__ == "__main__":
    main()
