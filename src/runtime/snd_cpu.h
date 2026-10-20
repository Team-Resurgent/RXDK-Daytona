// The sound board's 68000 as native state (design doc, sound): registers,
// flags and the board's address map, for code recompiled from the sound
// program by tools/m2sndrecomp. Nothing here decodes or interprets
// instructions; the generated code calls these helpers for memory access,
// flag arithmetic, SR changes and interrupt entry, all inline.
//
// The flag arithmetic follows the 68000 programmer's reference; the lockstep
// check (tools/m2sndcheck) holds it to MAME's m68000 core instruction by
// instruction.
#pragma once

#include "runtime/cpu.h" // rt::Fatal

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace snd {

// Device side of the sound board's map: the UART, two MultiPCMs with their
// bank registers and the YM3438. Addresses are the ones on the 68000 bus:
// 8-bit devices sit on the odd byte (0xc20001, 0xd00001 ...), the 16-bit
// registers (0xc40012, 0xc50000, 0xc70000) at their even address.
class Devices {
public:
    virtual ~Devices() = default;
    virtual uint8_t read(uint32_t addr) = 0;
    virtual void write(uint32_t addr, uint16_t data) = 0;
};

struct Cpu68k {
    uint32_t d[8] = {}, a[8] = {};
    uint32_t other_sp = 0;          // USP while supervisor, SSP while user
    uint32_t pc = 0;
    uint32_t x = 0, n = 0, z = 0, v = 0, c = 0; // flags, each 0 or 1
    uint16_t sys = 0x2700;          // SR system byte: T, S, interrupt mask (flags in the variables above)
    int irq_line = 0;               // level asserted on IPL (autovectored)

    std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000, 0); // 0xf00000-0xf0ffff
    const uint8_t *rom = nullptr;   // 0x000000-0x03ffff, big-endian bytes; 0x080000-0x09ffff mirrors 0x020000
    Devices *dev = nullptr;

    // Reset: SSP and PC from the vector table, supervisor, mask 7.
    void reset();

    uint16_t sr() const { return uint16_t(sys | (x << 4) | (n << 3) | (z << 2) | (v << 1) | c); }
    void set_ccr(uint32_t v8) { x = (v8 >> 4) & 1; n = (v8 >> 3) & 1; z = (v8 >> 2) & 1; v = (v8 >> 1) & 1; c = v8 & 1; }
    void set_sr(uint32_t v16); // switches stacks when S changes

    int mask() const { return (sys >> 8) & 7; }
    // Autovectored interrupt entry: pushes PC and SR, raises the mask, jumps.
    void take_irq(int level);

    // Memory: 24-bit bus.
    uint8_t r8(uint32_t addr) {
        addr &= 0xffffff;
        if (addr >= 0xf00000 && addr < 0xf10000) return ram[addr - 0xf00000];
        if (addr < 0x40000) return rom[addr];
        if (addr >= 0x80000 && addr < 0xa0000) return rom[addr - 0x60000];
        return dev_r8(addr);
    }
    uint16_t r16(uint32_t addr) {
        addr &= 0xffffff;
        if (addr & 1) odd(addr);
        if (addr >= 0xf00000 && addr < 0xf10000) return uint16_t(ram[addr - 0xf00000] << 8 | ram[addr - 0xf00000 + 1]);
        if (addr < 0x40000) return uint16_t(rom[addr] << 8 | rom[addr + 1]);
        if (addr >= 0x80000 && addr < 0xa0000) return uint16_t(rom[addr - 0x60000] << 8 | rom[addr - 0x60000 + 1]);
        return dev_r16(addr);
    }
    uint32_t r32(uint32_t addr) { return uint32_t(r16(addr)) << 16 | r16(addr + 2); }
    void w8(uint32_t addr, uint32_t val) {
        addr &= 0xffffff;
        if (addr >= 0xf00000 && addr < 0xf10000) ram[addr - 0xf00000] = uint8_t(val);
        else dev_w(addr, uint16_t(addr & 1 ? (val & 0xff) : (val & 0xff) << 8), addr & 1 ? 0x00ff : 0xff00);
    }
    void w16(uint32_t addr, uint32_t val) {
        addr &= 0xffffff;
        if (addr & 1) odd(addr);
        if (addr >= 0xf00000 && addr < 0xf10000) {
            ram[addr - 0xf00000] = uint8_t(val >> 8);
            ram[addr - 0xf00000 + 1] = uint8_t(val);
        } else dev_w(addr, uint16_t(val), 0xffff);
    }
    void w32(uint32_t addr, uint32_t val) { w16(addr, val >> 16); w16(addr + 2, val); }

    void push16(uint32_t v16) { a[7] -= 2; w16(a[7], v16); }
    void push32(uint32_t v32) { a[7] -= 4; w32(a[7], v32); }
    uint16_t pop16() { const uint16_t v16 = r16(a[7]); a[7] += 2; return v16; }
    uint32_t pop32() { const uint32_t v32 = r32(a[7]); a[7] += 4; return v32; }

    [[noreturn]] void fatal(const std::string &what) const;

private:
    uint8_t dev_r8(uint32_t addr);
    uint16_t dev_r16(uint32_t addr);
    void dev_w(uint32_t addr, uint16_t data, uint16_t mem_mask);
    [[noreturn]] void odd(uint32_t addr) const;
};

