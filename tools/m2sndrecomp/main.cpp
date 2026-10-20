// m2sndrecomp: the sound CPU's 68000 static recompiler (design doc, sound).
//
//   m2sndrecomp SOUND_PROGRAM.bin OUT.cpp [--seeds FILE]
//
// Decodes every instruction reachable from the 68000 vector table plus any
// seed addresses (src/m68k/reach) and emits native C++, one block per
// instruction with its operands, addressing and size resolved here. Direct
// branches are gotos; returns and interrupt entry re-dispatch on the address.
// The flag semantics are the helpers in src/runtime/snd_cpu.h, held to MAME's
// 68000 by the lockstep check (tools/m2sndcheck).
//
// No fallback: an instruction without a native template fails the recompile,
// listed by address; control reaching an address that was not recompiled is
// a hard error at run time. Output is portable C++20 for x86-64 and ARM64.
//
// The output is derived from the game: write it under build/ (git-ignored),
// never commit it (rules.md rule 6).

#include "m68k/reach.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using m68k::Ea;
using m68k::Insn;
using m68k::Mode;
using m68k::Op;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%06xu", v);
    return b;
}
std::string lbl(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof b, "L_%06x", a);
    return b;
}
std::string S(unsigned size) { return std::to_string(size); }

// One instruction's C++ body being built.
struct Body {
    std::string code;
    int temps = 0;
    std::string tmp() { return "t" + std::to_string(temps++); }
    void add(const std::string &s) { code += s + " "; }
};

std::string xreg(const Ea &e) {
    const std::string r = e.xreg >= 8 ? "A[" + std::to_string(e.xreg & 7) + "]" : "D[" + std::to_string(e.xreg & 7) + "]";
    return e.xlong ? r : "uint32_t(int32_t(int16_t(" + r + ")))";
}

// Address of a memory operand, applying (An)+ / -(An) now (the 68000 order:
// an operand's own register update happens as the operand is evaluated).
std::string addr(Body &b, const Ea &e, unsigned size) {
    const std::string A = "A[" + std::to_string(e.reg) + "]";
    const unsigned step = (size == 1 && e.reg == 7) ? 2 : size;
    const std::string t = b.tmp();
    switch (e.mode) {
    case Mode::Ind: b.add("const uint32_t " + t + " = " + A + ";"); break;
    case Mode::PostInc: b.add("const uint32_t " + t + " = " + A + "; " + A + " += " + std::to_string(step) + ";"); break;
    case Mode::PreDec: b.add(A + " -= " + std::to_string(step) + "; const uint32_t " + t + " = " + A + ";"); break;
    case Mode::Disp: b.add("const uint32_t " + t + " = " + A + " + " + hex(uint32_t(e.disp)) + ";"); break;
    case Mode::Index: b.add("const uint32_t " + t + " = " + A + " + " + hex(uint32_t(e.disp)) + " + " + xreg(e) + ";"); break;
    case Mode::AbsW: case Mode::AbsL: case Mode::PcDisp: return hex(e.addr);
    case Mode::PcIndex: b.add("const uint32_t " + t + " = " + hex(e.addr) + " + " + xreg(e) + ";"); break;
    default: return "?";
    }
    return t;
}

std::string rd(const std::string &a, unsigned size) {
    return size == 1 ? "c.r8(" + a + ")" : size == 2 ? "c.r16(" + a + ")" : "c.r32(" + a + ")";
}
std::string wr(const std::string &a, unsigned size, const std::string &v) {
    return (size == 1 ? "c.w8(" : size == 2 ? "c.w16(" : "c.w32(") + a + ", " + v + ");";
}
std::string mask(unsigned size) { return size == 1 ? "0xffu" : size == 2 ? "0xffffu" : "0xffffffffu"; }

// A resolved operand: its value expression, and how to write it back.
struct Operand {
    Ea e;
    unsigned size;
    std::string a; // memory address temp / constant

