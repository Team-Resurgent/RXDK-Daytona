#!/usr/bin/env python3
"""Summarise tools/romuse output: how much of each ROM region the game read,
over a whole run, over chosen frame ranges, and at most in any window of a
given length (the Dreamcast port's memory budget). Also decodes a
--sound-log: which music sequences the game starts, and how much of the rest
of its sound traffic is direct note and controller messages.

  rom_usage.py USAGE.txt [--range A:B ...] [--window FRAMES]
  rom_usage.py --union USAGE.txt... [--from FRAME]
  rom_usage.py --sound SOUND.txt

Ranges are in frames, end exclusive, rounded out to whole blocks. --union:
the pages read in any of the runs (from frame FRAME on in each), per region.
"""

import argparse
import collections

PAGE = 4096


def load(path):
    regions, pages = [], collections.defaultdict(dict)
    with open(path) as f:
        for line in f:
            w = line.split()
            if w[0] == "frames":
                frames, block, blocks = int(w[1]), int(w[3]), int(w[5])
            elif w[0] == "region":
                regions.append((w[1], int(w[2])))
            elif w[0] == "page":
                pages[int(w[1])][int(w[2])] = int(w[3], 16)
    return frames, block, blocks, regions, pages


def mb(n_pages):
    return n_pages * PAGE / 1048576


def read_in(pages, mask):
    return {ri: sum(1 for bits in p.values() if bits & mask) for ri, p in pages.items()}


def report(title, regions, counts):
    total = sum(counts.get(ri, 0) for ri in range(len(regions)))
    cells = "  ".join(f"{name} {mb(counts.get(ri, 0)):6.2f}" for ri, (name, _) in enumerate(regions))
    print(f"{title:<22} {cells}  | total {mb(total):6.2f} MB")


def usage(args):
    frames, block, blocks, regions, pages = load(args.usage)
    print(f"{args.usage}: {frames} frames, blocks of {block}; MB read per region")
    print("sizes".ljust(22), "  ".join(f"{name} {size / 1048576:6.2f}" for name, size in regions))
    report("whole run", regions, read_in(pages, (1 << blocks) - 1))
    for r in args.range:
        a, b = (int(x) for x in r.split(":"))
        lo, hi = a // block, min(blocks, -(-b // block))
        report(f"frames {a}-{b}", regions, read_in(pages, ((1 << hi) - 1) & ~((1 << lo) - 1)))
    if args.window:
        n = max(1, -(-args.window // block))
        best, best_at = None, 0
        for lo in range(0, max(1, blocks - n + 1)):
            counts = read_in(pages, ((1 << n) - 1) << lo)
            t = sum(counts.values())
            if best is None or t > sum(best.values()):
                best, best_at = counts, lo
        report(f"peak {args.window}f @{best_at * block}", regions, best)


def sound(path):
    """The UART stream as the native sequencer reads it (status byte, then one
    or two data bytes; 0xf8 ignored, 0xff reset)."""
    kinds = collections.Counter()
    per_channel = collections.defaultdict(collections.Counter)
    starts = []
    frames_with = 0
    status, data = 0, []
    with open(path) as f:
        for line in f:
            w = line.split()
            frame, frames_with = int(w[0]), frames_with + 1
            for v in (int(x, 16) for x in w[1:]):
                if v == 0xF8:
                    continue
                if v == 0xFF:
                    kinds["reset"] += 1
                    continue
                if v & 0x80:
                    status, data = v, []
                    continue
                if not status:
                    kinds["data without status"] += 1
                    continue
                data.append(v)
                if len(data) == (1 if (status & 0xE0) == 0xC0 else 2):
                    kind = {0x80: "note off", 0x90: "note on", 0xA0: "start sequence", 0xB0: "controller",
                            0xC0: "program", 0xD0: "d0", 0xE0: "pitch bend", 0xF0: "f0"}[status & 0xF0]
                    if kind == "note on" and data[1] == 0:
                        kind = "note off"
                    kinds[kind] += 1
                    per_channel[status & 15][kind] += 1
                    if kind == "start sequence":
                        starts.append((frame, status & 15, data[0], data[1]))
                    data = []
    print(f"{path}: {frames_with} frames sent sound commands")
    for k, n in kinds.most_common():
        print(f"  {k:<16} {n}")
    print("  per channel:")
    for ch in sorted(per_channel):
        print(f"    {ch:2}: " + ", ".join(f"{k} {n}" for k, n in per_channel[ch].most_common()))
    print(f"  sequences started ({len(starts)}; frame channel bank index):")
    for s in starts:
        print("    %6d  ch %2d  bank %3d  index %3d" % s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("usage", nargs="?")
    ap.add_argument("--range", action="append", default=[])
    ap.add_argument("--window", type=int, default=0)
    ap.add_argument("--sound")
    ap.add_argument("--union", nargs="+")
    ap.add_argument("--from", dest="start", type=int, default=0)
    args = ap.parse_args()
    if args.sound:
        sound(args.sound)
    if args.union:
        union(args.union, args.start)
    if args.usage:
        usage(args)


def union(paths, start):
    """Pages read in any of the runs, from frame `start` on in each."""
    regions, read = None, collections.defaultdict(set)
    for path in paths:
        frames, block, blocks, regs, pages = load(path)
        regions = regions or regs
        mask = ((1 << blocks) - 1) & ~((1 << (start // block)) - 1)
        for ri, p in pages.items():
            read[ri] |= {pi for pi, bits in p.items() if bits & mask}
    report(f"union of {len(paths)}, from {start}", regions, {ri: len(s) for ri, s in read.items()})


if __name__ == "__main__":
    main()
