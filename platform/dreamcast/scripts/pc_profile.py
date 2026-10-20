"""Name the game thread's PC samples (build_dreamcast.py game --sample).

The frontend prints "SAMPLE address count" lines, 128-byte buckets of the
program; this sums them by function from game.elf's symbols (sh-elf-nm) and
prints the busiest, with their share of the samples.

    python platform/dreamcast/scripts/pc_profile.py [SERIAL] [--elf ELF] [--top N] [--from FRAME]
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NM = Path("C:/DreamSDK/opt/toolchains/dc/sh-elf/bin/sh-elf-nm.exe")


def symbols(elf):
    out = subprocess.run([str(NM), "-C", "-n", str(elf)], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "TtWw":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial", nargs="?", default=ROOT / "build-daytona/dreamcast/flycast/serial.txt")
    ap.add_argument("--elf", default=ROOT / "build-daytona/dreamcast/game.elf")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--from", dest="first", type=int, default=0, help="only reports from this frame on")
    args = ap.parse_args()

    addrs, names = symbols(args.elf)
    by_name, total, shown, frame = collections.Counter(), 0, 0, 0
    callers = collections.Counter()
    coarse = collections.Counter()  # 16 KB bucket -> samples (every sample, every other report)
    coarse_total = 0
    for line in Path(args.serial).read_text(errors="replace").splitlines():
        m = re.match(r"SAMPLE frame (\d+): (\d+) samples", line)
        if m:
            frame = int(m.group(1))
            if frame >= args.first:
                total += int(m.group(2))
            continue
        m = re.match(r"CALLER ([0-9a-f]{8}) (\d+)", line)
        if m and frame >= args.first:
            i = bisect.bisect_right(addrs, int(m.group(1), 16) + 256) - 1  # (the middle of a 512-byte bucket)
            callers[names[i] if i >= 0 else "?"] += int(m.group(2))
            continue
        m = re.match(r"COARSE frame (\d+): (\d+) samples", line)
        if m:
            frame = int(m.group(1))
            if frame >= args.first:
                coarse_total += int(m.group(2))
            continue
        m = re.match(r"COARSE ([0-9a-f]{8}) (\d+)", line)
        if m and frame >= args.first:
            coarse[int(m.group(1), 16)] += int(m.group(2))
            continue
        m = re.match(r"SAMPLE ([0-9a-f]{8}) (\d+)", line)
        if m and frame >= args.first:
            addr, count = int(m.group(1), 16), int(m.group(2))
            # A bucket that straddles two functions is named for both
            # ("first | second"): the samples could be in either.
            i = bisect.bisect_right(addrs, addr) - 1
            j = bisect.bisect_right(addrs, addr + 127) - 1
            name = names[i] if i >= 0 else "?"
            if j != i and j >= 0:
                name = f"{name[:70]} | {names[j][:70]}"
            by_name[name] += count
            shown += count
    if not total:
        sys.exit("pc_profile: no SAMPLE lines (build with --sample)")
    print(f"{total} samples, {shown} in the printed buckets ({100 * shown / total:.1f}%)")
    for name, count in by_name.most_common(args.top):
        print(f"{100 * count / total:6.2f}%  {count:7d}  {name[:150]}")
    if coarse:
        # Every sample by 16 KB of the program, summed by what the bucket's
        # functions belong to: the generated code (gen::, rt::tgpgen,
        # rt::sndgen), the runtime (rt::), the frontend (dc::), the rest
        # (KOS, libc, libstdc++).
        groups = collections.Counter()
        for addr, count in coarse.items():
            lo = bisect.bisect_right(addrs, addr) - 1
            hi = bisect.bisect_right(addrs, addr + 0x3fff)
            kinds = collections.Counter()
            for k in range(max(lo, 0), hi):
                n = names[k]
                kind = ("generated i960" if n.startswith("gen::") else
                        "generated TGP/68000" if n.startswith(("rt::tgpgen", "rt::sndgen", "tgpgen", "sndgen")) else
                        "runtime" if n.startswith("rt::") or "rt::" in n[:40] else
                        "frontend" if n.startswith("dc::") or "(anonymous namespace)" in n[:25] else "other")
                kinds[kind] += 1
            groups[kinds.most_common(1)[0][0] if kinds else "?"] += count
        print(f"all {coarse_total} samples of the COARSE reports by 16 KB bucket, by the bucket's main kind of function:")
        for kind, count in groups.most_common():
            print(f"{100 * count / coarse_total:6.2f}%  {count:7d}  {kind}")
        print("busiest 16 KB buckets (first function in each):")
        for addr, count in coarse.most_common(15):
            i = bisect.bisect_right(addrs, addr) - 1
            print(f"{100 * count / coarse_total:6.2f}%  {addr:08x}  {names[i][:100] if i >= 0 else '?'}")
    if callers:
        print("memcpy/memset called from:")
        for name, count in callers.most_common(15):
            print(f"{100 * count / total:6.2f}%  {count:7d}  {name[:150]}")


if __name__ == "__main__":
    main()