// Flag arithmetic for recompiled code, by operand size in bytes.
template <unsigned S> constexpr uint32_t kMask = S == 1 ? 0xffu : S == 2 ? 0xffffu : 0xffffffffu;
template <unsigned S> constexpr uint32_t kMsb = S == 1 ? 0x80u : S == 2 ? 0x8000u : 0x80000000u;
template <unsigned S> inline uint32_t sext(uint32_t v) {
    if constexpr (S == 1) return uint32_t(int32_t(int8_t(v)));
    else if constexpr (S == 2) return uint32_t(int32_t(int16_t(v)));
    else return v;
}

template <unsigned S> inline void nz(Cpu68k &c, uint32_t r) {
    c.n = (r & kMsb<S>) ? 1 : 0;
    c.z = (r & kMask<S>) ? 0 : 1;
}
// move, and, or, eor, not, tst, clr, ext, moveq: N Z, V = C = 0
template <unsigned S> inline uint32_t logic(Cpu68k &c, uint32_t r) {
    r &= kMask<S>;
    nz<S>(c, r);
    c.v = c.c = 0;
    return r;
}
template <unsigned S> inline uint32_t add(Cpu68k &c, uint32_t s, uint32_t d) {
    s &= kMask<S>;
    d &= kMask<S>;
    const uint64_t wide = uint64_t(s) + d;
    const uint32_t r = uint32_t(wide) & kMask<S>;
    nz<S>(c, r);
    c.v = ((s ^ r) & (d ^ r) & kMsb<S>) ? 1 : 0;
    c.c = c.x = uint32_t(wide >> (S * 8)) & 1;
    return r;
}
template <unsigned S> inline uint32_t sub(Cpu68k &c, uint32_t s, uint32_t d) { // d - s
    s &= kMask<S>;
    d &= kMask<S>;
    const uint32_t r = (d - s) & kMask<S>;
    nz<S>(c, r);
    c.v = ((s ^ d) & (r ^ d) & kMsb<S>) ? 1 : 0;
    c.c = c.x = s > d ? 1 : 0;
    return r;
}
template <unsigned S> inline void cmp(Cpu68k &c, uint32_t s, uint32_t d) { // d - s, X unchanged
    const uint32_t x = c.x;
    sub<S>(c, s, d);
    c.x = x;
}
template <unsigned S> inline uint32_t addx(Cpu68k &c, uint32_t s, uint32_t d) {
    s &= kMask<S>;
    d &= kMask<S>;
    const uint64_t wide = uint64_t(s) + d + c.x;
    const uint32_t r = uint32_t(wide) & kMask<S>;
    c.n = (r & kMsb<S>) ? 1 : 0;
    if (r) c.z = 0; // Z cleared if nonzero, otherwise unchanged
    c.v = ((s ^ r) & (d ^ r) & kMsb<S>) ? 1 : 0;
    c.c = c.x = uint32_t(wide >> (S * 8)) & 1;
    return r;
}
template <unsigned S> inline uint32_t subx(Cpu68k &c, uint32_t s, uint32_t d) {
    s &= kMask<S>;
    d &= kMask<S>;
    const uint64_t wide = uint64_t(d) - s - c.x;
    const uint32_t r = uint32_t(wide) & kMask<S>;
    c.n = (r & kMsb<S>) ? 1 : 0;
    if (r) c.z = 0;
    c.v = ((s ^ d) & (r ^ d) & kMsb<S>) ? 1 : 0;
    c.c = c.x = (uint64_t(s) + c.x > d) ? 1 : 0;
    return r;
}
template <unsigned S> inline uint32_t neg(Cpu68k &c, uint32_t d) { return sub<S>(c, d, 0); }
template <unsigned S> inline uint32_t negx(Cpu68k &c, uint32_t d) { return subx<S>(c, d, 0); }

