#include "runtime/snd_cpu.h"

#include <cstdio>

namespace snd {

void Cpu68k::reset() {
    sys = 0x2700;
    set_ccr(0);
    a[7] = r32(0);
    pc = r32(4);
}

void Cpu68k::set_sr(uint32_t v16) {
    const bool was_super = sys & 0x2000;
    sys = uint16_t(v16 & 0xa700);
    set_ccr(v16);
    if (was_super != bool(sys & 0x2000)) std::swap(a[7], other_sp);
}

void Cpu68k::take_irq(int level) {
    const uint16_t old = sr();
    if (!(sys & 0x2000)) std::swap(a[7], other_sp); // enter supervisor
    sys = uint16_t((sys & ~0x8700) | 0x2000 | (level << 8));
    push32(pc);
    push16(old);
    pc = r32(uint32_t(24 + level) * 4);
}

namespace {
bool dev_range(uint32_t a) {
    return (a >= 0xc20000 && a <= 0xc20003) || (a >= 0xc40000 && a <= 0xc40007) || (a >= 0xc60000 && a <= 0xc60007) ||
           (a >= 0xd00000 && a <= 0xd00007);
}
} // namespace

// 8-bit devices answer on the odd byte lane only; the even lane and unmapped
// addresses read 0 (MAME's unmap value for this space).
uint8_t Cpu68k::dev_r8(uint32_t addr) {
    if ((addr & 1) && dev_range(addr)) return dev->read(addr);
    return 0;
}

uint16_t Cpu68k::dev_r16(uint32_t addr) {
    if (dev_range(addr)) return dev->read(addr | 1);
    return 0;
}

void Cpu68k::dev_w(uint32_t addr, uint16_t data, uint16_t mem_mask) {
    const uint32_t even = addr & ~1u;
    if (dev_range(addr)) {
        if (mem_mask & 0x00ff) dev->write(even | 1, data & 0xff);
    } else if (even == 0xc40012 || even == 0xc50000 || even == 0xc70000) {
        dev->write(even, data & mem_mask);
    }
}

void Cpu68k::fatal(const std::string &what) const {
    char b[64];
    std::snprintf(b, sizeof b, " (sound 68000, pc %06x)", pc);
    throw rt::Fatal(what + b);
}

void Cpu68k::odd(uint32_t addr) const {
    char b[64];
    std::snprintf(b, sizeof b, "word access at odd address %06x", addr);
    fatal(b);
}

} // namespace snd
