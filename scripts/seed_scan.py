#!/usr/bin/env python3
"""Find i960 entry points the MAME harvest never reached, by static analysis
of the user's ROM set, and list them as seeds.

  seed_scan.py [--set daytona93|daytona] [--build-dir build] [--config Release] [--append]

The game calls code it has only as a pointer: state machines store the next
state's address (lda 0x2266f8,r5; st r5,0xc(r3)), jump tables are indexed
(ld 0x18cc[g0*4]; callx, lda 0x225010[r3*4]; ld; callx), and object records
in the program and data ROMs carry handler addresses. A state the harvest did
not visit stops the game with "no recompiled code at X". Candidates:

  - lda constants in reachable code;
  - entries of tables read with a [reg*4] index in reachable code;
  - every aligned word of the program ROM that points into code;
  - words of the main data ROM that point at a function start (after a ret
    or an unconditional branch; the data ROM is large, so this is stricter).

A candidate is kept only if it decodes as executable code up to a ret or
branch (i960dis: no undecodable, !noexec or !quirk word first). Repeats to a
fixed point, recompiling into a scratch directory each round. A false
positive only costs unused native code; a miss stops the game.

Needs the ROM set imported (scripts/recompile.py has run). Prints the new
addresses; --append adds them to seeds/<set>.txt. Then run
scripts/recompile.py.
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RET = 0x0A000000
ENDS = {"ret", "b", "bx"}
INSN = re.compile(r"^L_([0-9a-f]{8}): // [0-9a-f]{8}: (\S+)\s+(0x[0-9a-f]+)?(\[\w+\*4\])?")


def tool(build, config, name):
    exe = name + (".exe" if os.name == "nt" else "")
    for p in (os.path.join(build, exe), os.path.join(build, config, exe)):
        if os.path.exists(p):
            return p
    sys.exit("seed_scan: %s not built (run scripts/setup.py)" % name)


def words(data):
    return {v for (v,) in struct.iter_unpack("<I", data[: len(data) & ~3])}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", default="daytona93", choices=["daytona93", "daytona"])
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--config", default="Release")
    ap.add_argument("--append", action="store_true", help="add the new seeds to seeds/<set>.txt")
    args = ap.parse_args()
    build = os.path.join(ROOT, args.build_dir)
    cache = os.path.join(build, "rom_cache", args.set)
    seeds_path = os.path.join(ROOT, "seeds", args.set + ".txt")
    prog_path = os.path.join(cache, "program.bin")
    if not os.path.exists(prog_path):
        sys.exit("seed_scan: no imported ROM set in %s (run scripts/recompile.py)" % cache)
    recomp, dis = tool(build, args.config, "m2recomp"), tool(build, args.config, "i960dis")

    img = open(prog_path, "rb").read()

    # Program ROM at 0, its upper 128 KiB mirrored at 0x220000 (as m2recomp).
    def off(a):
        if 0x800 <= a < len(img):
            return a
        if 0x220000 <= a < 0x240000:
            return a - 0x200000
        return None

    def word(a):
        o = off(a)
        return None if o is None or o + 4 > len(img) else struct.unpack_from("<I", img, o)[0]

    def plausible(a):
        return a % 4 == 0 and off(a) is not None

    def fstart(a):
        w = word(a - 4)
        return w is not None and (w == RET or (w >> 24) == 0x08)

    decoded = {}

    def is_code(a):
        if a not in decoded:
            out = subprocess.run([dis, "--start", hex(off(a)), "--count", "400", prog_path],
                                 capture_output=True, text=True).stdout
            decoded[a] = False
            for line in out.splitlines():
                parts = line.split()
                if len(parts) < 3 or "!noexec" in line or "!quirk" in line:
                    break
                mn = next((p for p in parts[2:] if not re.fullmatch(r"[0-9a-f]{8}", p)), "?")
                if mn == "?":
                    break
                if mn in ENDS:
                    decoded[a] = True
                    break
        return decoded[a]

    rom_ptrs = {v for v in words(img) if plausible(v)}
    data = open(os.path.join(cache, "main_data.bin"), "rb").read()
    data_ptrs = {v for v in words(data) if plausible(v) and fstart(v)}
    del data

    seeds = [int(l, 16) for l in open(seeds_path) if l.strip() and not l.startswith("#")]
    found = []
    scratch = os.path.join(build, "seed_scan")
    for rnd in range(1, 50):
        shutil.rmtree(scratch, ignore_errors=True)
        os.makedirs(scratch)
        sf = os.path.join(scratch, "seeds.txt")
        with open(sf, "w") as f:
            f.writelines("%08x\n" % a for a in seeds + found)
        gen = os.path.join(scratch, "gen")
        os.makedirs(gen)
        r = subprocess.run([recomp, prog_path, gen, "--seeds", sf], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr + "\nseed_scan: recompile failed in round %d" % rnd)

        reached, targets, tables = set(), set(rom_ptrs | data_ptrs), set()
        for path in glob.glob(os.path.join(gen, "chunk_*.cpp")):
            for line in open(path):
                m = INSN.match(line)
                if not m:
                    continue
                reached.add(int(m.group(1), 16))
                if m.group(3) and m.group(2) in ("ld", "lda"):
                    if m.group(4):
                        tables.add(int(m.group(3), 16))
                    elif m.group(2) == "lda":
                        targets.add(int(m.group(3), 16))
        for t in tables:
            for j in range(256):  # entries run until one is not a code address
                v = word(t + 4 * j)
                if v is None or not plausible(v) or (t + 4 * j) in reached or not is_code(v):
                    break
                targets.add(v)

        new = sorted(a for a in targets if a not in reached and plausible(a) and is_code(a))
        print("round %d: %d instructions reachable, %d new entry points" % (rnd, len(reached), len(new)))
        if not new:
            break
        found += new
    shutil.rmtree(scratch, ignore_errors=True)

    found = sorted(set(found))
    for a in found:
        print("%08x" % a)
    print("seed_scan: %d new seeds" % len(found), file=sys.stderr)
    if found and args.append:
        with open(seeds_path, "a") as f:
            f.write("# scripts/seed_scan.py\n")
            f.writelines("%08x\n" % a for a in found)
        print("seed_scan: appended to %s; now run scripts/recompile.py" % seeds_path, file=sys.stderr)


if __name__ == "__main__":
    main()
