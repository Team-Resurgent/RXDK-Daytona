"""The Dreamcast's sound pack: the samples the game plays, as AICA ADPCM.

Sound RAM has 2 MB and the PCM ROMs 8; the samples tools/soundusage found
the game playing (its "rom bank sample" lines) are converted to the AICA's
4-bit ADPCM (KallistiOS's wav2adpcm encoder, ported) and packed with a table
the frontend (game/audio.h) loads them into sound RAM from, as needed.

    python platform/dreamcast/scripts/sound_pack.py IMAGES_DIR USAGE.txt OUT.pak

Layout (little-endian): "DSP1", u32 count, then per sample u8 rom, u8 bank,
u16 sample, u32 frames, u32 loop_start, u32 offset, u32 bytes; then the data
(each sample from its offset, 32-byte aligned). Sample decoding follows
src/runtime/native_sample_mixer.cpp (NativeSampleBank): 12-byte headers, the
address's bit 22 selects packed 12-bit samples, logical 0x100000-0x1fffff
the data bank.
"""
import struct
import sys
from pathlib import Path

WINDOW = 0x100000
STEP_TABLE = (230, 230, 230, 230, 307, 409, 512, 614)
MAX_FRAMES = 65534  # the AICA's 16-bit length (KOS clamps at 65534)


def clamp(x, lo, hi):
    return hi if x > hi else lo if x < lo else x


def ymz_step(step, state):
    history, step_size = state
    sign, delta = step & 8, step & 7
    diff = clamp(((1 + (delta << 1)) * step_size) >> 3, 0, 32767)
    nstep = (STEP_TABLE[delta] * step_size) >> 8
    newval = history - diff if sign else history + diff
    state[0] = clamp(newval, -32768, 32767)
    state[1] = clamp(nstep, 127, 24576)


def pcm_to_adpcm(samples):
    """wav2adpcm's pcm2adpcm: 16-bit samples to 4-bit ADPCM, low nibble first."""
    state = [0, 127]
    out = bytearray((len(samples) + 1) // 2)
    for i, s in enumerate(samples):
        step = (s & -8) - state[0]
        code = clamp((abs(step) << 16) // (state[1] << 14), 0, 7)
        if step < 0:
            code |= 8
        if i & 1:
            out[i >> 1] |= code << 4
        else:
            out[i >> 1] = code
        ymz_step(code, state)
    return out


def decode(rom, bank, index):
    h = rom[index * 12:index * 12 + 12]
    address = h[0] << 16 | h[1] << 8 | h[2]
    start, packed = address & 0x3FFFFF, bool(address & 0x400000)
    loop_start = h[3] << 8 | h[4]
    frames = 65536 - (h[5] << 8 | h[6])

    def byte(logical):
        return rom[logical if logical < WINDOW else bank * WINDOW + logical - WINDOW]

    out = []
    for f in range(frames):
        if not packed:
            raw = byte(start + f)
            out.append((raw - 256 if raw >= 128 else raw) << 8)
        else:
            a = start + (f // 2) * 3
            raw = (byte(a + 2) << 4 | byte(a + 1) >> 4) if f & 1 else (byte(a) << 4 | (byte(a + 1) & 15))
            out.append((raw - 4096 if raw >= 2048 else raw) << 4)
    return out, loop_start


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    images, usage, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    roms = [(images / f"pcm{r + 1}.bin").read_bytes() for r in (0, 1)]
    used = [tuple(map(int, line.split())) for line in usage.read_text().splitlines() if line.strip()]
    table, data = [], bytearray()
    for rom, bank, index in used:
        samples, loop_start = decode(roms[rom], bank, index)
        samples = samples[:MAX_FRAMES]
        if len(samples) & 1:
            samples.append(samples[-1])  # whole bytes
        adpcm = pcm_to_adpcm(samples)
        data += bytes((-len(data)) % 32)
        table.append((rom, bank, index, len(samples), min(loop_start, len(samples) - 1), len(data), len(adpcm)))
        data += adpcm
    header = b"DSP1" + struct.pack("<I", len(table))
    entry_bytes = len(table) * 20
    base = (len(header) + entry_bytes + 31) // 32 * 32
    blob = bytearray(header)
    for rom, bank, index, frames, loop, offset, size in table:
        blob += struct.pack("<BBHIIII", rom, bank, index, frames, loop, base + offset, size)
    blob += bytes(base - len(blob)) + data
    out.write_bytes(blob)
    print(f"sound_pack: {len(table)} samples, {len(data) / 1048576:.2f} MB of ADPCM -> {out}")


if __name__ == "__main__":
    main()