    std::string value() const {
        switch (e.mode) {
        case Mode::Dn: return size == 4 ? "D[" + std::to_string(e.reg) + "]" : "(D[" + std::to_string(e.reg) + "] & " + mask(size) + ")";
        case Mode::An: return size == 4 ? "A[" + std::to_string(e.reg) + "]" : "(A[" + std::to_string(e.reg) + "] & " + mask(size) + ")";
        case Mode::Imm: return hex(e.imm);
        default: return rd(a, size);
        }
    }
    std::string write(const std::string &v) const {
        const std::string r = std::to_string(e.reg);
        switch (e.mode) {
        case Mode::Dn:
            if (size == 4) return "D[" + r + "] = " + v + ";";
            return "D[" + r + "] = (D[" + r + "] & ~" + mask(size) + ") | ((" + v + ") & " + mask(size) + ");";
        case Mode::An: return "A[" + r + "] = " + v + ";";
        default: return wr(a, size, v);
        }
    }
};

Operand operand(Body &b, const Ea &e, unsigned size) {
    Operand o{e, size, {}};
    if (e.is_mem()) o.a = addr(b, e, size);
    return o;
}

struct Emitter {
    const std::set<uint32_t> *addrs;
    std::ostringstream o;
    uint64_t native = 0;
    std::vector<std::string> missing;

    std::string jump(uint32_t t) const {
        if (addrs->count(t)) return "goto " + lbl(t) + ";";
        return "{ c.pc = " + hex(t) + "; goto dispatch; }";
    }
    void line(const std::string &s) { o << "    " << s << "\n"; }

    bool unsupported(const Insn &in) {
        char b[96];
        std::snprintf(b, sizeof b, "%06x: %s", in.addr, m68k::format_mame(in).c_str());
        missing.push_back(b);
        return false;
    }

