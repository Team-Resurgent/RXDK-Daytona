// Unit tests for the sound CPU's 68000 decoder (src/m68k) and the flag
// arithmetic recompiled code uses (src/runtime/snd_cpu.h). Encodings are
// built by hand; expected text is MAME's m68kdasm format. The recompiled
// program as a whole is held to MAME by the lockstep check (m2sndcheck).

#include "m68k/decode.h"
#include "runtime/snd_cpu.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

// Decodes words placed at addr.
m68k::Insn dec(uint32_t addr, std::vector<uint16_t> words) {
    std::map<uint32_t, uint16_t> mem;
    for (size_t i = 0; i < words.size(); ++i) mem[addr + uint32_t(i) * 2] = words[i];
    m68k::Insn in;
    const bool ok = m68k::decode(addr, [&](uint32_t a) -> std::optional<uint16_t> {
        const auto it = mem.find(a);
        if (it == mem.end()) return std::nullopt;
        return it->second;
    }, in);
    CHECK(ok);
    return in;
}

void text(uint32_t addr, std::vector<uint16_t> words, const char *expected, uint32_t length) {
    const m68k::Insn in = dec(addr, words);
    const std::string got = m68k::format_mame(in);
    ++g_checks;
    if (got != expected || in.length != length) {
        ++g_failures;
        std::printf("%06x: [%s] length %u, expected [%s] length %u\n", addr, got.c_str(), in.length, expected, length);
    }
}

void decoder() {
    text(0x120, {0x46fc, 0x2700}, "move    #$2700, SR", 4);
    text(0x124, {0x48e7, 0x9820}, "movem.l D0/D3-D4/A2, -(A7)", 4);
    text(0x128, {0x1639, 0x00c2, 0x0003}, "move.b  $c20003.l, D3", 6);
    text(0x12e, {0x0203, 0x0038}, "andi.b  #$38, D3", 4);
    text(0x132, {0x6600, 0x00c8}, "bne     $1fc", 4);
    text(0x13c, {0x0c03, 0x00ff}, "cmpi.b  #-$1, D3", 4);
    text(0x346, {0x51c8, 0xfffa}, "dbra    D0, $342", 4);
    text(0x166, {0x4eba, 0x0694}, "jsr     ($7fc,PC)", 4);
    text(0x358, {0x781b}, "moveq   #$1b, D4", 2);
    text(0x4a4, {0xe248}, "lsr.w   #1, D0", 2);
    text(0x4a2, {0xc6c5}, "mulu.w  D5, D3", 2);
    text(0x100, {0x4e73}, "rte", 2);
    text(0x100, {0x4e75}, "rts", 2);
    text(0x100, {0xffff}, "dc.w    $ffff; opcode 1111", 2);

    const m68k::Insn b = dec(0x132, {0x6600, 0x00c8});
    CHECK(b.op == m68k::Op::Bcc && b.cond == 6 && b.target == 0x1fc && m68k::is_branch(b) && !m68k::ends_block(b));
    const m68k::Insn j = dec(0x166, {0x4eba, 0x0694});
    CHECK(j.op == m68k::Op::Jsr && !m68k::is_indirect(j) && j.src.addr == 0x7fc);
    const m68k::Insn ji = dec(0x200, {0x4e90}); // jsr (A0)
    CHECK(ji.op == m68k::Op::Jsr && m68k::is_indirect(ji));
    CHECK(m68k::ends_block(dec(0x100, {0x4e75})));
    CHECK(dec(0x100, {0x4afc}).op == m68k::Op::Illegal);
    CHECK(dec(0x100, {0x0e00}).op == m68k::Op::Illegal); // moves (68010+)
}

void flags() {
    snd::Cpu68k c;
    auto f = [&](int x, int n, int z, int v, int cc) { return int(c.x) == x && int(c.n) == n && int(c.z) == z && int(c.v) == v && int(c.c) == cc; };
    CHECK(snd::add<1>(c, 0x7f, 0x01) == 0x80 && f(0, 1, 0, 1, 0));
    CHECK(snd::add<2>(c, 0xffff, 0x0001) == 0 && f(1, 0, 1, 0, 1));
    CHECK(snd::sub<1>(c, 1, 0) == 0xff && f(1, 1, 0, 0, 1)); // 0 - 1
    CHECK(snd::sub<1>(c, 1, 0x80) == 0x7f && f(0, 0, 0, 1, 0)); // -128 - 1 overflows
    c.x = 1;
    snd::cmp<2>(c, 5, 5);
    CHECK(f(1, 0, 1, 0, 0)); // X untouched
    CHECK(snd::logic<4>(c, 0x80000000u) == 0x80000000u && c.n == 1 && c.v == 0 && c.c == 0);
    CHECK(snd::asl<1>(c, 0x40, 1) == 0x80 && c.v == 1 && c.c == 0);
    CHECK(snd::asl<1>(c, 0x81, 1) == 0x02 && c.v == 1 && c.c == 1 && c.x == 1);
    CHECK(snd::asl<1>(c, 0xc0, 1) == 0x80 && c.v == 0);
    CHECK(snd::lsr<2>(c, 0x0001, 1) == 0 && c.c == 1 && c.x == 1 && c.z == 1);
    CHECK(snd::lsl<4>(c, 1, 32) == 0 && c.c == 1);
    CHECK(snd::asr<1>(c, 0x81, 1) == 0xc0 && c.c == 1 && c.n == 1);
    CHECK(snd::rol<1>(c, 0x81, 1) == 0x03 && c.c == 1);
    CHECK(snd::ror<1>(c, 0x01, 1) == 0x80 && c.c == 1);
    c.x = 1;
    CHECK(snd::roxl<1>(c, 0x80, 1) == 0x01 && c.x == 1 && c.c == 1);
    c.x = 0;
    c.z = 1;
    CHECK(snd::addx<1>(c, 0, 0) == 0 && c.z == 1); // Z kept when the result is zero
    CHECK(snd::cond(c, 7) && !snd::cond(c, 6));    // eq / ne
}

} // namespace

int main() {
    decoder();
    flags();
    std::printf("test_m68k: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
