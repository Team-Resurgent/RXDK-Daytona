#!/usr/bin/env python3
"""Carry recompiler seeds from one ROM set's program to another's, by code.

  seed_map.py FROM_PROGRAM.bin TO_PROGRAM.bin FROM_SEEDS.txt [--hooks]

The two Daytona USA programs (daytona93 and daytona, Revision A) are mostly
the same code at shifted addresses. For each seed (an entry point in FROM),
this finds the place in TO whose next instructions are the same, ignoring
what moves between versions: branch displacements and the 32-bit address
words of MEMB instructions. A seed maps when exactly one place matches (16
instructions, or 32 to settle a tie; the nearest to the shift of the seeds
already mapped breaks a remaining tie). A seed with no exact match (the
revision changed the code after it: an inserted call, say) is looked for
near where its neighbours moved, scored by how much of the next 24
instructions lines up (difflib, insertions allowed): kept at 75% or more
with the first instruction the same. Prints the mapped seeds (the same
base: the program ROM is also seen at 0x200000) and, on stderr, the ones
that did not map. --hooks maps "ADDRESS name" lines instead.

Addresses only (rules.md rule 7): reads the user's own images, writes no ROM
bytes.
"""

import bisect
import difflib
import struct
import sys

ROM = 0x40000  # the program ROMs: 2 x 128 KB, interleaved


def words(path):
    data = open(path, "rb").read()[:ROM]
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


def masked(w, i):
    """The instruction at word i, its version-independent part, and its length in words."""
    op = w[i] >> 24
    if op < 0x20:  # CTRL: 24-bit displacement
        return w[i] & 0xFF000003, 1
    if op < 0x40:  # COBR: 13-bit displacement
        return w[i] & 0xFFFFE003, 1
    if op < 0x80:  # REG
        return w[i], 1
    if w[i] & 0x1000:  # MEMB
        mode = (w[i] >> 10) & 0xF
        if mode in (0x5, 0xC, 0xD, 0xE, 0xF):  # a 32-bit displacement word follows: an address, ignored
            return w[i], 2
        return w[i], 1
    return w[i], 1  # MEMA: a 12-bit offset (structure fields), kept


def signature(w, i, n):
    sig = []
    while len(sig) < n and i < len(w):
        m, size = masked(w, i)
        sig.append(m)
        i += size
    return tuple(sig)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, dst = words(sys.argv[1]), words(sys.argv[2])
    hooks = "--hooks" in sys.argv
    index = {}
    for n in (16, 32):
        index[n] = {}
        for i in range(len(dst)):
            index[n].setdefault(signature(dst, i, n), []).append(i)
    out, missed, shifts, fuzzy, anchors = [], [], [], [], []
    pending = []
    for line in open(sys.argv[3]):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        fields = text.split()
        addr = int(fields[0], 16)
        base, off = addr & ~(ROM - 1), addr & (ROM - 1)
        i = off // 4
        hits = []
        for n in (16, 32):
            hits = index[n].get(signature(src, i, n), [])
            if len(hits) <= 1:
                break
        if len(hits) > 1 and shifts:
            shift = sorted(shifts)[len(shifts) // 2]
            hits = [min(hits, key=lambda h: abs((h - i) * 4 - shift))]
        if len(hits) != 1:
            pending.append((addr, fields, len(hits)))
            continue
        shifts.append((hits[0] - i) * 4)
        out.append(("%08x" % (base + hits[0] * 4)) + ("" if not hooks else " " + " ".join(fields[1:])))
        anchors.append((off, hits[0] * 4))
    # Fuzzy pass: near the shift of the nearest exact matches, best alignment.
    anchors.sort()
    keys = [a for a, _ in anchors]
    for addr, fields, nhits in pending:
        base, off = addr & ~(ROM - 1), addr & (ROM - 1)
        k = bisect.bisect_left(keys, off)
        near = [anchors[j] for j in (k - 1, k) if 0 <= j < len(anchors)]
        guesses = {to - frm for frm, to in near} or {0}
        want = signature(src, off // 4, 24)
        best, best_at = 0.0, None
        for g in guesses:
            for at in range(max(0, off + g - 0x800), min(ROM, off + g + 0x800), 4):
                cand = signature(dst, at // 4, 24)
                if not cand or cand[0] != want[0]:
                    continue
                r = difflib.SequenceMatcher(None, want, cand, autojunk=False).ratio()
                if r > best or (r == best and best_at is not None and abs(at - off - g) < abs(best_at - off - g)):
                    best, best_at = r, at
        if best_at is None or best < 0.75:
            missed.append("%08x (%d exact matches, best alignment %.0f%%)" % (addr, nhits, best * 100))
            continue
        fuzzy.append("%08x -> %08x (%.0f%%)" % (addr, base + best_at, best * 100))
        out.append(("%08x" % (base + best_at)) + ("" if not hooks else " " + " ".join(fields[1:])))
    for line in sorted(set(out)):
        print(line)
    print("seed_map: %d mapped (%d by alignment), %d not" % (len(out), len(fuzzy), len(missed)), file=sys.stderr)
    for m in missed:
        print("  not mapped: " + m, file=sys.stderr)


if __name__ == "__main__":
    main()