    // Emits one instruction. Returns false when control never falls through.
    bool emit(const Insn &in) {
        const uint32_t pc = in.addr, next = pc + in.length;
        const unsigned sz = in.size;
        char cm[16];
        std::snprintf(cm, sizeof cm, ": // %06x: ", pc);
        o << lbl(pc) << cm << m68k::format_mame(in) << "\n";
        line("c.pc = " + hex(pc) + ";");
        line("if (s.boundary()) goto dispatch;");

        Body b;
        std::string exit;
        bool falls = true;
        const std::string Z = S(sz);

        auto binop = [&](const std::string &fn) { // dst = fn(src, dst) with flags
            Operand src = operand(b, in.src, sz);
            const std::string sv = b.tmp();
            b.add("const uint32_t " + sv + " = " + src.value() + ";");
            Operand dst = operand(b, in.dst, sz);
            b.add(dst.write("snd::" + fn + "<" + Z + ">(c, " + sv + ", " + dst.value() + ")"));
        };
        auto logic = [&](const std::string &opch) {
            Operand src = operand(b, in.src, sz);
            const std::string sv = b.tmp();
            b.add("const uint32_t " + sv + " = " + src.value() + ";");
            Operand dst = operand(b, in.dst, sz);
            b.add(dst.write("snd::logic<" + Z + ">(c, " + sv + " " + opch + " " + dst.value() + ")"));
        };
        auto unary = [&](const std::string &expr_of_v) { // dst = f(dst)
            Operand dst = operand(b, in.dst, sz);
            const std::string v = b.tmp();
            b.add("const uint32_t " + v + " = " + dst.value() + ";");
            std::string e = expr_of_v;
            for (size_t p; (p = e.find('$')) != std::string::npos;) e.replace(p, 1, v);
            b.add(dst.write(e));
        };

        switch (in.op) {
        case Op::Move: {
            Operand src = operand(b, in.src, sz);
            const std::string v = b.tmp();
            b.add("const uint32_t " + v + " = snd::logic<" + Z + ">(c, " + src.value() + ");");
            Operand dst = operand(b, in.dst, sz);
            b.add(dst.write(v));
            break;
        }
        case Op::Movea: {
            Operand src = operand(b, in.src, sz);
            b.add("A[" + std::to_string(in.dst.reg) + "] = snd::sext<" + Z + ">(" + src.value() + ");");
            break;
        }
        case Op::Moveq: b.add("D[" + std::to_string(in.dst.reg) + "] = snd::logic<4>(c, " + hex(in.src.imm) + ");"); break;
        case Op::Lea: b.add("A[" + std::to_string(in.dst.reg) + "] = " + addr(b, in.src, 4) + ";"); break;
        case Op::Pea: { const std::string a = addr(b, in.src, 4); b.add("c.push32(" + a + ");"); break; }
        case Op::Clr: { Operand dst = operand(b, in.dst, sz); b.add(dst.write("snd::logic<" + Z + ">(c, 0)")); break; }
        case Op::Tst: { Operand src = operand(b, in.src, sz); b.add("snd::logic<" + Z + ">(c, " + src.value() + ");"); break; }
        case Op::Not: unary("snd::logic<" + Z + ">(c, ~$)"); break;
        case Op::Neg: unary("snd::neg<" + Z + ">(c, $)"); break;
        case Op::Negx: unary("snd::negx<" + Z + ">(c, $)"); break;
        case Op::Ext: {
            const std::string r = "D[" + std::to_string(in.dst.reg) + "]";
            if (sz == 2) b.add(r + " = (" + r + " & 0xffff0000u) | snd::logic<2>(c, snd::sext<1>(" + r + "));");
            else b.add(r + " = snd::logic<4>(c, snd::sext<2>(" + r + "));");
            break;
        }
        case Op::Swap: {
            const std::string r = "D[" + std::to_string(in.dst.reg) + "]";
            b.add(r + " = snd::logic<4>(c, (" + r + " << 16) | (" + r + " >> 16));");
            break;
        }
        case Op::And: case Op::Andi: logic("&"); break;
        case Op::Or: case Op::Ori: logic("|"); break;
        case Op::Eor: case Op::Eori: logic("^"); break;
        case Op::Add: case Op::Addi: binop("add"); break;
        case Op::Sub: case Op::Subi: binop("sub"); break;
        case Op::Addx: binop("addx"); break;
        case Op::Subx: binop("subx"); break;
        case Op::Addq: case Op::Subq:
            if (in.dst.mode == Mode::An) { // whole register, no flags
                b.add("A[" + std::to_string(in.dst.reg) + "] " + (in.op == Op::Addq ? "+" : "-") + "= " + hex(in.src.imm) + ";");
            } else binop(in.op == Op::Addq ? "add" : "sub");
            break;
        case Op::Adda: case Op::Suba: {
            Operand src = operand(b, in.src, sz);
            b.add("A[" + std::to_string(in.dst.reg) + "] " + (in.op == Op::Adda ? "+" : "-") + "= snd::sext<" + Z + ">(" + src.value() + ");");
            break;
        }
        case Op::Cmp: case Op::Cmpi: case Op::Cmpm: {
            Operand src = operand(b, in.src, sz);
            const std::string sv = b.tmp();
            b.add("const uint32_t " + sv + " = " + src.value() + ";");
            Operand dst = operand(b, in.dst, sz);
            b.add("snd::cmp<" + Z + ">(c, " + sv + ", " + dst.value() + ");");
            break;
        }
        case Op::Cmpa: {
            Operand src = operand(b, in.src, sz);
            b.add("snd::cmp<4>(c, snd::sext<" + Z + ">(" + src.value() + "), A[" + std::to_string(in.dst.reg) + "]);");
            break;
        }
        case Op::Mulu: case Op::Muls: {
            Operand src = operand(b, in.src, 2);
            const std::string r = "D[" + std::to_string(in.dst.reg) + "]";
            if (in.op == Op::Mulu) b.add(r + " = snd::logic<4>(c, (" + r + " & 0xffffu) * " + src.value() + ");");
            else b.add(r + " = snd::logic<4>(c, uint32_t(int32_t(int16_t(" + r + ")) * int32_t(int16_t(" + src.value() + "))));");
            break;
        }
        case Op::Divu: case Op::Divs: {
            Operand src = operand(b, in.src, 2);
            const std::string r = "D[" + std::to_string(in.dst.reg) + "]", dv = b.tmp();
            b.add("const uint32_t " + dv + " = " + src.value() + ";");
            b.add("if (!" + dv + ") c.fatal(\"divide by zero\");");
            if (in.op == Op::Divu)
                b.add("{ const uint32_t q = " + r + " / " + dv + ", m = " + r + " % " + dv +
                      "; c.c = 0; if (q > 0xffffu) { c.v = 1; c.n = 1; } else { " + r +
                      " = (m << 16) | q; snd::logic<2>(c, q); } }");
            else
                b.add("{ const int32_t n = int32_t(" + r + "), d = int16_t(" + dv + "); const int64_t q = int64_t(n) / d, m = int64_t(n) % d; "
                      "c.c = 0; if (q > 32767 || q < -32768) { c.v = 1; c.n = 1; } else { " + r +
                      " = (uint32_t(m) << 16) | (uint32_t(q) & 0xffffu); snd::logic<2>(c, uint32_t(q)); } }");
            break;
        }
        case Op::Exg: {
            auto reg = [](const Ea &e) { return (e.mode == Mode::Dn ? "D[" : "A[") + std::to_string(e.reg) + "]"; };
            b.add("{ const uint32_t t = " + reg(in.src) + "; " + reg(in.src) + " = " + reg(in.dst) + "; " + reg(in.dst) + " = t; }");
            break;
        }
        case Op::Btst: case Op::Bchg: case Op::Bclr: case Op::Bset: {
            const bool reg = in.dst.mode == Mode::Dn;
            const std::string nb = b.tmp();
            b.add("const uint32_t " + nb + " = (" + (in.src.mode == Mode::Imm ? hex(in.src.imm) : "D[" + std::to_string(in.src.reg) + "]") +
                  ") & " + (reg ? "31u" : "7u") + ";");
            Operand dst = operand(b, in.dst, reg ? 4 : 1);
            const std::string v = b.tmp();
            b.add("const uint32_t " + v + " = " + dst.value() + ";");
            b.add("c.z = ((" + v + " >> " + nb + ") & 1) ? 0 : 1;");
            if (in.op == Op::Bchg) b.add(dst.write(v + " ^ (1u << " + nb + ")"));
            if (in.op == Op::Bclr) b.add(dst.write(v + " & ~(1u << " + nb + ")"));
            if (in.op == Op::Bset) b.add(dst.write(v + " | (1u << " + nb + ")"));
            break;
        }
        case Op::Asl: case Op::Asr: case Op::Lsl: case Op::Lsr: case Op::Roxl: case Op::Roxr: case Op::Rol: case Op::Ror: {
            static const char *fn[] = {"asl", "asr", "lsl", "lsr", "roxl", "roxr", "rol", "ror"};
            const std::string f = std::string("snd::") + fn[int(in.op) - int(Op::Asl)] + "<" + Z + ">";
            if (in.src.mode == Mode::None) { // memory, by one
                Operand dst = operand(b, in.dst, 2);
                b.add(dst.write(f + "(c, " + dst.value() + ", 1)"));
            } else {
                const std::string n = in.src.mode == Mode::Imm ? std::to_string(in.src.imm) : "(D[" + std::to_string(in.src.reg) + "] & 63u)";
                Operand dst = operand(b, in.dst, sz);
                b.add(dst.write(f + "(c, " + dst.value() + ", " + n + ")"));
            }
            break;
        }
        case Op::Scc: {
            Operand dst = operand(b, in.dst, 1);
            b.add(dst.write("snd::cond(c, " + std::to_string(in.cond) + ") ? 0xffu : 0u"));
            break;
        }
        case Op::Dbcc: {
            const std::string r = "D[" + std::to_string(in.dst.reg) + "]";
            b.add("bool br = false; if (!snd::cond(c, " + std::to_string(in.cond) + ")) { const uint32_t w = (" + r + " - 1) & 0xffffu; " + r +
                  " = (" + r + " & 0xffff0000u) | w; br = w != 0xffffu; }");
            exit = "if (br) " + jump(in.target);
            break;
        }
        case Op::Bcc: exit = "if (snd::cond(c, " + std::to_string(in.cond) + ")) " + jump(in.target); break;
        case Op::Bra: exit = jump(in.target); falls = false; break;
        case Op::Bsr: b.add("c.push32(" + hex(next) + ");"); exit = jump(in.target); falls = false; break;
        case Op::Jmp: case Op::Jsr: {
            const bool direct = !m68k::is_indirect(in);
            const std::string a = direct ? hex(in.src.addr) : addr(b, in.src, 4);
            if (in.op == Op::Jsr) b.add("c.push32(" + hex(next) + ");");
            if (direct) exit = jump(in.src.addr);
            else { b.add("c.pc = " + a + ";"); exit = "goto dispatch;"; }
            falls = false;
            break;
        }
        case Op::Rts: b.add("c.pc = c.pop32();"); exit = "goto dispatch;"; falls = false; break;
        case Op::Rtr: b.add("{ const uint16_t cc = c.pop16(); c.pc = c.pop32(); c.set_ccr(cc); }"); exit = "goto dispatch;"; falls = false; break;
        case Op::Rte:
            b.add("if (!(c.sys & 0x2000)) c.fatal(\"privilege violation\");");
            b.add("{ const uint16_t sr = c.pop16(); c.pc = c.pop32(); c.set_sr(sr); s.poke(); }");
            exit = "goto dispatch;";
            falls = false;
            break;
        case Op::MoveToSr: {
            b.add("if (!(c.sys & 0x2000)) c.fatal(\"privilege violation\");");
            Operand src = operand(b, in.src, 2);
            b.add("c.set_sr(" + src.value() + "); s.poke();");
            break;
        }
        case Op::MoveFromSr: { Operand dst = operand(b, in.dst, 2); b.add(dst.write("c.sr()")); break; }
        case Op::MoveToCcr: { Operand src = operand(b, in.src, 2); b.add("c.set_ccr(" + src.value() + " & 0xffu);"); break; }
        case Op::AndiSr: case Op::OriSr: case Op::EoriSr: {
            b.add("if (!(c.sys & 0x2000)) c.fatal(\"privilege violation\");");
            const char *opch = in.op == Op::AndiSr ? "&" : in.op == Op::OriSr ? "|" : "^";
            b.add("c.set_sr(c.sr() " + std::string(opch) + " " + hex(in.src.imm) + "); s.poke();");
            break;
        }
        case Op::AndiCcr: case Op::OriCcr: case Op::EoriCcr: {
            const char *opch = in.op == Op::AndiCcr ? "&" : in.op == Op::OriCcr ? "|" : "^";
            b.add("c.set_ccr((c.sr() " + std::string(opch) + " " + hex(in.src.imm) + ") & 0xffu);");
            break;
        }
        case Op::Movem: {
            const unsigned n = sz;
            auto reg = [](int i) { return (i < 8 ? "D[" : "A[") + std::to_string(i & 7) + "]"; };
            if (in.to_mem) {
                if (in.dst.mode == Mode::PreDec) { // mask bit 0 is A7; registers stored from A7 down
                    const std::string A = "A[" + std::to_string(in.dst.reg) + "]";
                    b.add("uint32_t a = " + A + ";");
                    for (int bit = 0; bit < 16; ++bit)
                        if (in.mask & (1u << bit)) b.add("a -= " + std::to_string(n) + "; " + wr("a", n, reg(15 - bit)));
                    b.add(A + " = a;");
                } else {
                    b.add("uint32_t a = " + addr(b, in.dst, n) + ";");
                    for (int i = 0; i < 16; ++i)
                        if (in.mask & (1u << i)) b.add(wr("a", n, reg(i)) + " a += " + std::to_string(n) + ";");
                }
            } else {
                const bool post = in.src.mode == Mode::PostInc;
                const std::string A = "A[" + std::to_string(in.src.reg) + "]";
                if (post) b.add("uint32_t a = " + A + ";");
                else b.add("uint32_t a = " + addr(b, in.src, n) + ";");
                for (int i = 0; i < 16; ++i)
                    if (in.mask & (1u << i))
                        b.add(reg(i) + " = " + (n == 2 ? "snd::sext<2>(" + rd("a", 2) + ")" : rd("a", 4)) + "; a += " + std::to_string(n) + ";");
                if (post) b.add(A + " = a;");
            }
            break;
        }
        case Op::Link: {
            const std::string A = "A[" + std::to_string(in.dst.reg) + "]";
            b.add("c.push32(" + A + "); " + A + " = A[7]; A[7] += " + hex(in.data) + ";");
            break;
        }
        case Op::Unlk: {
            const std::string A = "A[" + std::to_string(in.dst.reg) + "]";
            b.add("A[7] = " + A + "; " + A + " = c.pop32();");
            break;
        }
        case Op::Nop: break;
        default: return unsupported(in);
        }

        ++native;
        // the exit (a branch) runs after the count, inside the block so it
        // sees the body's locals
        line("{ " + b.code + "++s.count; " + exit + (exit.empty() ? "" : " ") + "}");
        return falls;
    }
};

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "m2sndrecomp: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2sndrecomp SOUND_PROGRAM.bin OUT.cpp [--seeds FILE]\n");
        return 2;
    }
    const std::vector<uint8_t> img = load(argv[1]);
    std::vector<uint32_t> seeds;
    for (int i = 3; i < argc; ++i)
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) {
            std::ifstream f(argv[++i]);
            std::string l;
            while (std::getline(f, l))
                if (!l.empty() && l[0] != '#') seeds.push_back(uint32_t(std::strtoul(l.c_str(), nullptr, 16)));
        }

    // Sound board map: ROM at 0, its upper 128 KiB mirrored at 0x080000.
    auto read = [&](uint32_t a) -> std::optional<uint16_t> {
        a &= 0xffffff;
        if (a >= 0x80000 && a < 0xa0000) a -= 0x60000;
        if (a + 1 >= img.size() || a >= 0x40000) return std::nullopt;
        return uint16_t(img[a] << 8 | img[a + 1]);
    };
    std::vector<uint32_t> all = m68k::vector_seeds(read);
    all.insert(all.end(), seeds.begin(), seeds.end());
    const m68k::ReachResult r = m68k::reach(all, read);

    std::set<uint32_t> addrs;
    for (const auto &[a, in] : r.insns) addrs.insert(a);
    Emitter em{&addrs, {}, 0, {}};
    em.o << "// Generated by m2sndrecomp. Derived from the game: never commit.\n"
         << "#include \"runtime/snd_gen_support.h\"\n\n#include <algorithm>\n#include <iterator>\n\nnamespace sndgen {\n\n"
         << "void run(Env &e) {\n    snd::Cpu68k &c = e.c;\n    snd::Sched &s = e.s;\n"
         << "    uint32_t *const D = c.d;\n    uint32_t *const A = c.a;\n"
         << "dispatch:\n    if (s.finished()) return;\n    switch (c.pc) {\n";
    for (uint32_t a : addrs) em.o << "    case " << hex(a) << ": goto " << lbl(a) << ";\n";
    em.o << "    default: return;\n    }\n";
    for (auto it = r.insns.begin(); it != r.insns.end(); ++it) {
        const Insn &in = it->second;
        const bool falls = em.emit(in);
        auto nx = std::next(it);
        if (falls && (nx == r.insns.end() || nx->first != in.addr + in.length))
            em.line("{ c.pc = " + hex(in.addr + in.length) + "; goto dispatch; }");
    }
    em.o << "}\n\nnamespace {\nconst uint32_t kAddrs[] = {\n";
    for (uint32_t a : addrs) em.o << "    " << hex(a) << ",\n";
    em.o << "};\n} // namespace\n\n"
         << "bool has_code(uint32_t a) { return std::binary_search(std::begin(kAddrs), std::end(kAddrs), a); }\n\n"
         << "uint64_t native_instructions() { return " << em.native << "ull; }\n\n} // namespace sndgen\n";

    if (!em.missing.empty()) {
        std::fprintf(stderr, "m2sndrecomp: FAILED: %zu reachable instructions have no native template:\n", em.missing.size());
        for (const std::string &m : em.missing) std::fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }
    if (!r.stops.empty() || !r.unmapped.empty()) {
        std::fprintf(stderr, "m2sndrecomp: FAILED: control flow reaches %zu undefined opcodes and %zu unmapped addresses\n",
                     r.stops.size(), r.unmapped.size());
        return 1;
    }
    std::ofstream(argv[2]) << em.o.str();
    std::printf("m2sndrecomp: %zu instructions from %zu seeds -> %s, all native; %zu indirect sites\n", r.insns.size(),
                all.size(), argv[2], r.indirect_sites.size());
    return 0;
}