// Shifts and rotates, register count (already reduced mod 64) or 1.
template <unsigned S> inline uint32_t lsl(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint32_t r;
    if (n == 0) { c.c = 0; r = d; }
    else if (n <= B) { c.c = c.x = (d >> (B - n)) & 1; r = n == 32 ? 0 : (d << n) & kMask<S>; }
    else { c.c = c.x = 0; r = 0; }
    nz<S>(c, r);
    c.v = 0;
    return r;
}
template <unsigned S> inline uint32_t lsr(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint32_t r;
    if (n == 0) { c.c = 0; r = d; }
    else if (n <= B) { c.c = c.x = (d >> (n - 1)) & 1; r = n == 32 ? 0 : d >> n; }
    else { c.c = c.x = 0; r = 0; }
    nz<S>(c, r);
    c.v = 0;
    return r;
}
template <unsigned S> inline uint32_t asl(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint32_t r;
    if (n == 0) { c.c = 0; c.v = 0; r = d; }
    else if (n < B) {
        c.c = c.x = (d >> (B - n)) & 1;
        r = (d << n) & kMask<S>;
        // V: the sign bit changed at some point: the top n+1 bits were not all equal
        const uint64_t top = uint64_t(kMask<S>) & ~(uint64_t(kMask<S>) >> (n + 1));
        c.v = ((d & top) != 0 && (d & top) != top) ? 1 : 0;
    } else {
        c.c = c.x = n == B ? d & 1 : 0;
        r = 0;
        c.v = d != 0 ? 1 : 0;
    }
    nz<S>(c, r);
    return r;
}
template <unsigned S> inline uint32_t asr(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    const int32_t sd = int32_t(sext<S>(d));
    uint32_t r;
    if (n == 0) { c.c = 0; r = d; }
    else if (n < B) { c.c = c.x = uint32_t(sd >> (n - 1)) & 1; r = uint32_t(sd >> n) & kMask<S>; }
    else { c.c = c.x = sd < 0 ? 1 : 0; r = sd < 0 ? kMask<S> : 0; }
    nz<S>(c, r);
    c.v = 0;
    return r;
}
template <unsigned S> inline uint32_t rol(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint32_t r = d;
    if (n == 0) c.c = 0;
    else {
        const unsigned k = n % B;
        if (k) r = ((d << k) | (d >> (B - k))) & kMask<S>;
        c.c = r & 1;
    }
    nz<S>(c, r);
    c.v = 0;
    return r;
}
template <unsigned S> inline uint32_t ror(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint32_t r = d;
    if (n == 0) c.c = 0;
    else {
        const unsigned k = n % B;
        if (k) r = ((d >> k) | (d << (B - k))) & kMask<S>;
        c.c = (r & kMsb<S>) ? 1 : 0;
    }
    nz<S>(c, r);
    c.v = 0;
    return r;
}
template <unsigned S> inline uint32_t roxl(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint64_t w = (uint64_t(c.x) << B) | d; // B+1 bits: X above the operand
    const unsigned k = n % (B + 1);
    for (unsigned i = 0; i < k; ++i) w = ((w << 1) | (w >> B)) & ((uint64_t(1) << (B + 1)) - 1);
    const uint32_t r = uint32_t(w) & kMask<S>;
    c.x = uint32_t(w >> B) & 1;
    c.c = c.x;
    nz<S>(c, r);
    c.v = 0;
    return r;
}
template <unsigned S> inline uint32_t roxr(Cpu68k &c, uint32_t d, unsigned n) {
    d &= kMask<S>;
    constexpr unsigned B = S * 8;
    uint64_t w = (uint64_t(c.x) << B) | d;
    const unsigned k = n % (B + 1);
    for (unsigned i = 0; i < k; ++i) w = ((w >> 1) | ((w & 1) << B));
    const uint32_t r = uint32_t(w) & kMask<S>;
    c.x = uint32_t(w >> B) & 1;
    c.c = c.x;
    nz<S>(c, r);
    c.v = 0;
    return r;
}

// Condition codes (Bcc, DBcc, Scc).
inline bool cond(const Cpu68k &c, unsigned cc) {
    switch (cc & 15) {
    case 0: return true;
    case 1: return false;
    case 2: return !c.c && !c.z;
    case 3: return c.c || c.z;
    case 4: return !c.c;
    case 5: return c.c;
    case 6: return !c.z;
    case 7: return c.z;
    case 8: return !c.v;
    case 9: return c.v;
    case 10: return !c.n;
    case 11: return c.n;
    case 12: return c.n == c.v;
    case 13: return c.n != c.v;
    case 14: return !c.z && c.n == c.v;
    default: return c.z || c.n != c.v;
    }
}

} // namespace snd
