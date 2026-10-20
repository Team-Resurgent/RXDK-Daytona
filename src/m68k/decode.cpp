#include "m68k/decode.h"

#include <cstdio>

namespace m68k {

namespace {

// Addressing-category masks, one bit per Mode.
constexpr uint32_t bit(Mode m) { return 1u << unsigned(m); }
constexpr uint32_t kMemAlt = bit(Mode::Ind) | bit(Mode::PostInc) | bit(Mode::PreDec) | bit(Mode::Disp) |
                             bit(Mode::Index) | bit(Mode::AbsW) | bit(Mode::AbsL);
constexpr uint32_t kDataAlt = kMemAlt | bit(Mode::Dn);
constexpr uint32_t kAlt = kDataAlt | bit(Mode::An);
constexpr uint32_t kData = kDataAlt | bit(Mode::PcDisp) | bit(Mode::PcIndex) | bit(Mode::Imm);
constexpr uint32_t kAll = kData | bit(Mode::An);
constexpr uint32_t kControl = bit(Mode::Ind) | bit(Mode::Disp) | bit(Mode::Index) | bit(Mode::AbsW) |
                              bit(Mode::AbsL) | bit(Mode::PcDisp) | bit(Mode::PcIndex);
constexpr uint32_t kCtrlAlt = kControl & ~(bit(Mode::PcDisp) | bit(Mode::PcIndex));

struct Decoder {
    const ReadWord &read;
    uint32_t pc;   // address of the next word to read
    bool ok = true;

    uint16_t word() {
        const auto w = read(pc);
        if (!w) ok = false;
        pc += 2;
        return w.value_or(0);
    }
    uint32_t lword() {
        const uint32_t hi = word();
        return (hi << 16) | word();
    }

    // Decodes an EA from its 6-bit mode/register field, reading extension
    // words. Returns false when the mode is not in `allowed`.
    bool ea(unsigned field, unsigned size, uint32_t allowed, Ea &e) {
        const unsigned mode = (field >> 3) & 7, reg = field & 7;
        e = Ea{};
        e.reg = uint8_t(reg);
        switch (mode) {
        case 0: e.mode = Mode::Dn; break;
        case 1: e.mode = Mode::An; break;
        case 2: e.mode = Mode::Ind; break;
        case 3: e.mode = Mode::PostInc; break;
        case 4: e.mode = Mode::PreDec; break;
        case 5: e.mode = Mode::Disp; break;
        case 6: e.mode = Mode::Index; break;
        default:
            switch (reg) {
            case 0: e.mode = Mode::AbsW; break;
            case 1: e.mode = Mode::AbsL; break;
            case 2: e.mode = Mode::PcDisp; break;
            case 3: e.mode = Mode::PcIndex; break;
            case 4: e.mode = Mode::Imm; break;
            default: return false;
            }
        }
        if (!(allowed & bit(e.mode))) return false;
        switch (e.mode) {
        case Mode::Disp: e.disp = int16_t(word()); break;
        case Mode::PcDisp: {
            const uint32_t base = pc;
            e.disp = int16_t(word());
            e.addr = base + uint32_t(e.disp);
            break;
        }
        case Mode::Index:
        case Mode::PcIndex: {
            const uint32_t base = pc;
            const uint16_t ext = word();
            if (ext & 0x0700) return false; // 68000: brief format only, no scale
            e.xreg = uint8_t((ext >> 12) & 15);
            e.xlong = ext & 0x0800;
            e.disp = int8_t(ext & 0xff);
            if (e.mode == Mode::PcIndex) e.addr = base + uint32_t(e.disp);
            break;
        }
        case Mode::AbsW: e.addr = uint32_t(int32_t(int16_t(word()))); break;
        case Mode::AbsL: e.addr = lword(); break;
        case Mode::Imm:
            if (size == 4) e.imm = lword();
            else if (size == 2) e.imm = word();
            else e.imm = word() & 0xff;
            break;
        default: break;
        }
        return true;
    }
};

Ea dreg(unsigned r) { Ea e; e.mode = Mode::Dn; e.reg = uint8_t(r & 7); return e; }
Ea areg(unsigned r) { Ea e; e.mode = Mode::An; e.reg = uint8_t(r & 7); return e; }
Ea imm(uint32_t v) { Ea e; e.mode = Mode::Imm; e.imm = v; return e; }
Ea mode_reg(Mode m, unsigned r) { Ea e; e.mode = m; e.reg = uint8_t(r & 7); return e; }

const uint8_t kSize[4] = {1, 2, 4, 0};

bool decode_body(Decoder &d, Insn &in) {
    const uint16_t op = in.opcode;
    const unsigned ry = op & 7, rx = (op >> 9) & 7, eaf = op & 0x3f;
    const unsigned sz2 = (op >> 6) & 3;
    auto illegal = [&] { in.op = Op::Illegal; return true; };

    switch (op >> 12) {
    case 0x0: {
        if (op & 0x0100) {
            if ((op & 0x38) == 0x08) { // movep
                in.op = Op::Movep;
                in.size = (op & 0x40) ? 4 : 2;
                in.to_mem = op & 0x80;
                Ea m = mode_reg(Mode::Disp, ry);
                m.disp = int16_t(d.word());
                if (in.to_mem) { in.src = dreg(rx); in.dst = m; }
                else { in.src = m; in.dst = dreg(rx); }
                return true;
            }
            static const Op bops[4] = {Op::Btst, Op::Bchg, Op::Bclr, Op::Bset};
            in.op = bops[sz2];
            in.src = dreg(rx);
            // btst Dn,<ea> accepts any data mode, the others data alterable
            if (!d.ea(eaf, 1, sz2 == 0 ? kData : kDataAlt, in.dst)) return illegal();
            in.size = in.dst.mode == Mode::Dn ? 4 : 1;
            return true;
        }
        const unsigned kind = (op >> 9) & 7;
        if (kind == 4) { // static bit
            static const Op bops[4] = {Op::Btst, Op::Bchg, Op::Bclr, Op::Bset};
            in.op = bops[sz2];
            in.src = imm(d.word() & 0xff);
            if (!d.ea(eaf, 1, sz2 == 0 ? (kData & ~bit(Mode::Imm)) : kDataAlt, in.dst)) return illegal();
            in.size = in.dst.mode == Mode::Dn ? 4 : 1;
            return true;
        }
        if (kind == 7) return illegal();
        if (kind == 0 || kind == 1 || kind == 5) {
            if (eaf == 0x3c && sz2 == 0) { // to CCR
                in.op = kind == 0 ? Op::OriCcr : kind == 1 ? Op::AndiCcr : Op::EoriCcr;
                in.size = 1;
                in.src = imm(d.word() & 0xff);
                return true;
            }
            if (eaf == 0x3c && sz2 == 1) { // to SR
                in.op = kind == 0 ? Op::OriSr : kind == 1 ? Op::AndiSr : Op::EoriSr;
                in.size = 2;
                in.src = imm(d.word());
                return true;
            }
        }
        static const Op iops[8] = {Op::Ori, Op::Andi, Op::Subi, Op::Addi, Op::Illegal, Op::Eori, Op::Cmpi, Op::Illegal};
        if (sz2 == 3) return illegal();
        in.op = iops[kind];
        in.size = kSize[sz2];
        in.src = imm(in.size == 4 ? d.lword() : in.size == 2 ? d.word() : (d.word() & 0xff));
        if (!d.ea(eaf, in.size, kDataAlt, in.dst)) return illegal();
        return true;
    }

    case 0x1: case 0x2: case 0x3: {
        const unsigned s = op >> 12;
        in.size = s == 1 ? 1 : s == 3 ? 2 : 4;
        const unsigned dmode = (op >> 6) & 7;
        if (!d.ea(eaf, in.size, in.size == 1 ? kData : kAll, in.src)) return illegal();
        if (dmode == 1) {
            if (in.size == 1) return illegal();
            in.op = Op::Movea;
            in.dst = areg(rx);
            return true;
        }
        in.op = Op::Move;
        if (!d.ea((dmode << 3) | rx, in.size, kDataAlt, in.dst)) return illegal();
        return true;
    }

    case 0x4: {
        if (op == 0x4afc) return illegal();
        if ((op & 0xf1c0) == 0x41c0) { // lea
            in.op = Op::Lea;
            in.size = 4;
            if (!d.ea(eaf, 4, kControl, in.src)) return illegal();
            in.dst = areg(rx);
            return true;
        }
        if ((op & 0xf1c0) == 0x4180) { // chk.w
            in.op = Op::Chk;
            in.size = 2;
            if (!d.ea(eaf, 2, kData, in.src)) return illegal();
            in.dst = dreg(rx);
            return true;
        }
        if (op & 0x0100) return illegal();
        switch ((op >> 8) & 0xf) {
        case 0x0: case 0x2: case 0x4: case 0x6:
            if (sz2 == 3) {
                switch ((op >> 8) & 0xf) {
                case 0x0: in.op = Op::MoveFromSr; in.size = 2; return d.ea(eaf, 2, kDataAlt, in.dst) || illegal();
                case 0x4: in.op = Op::MoveToCcr; in.size = 2; return d.ea(eaf, 2, kData, in.src) || illegal();
                case 0x6: in.op = Op::MoveToSr; in.size = 2; return d.ea(eaf, 2, kData, in.src) || illegal();
                default: return illegal(); // move from CCR is 68010+
                }
            }
            in.op = ((op >> 8) & 0xf) == 0 ? Op::Negx : ((op >> 8) & 0xf) == 2 ? Op::Clr
                  : ((op >> 8) & 0xf) == 4 ? Op::Neg : Op::Not;
            in.size = kSize[sz2];
            return d.ea(eaf, in.size, kDataAlt, in.dst) || illegal();
        case 0x8:
            switch (sz2) {
            case 0: in.op = Op::Nbcd; in.size = 1; return d.ea(eaf, 1, kDataAlt, in.dst) || illegal();
            case 1:
                if ((op & 0x38) == 0) { in.op = Op::Swap; in.size = 4; in.dst = dreg(ry); return true; }
                in.op = Op::Pea; in.size = 4; return d.ea(eaf, 4, kControl, in.src) || illegal();
            default:
                if ((op & 0x38) == 0) { in.op = Op::Ext; in.size = sz2 == 2 ? 2 : 4; in.dst = dreg(ry); return true; }
                in.op = Op::Movem;
                in.size = sz2 == 2 ? 2 : 4;
                in.to_mem = true;
                in.mask = d.word();
                return d.ea(eaf, in.size, kCtrlAlt | bit(Mode::PreDec), in.dst) || illegal();
            }
        case 0xa:
            if (sz2 == 3) { in.op = Op::Tas; in.size = 1; return d.ea(eaf, 1, kDataAlt, in.dst) || illegal(); }
            in.op = Op::Tst; in.size = kSize[sz2];
            return d.ea(eaf, in.size, kDataAlt, in.src) || illegal();
        case 0xc:
            if (sz2 < 2) return illegal(); // mulu.l / divu.l are 68020+
            in.op = Op::Movem;
            in.size = sz2 == 2 ? 2 : 4;
            in.to_mem = false;
            in.mask = d.word();
            return d.ea(eaf, in.size, kControl | bit(Mode::PostInc), in.src) || illegal();
        case 0xe:
            if (sz2 == 2) { in.op = Op::Jsr; return d.ea(eaf, 4, kControl, in.src) || illegal(); }
            if (sz2 == 3) { in.op = Op::Jmp; return d.ea(eaf, 4, kControl, in.src) || illegal(); }
            if (sz2 == 0) return illegal();
            switch ((op >> 3) & 7) {
            case 0: case 1: in.op = Op::Trap; in.data = op & 15; return true;
            case 2: in.op = Op::Link; in.dst = areg(ry); in.data = uint32_t(int32_t(int16_t(d.word()))); return true;
            case 3: in.op = Op::Unlk; in.dst = areg(ry); return true;
            case 4: in.op = Op::MoveUsp; in.to_mem = true; in.src = areg(ry); return true;  // An -> USP
            case 5: in.op = Op::MoveUsp; in.to_mem = false; in.dst = areg(ry); return true; // USP -> An
            default:
                switch (op & 7) {
                case 0: in.op = Op::Reset; return true;
                case 1: in.op = Op::Nop; return true;
                case 2: in.op = Op::Stop; in.data = d.word(); return true;
                case 3: in.op = Op::Rte; return true;
                case 5: in.op = Op::Rts; return true;
                case 6: in.op = Op::Trapv; return true;
                case 7: in.op = Op::Rtr; return true;
                default: return illegal(); // rtd is 68010+
                }
            }
        default: return illegal();
        }
    }

    case 0x5:
        if (sz2 == 3) {
            in.cond = uint8_t((op >> 8) & 15);
            if ((op & 0x38) == 0x08) {
                in.op = Op::Dbcc;
                in.dst = dreg(ry);
                const uint32_t base = d.pc;
                in.target = base + uint32_t(int32_t(int16_t(d.word())));
                return true;
            }
            in.op = Op::Scc;
            in.size = 1;
            return d.ea(eaf, 1, kDataAlt, in.dst) || illegal();
        }
        in.op = (op & 0x0100) ? Op::Subq : Op::Addq;
        in.size = kSize[sz2];
        in.src = imm(rx ? rx : 8);
        return d.ea(eaf, in.size, in.size == 1 ? kDataAlt : kAlt, in.dst) || illegal();

    case 0x6: {
        in.cond = uint8_t((op >> 8) & 15);
        in.op = in.cond == 0 ? Op::Bra : in.cond == 1 ? Op::Bsr : Op::Bcc;
        const uint32_t base = d.pc;
        const int8_t d8 = int8_t(op & 0xff);
        if (d8 == 0) in.target = base + uint32_t(int32_t(int16_t(d.word())));
        else in.target = base + uint32_t(int32_t(d8));
        return true;
    }

    case 0x7:
        if (op & 0x0100) return illegal();
        in.op = Op::Moveq;
        in.size = 4;
        in.src = imm(uint32_t(int32_t(int8_t(op & 0xff))));
        in.dst = dreg(rx);
        return true;

    case 0x8: case 0xc: {
        const bool is_and = (op >> 12) == 0xc;
        if ((op & 0x1c0) == 0x0c0 || (op & 0x1c0) == 0x1c0) {
            in.op = is_and ? ((op & 0x100) ? Op::Muls : Op::Mulu) : ((op & 0x100) ? Op::Divs : Op::Divu);
            in.size = 2;
            in.dst = dreg(rx);
            return d.ea(eaf, 2, kData, in.src) || illegal();
        }
        if ((op & 0x1f0) == 0x100) {
            in.op = is_and ? Op::Abcd : Op::Sbcd;
            in.size = 1;
            if (op & 8) { in.src = mode_reg(Mode::PreDec, ry); in.dst = mode_reg(Mode::PreDec, rx); }
            else { in.src = dreg(ry); in.dst = dreg(rx); }
            return true;
        }
        if (is_and && (op & 0x130) == 0x100) { // exg
            switch (op & 0x1f8) {
            case 0x140: in.op = Op::Exg; in.src = dreg(rx); in.dst = dreg(ry); return true;
            case 0x148: in.op = Op::Exg; in.src = areg(rx); in.dst = areg(ry); return true;
            case 0x188: in.op = Op::Exg; in.src = dreg(rx); in.dst = areg(ry); return true;
            default: return illegal();
            }
        }
        in.op = is_and ? Op::And : Op::Or;
        in.size = kSize[sz2];
        if (op & 0x100) { in.src = dreg(rx); return d.ea(eaf, in.size, kMemAlt, in.dst) || illegal(); }
        in.dst = dreg(rx);
        return d.ea(eaf, in.size, kData, in.src) || illegal();
    }

    case 0x9: case 0xd: {
        const bool add = (op >> 12) == 0xd;
        if (sz2 == 3) {
            in.op = add ? Op::Adda : Op::Suba;
            in.size = (op & 0x100) ? 4 : 2;
            in.dst = areg(rx);
            return d.ea(eaf, in.size, kAll, in.src) || illegal();
        }
        in.size = kSize[sz2];
        if ((op & 0x130) == 0x100) {
            in.op = add ? Op::Addx : Op::Subx;
            if (op & 8) { in.src = mode_reg(Mode::PreDec, ry); in.dst = mode_reg(Mode::PreDec, rx); }
            else { in.src = dreg(ry); in.dst = dreg(rx); }
            return true;
        }
        in.op = add ? Op::Add : Op::Sub;
        if (op & 0x100) { in.src = dreg(rx); return d.ea(eaf, in.size, kMemAlt, in.dst) || illegal(); }
        in.dst = dreg(rx);
        return d.ea(eaf, in.size, in.size == 1 ? kData : kAll, in.src) || illegal();
    }

    case 0xb:
        if (sz2 == 3) {
            in.op = Op::Cmpa;
            in.size = (op & 0x100) ? 4 : 2;
            in.dst = areg(rx);
            return d.ea(eaf, in.size, kAll, in.src) || illegal();
        }
        in.size = kSize[sz2];
        if (!(op & 0x100)) {
            in.op = Op::Cmp;
            in.dst = dreg(rx);
            return d.ea(eaf, in.size, in.size == 1 ? kData : kAll, in.src) || illegal();
        }
        if ((op & 0x38) == 0x08) {
            in.op = Op::Cmpm;
            in.src = mode_reg(Mode::PostInc, ry);
            in.dst = mode_reg(Mode::PostInc, rx);
            return true;
        }
        in.op = Op::Eor;
        in.src = dreg(rx);
        return d.ea(eaf, in.size, kDataAlt, in.dst) || illegal();

    case 0xe: {
        static const Op left[4] = {Op::Asl, Op::Lsl, Op::Roxl, Op::Rol};
        static const Op right[4] = {Op::Asr, Op::Lsr, Op::Roxr, Op::Ror};
        const bool l = op & 0x100;
        if (sz2 == 3) { // memory, one bit, word
            if (op & 0x0800) return illegal();
            const unsigned t = (op >> 9) & 3;
            in.op = l ? left[t] : right[t];
            in.size = 2;
            return d.ea(eaf, 2, kMemAlt, in.dst) || illegal();
        }
        const unsigned t = (op >> 3) & 3;
        in.op = l ? left[t] : right[t];
        in.size = kSize[sz2];
        in.src = (op & 0x20) ? dreg(rx) : imm(rx ? rx : 8);
        in.dst = dreg(ry);
        return true;
    }

    case 0xa: in.op = Op::LineA; return true;
    case 0xf: in.op = Op::LineF; return true;
    }
    return illegal();
}

std::string hexs(const char *fmt, uint32_t v) {
    char b[32];
    std::snprintf(b, sizeof b, fmt, v);
    return b;
}
std::string sh8(uint32_t v) { v &= 0xff; return v == 0x80 ? "-$80" : (v & 0x80) ? hexs("-$%x", (0x100 - v) & 0x7f) : hexs("$%x", v & 0x7f); }
std::string sh16(uint32_t v) { v &= 0xffff; return v == 0x8000 ? "-$8000" : (v & 0x8000) ? hexs("-$%x", (0x10000 - v) & 0x7fff) : hexs("$%x", v & 0x7fff); }
std::string sh32(uint32_t v) { return v == 0x80000000u ? "-$80000000" : (v & 0x80000000u) ? hexs("-$%x", (0u - v) & 0x7fffffff) : hexs("$%x", v & 0x7fffffff); }

std::string ea_str(const Ea &e) {
    auto x = [&] { return std::string(e.xreg >= 8 ? "A" : "D") + std::to_string(e.xreg & 7) + (e.xlong ? ".l" : ".w"); };
    const std::string r = std::to_string(e.reg);
    switch (e.mode) {
    case Mode::Dn: return "D" + r;
    case Mode::An: return "A" + r;
    case Mode::Ind: return "(A" + r + ")";
    case Mode::PostInc: return "(A" + r + ")+";
    case Mode::PreDec: return "-(A" + r + ")";
    case Mode::Disp: return "(" + sh16(uint32_t(e.disp)) + ",A" + r + ")";
    case Mode::Index: return e.disp ? "(" + sh8(uint32_t(e.disp)) + ",A" + r + "," + x() + ")" : "(A" + r + "," + x() + ")";
    case Mode::AbsW: return hexs("$%x.w", e.addr & 0xffff);
    case Mode::AbsL: return hexs("$%x.l", e.addr);
    case Mode::PcDisp: return hexs("($%x,PC)", e.addr);
    case Mode::PcIndex: return e.disp ? "(" + sh8(uint32_t(e.disp)) + ",PC," + x() + ")" : "(PC," + x() + ")";
    case Mode::Imm: return hexs("#$%x", e.imm);
    default: return "?";
    }
}
std::string imm_s(const Insn &in) {
    return "#" + (in.size == 1 ? sh8(in.src.imm) : in.size == 2 ? sh16(in.src.imm) : sh32(in.src.imm));
}
const char *const kCc[16] = {"t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"};
char kSz(unsigned s) { return s == 1 ? 'b' : s == 2 ? 'w' : 'l'; }

std::string regs(uint16_t m) {
    std::string out;
    for (int bank = 0; bank < 2; ++bank)
        for (int i = 0; i < 8; ++i) {
            if (!(m & (1u << (i + bank * 8)))) continue;
            const int first = i;
            while (i < 7 && (m & (1u << (i + 1 + bank * 8)))) ++i;
            if (!out.empty()) out += '/';
            out += (bank ? "A" : "D") + std::to_string(first);
            if (i > first) out += std::string("-") + (bank ? "A" : "D") + std::to_string(i);
        }
    return out;
}

std::string pad(std::string mn) {
    if (mn.size() < 8) mn.resize(8, ' ');
    else mn += ' ';
    return mn;
}

} // namespace

bool decode(uint32_t addr, const ReadWord &read, Insn &out) {
    Decoder d{read, addr};
    out = Insn{};
    out.addr = addr;
    out.opcode = d.word();
    if (!d.ok) return false;
    decode_body(d, out);
    if (!d.ok) return false;
    if (out.op == Op::Illegal || out.op == Op::LineA || out.op == Op::LineF) {
        const uint16_t op = out.opcode;
        const Op o = out.op;
        out = Insn{};
        out.addr = addr;
        out.opcode = op;
        out.op = o;
        out.length = 2;
        return true;
    }
    out.length = d.pc - addr;
    return true;
}

std::string format_mame(const Insn &in) {
    const std::string s = std::string(".") + kSz(in.size);
    auto two = [&](const char *mn, const std::string &a, const std::string &b) { return pad(mn + s) + a + ", " + b; };
    switch (in.op) {
    case Op::Illegal: return hexs("dc.w    $%04x; ILLEGAL", in.opcode);
    case Op::LineA: return hexs("dc.w    $%04x; opcode 1010", in.opcode);
    case Op::LineF: return hexs("dc.w    $%04x; opcode 1111", in.opcode);
    case Op::Ori: return two("ori", ea_str(in.src), ea_str(in.dst));
    case Op::Andi: return two("andi", ea_str(in.src), ea_str(in.dst));
    case Op::Eori: return two("eori", ea_str(in.src), ea_str(in.dst));
    case Op::Subi: return two("subi", imm_s(in), ea_str(in.dst));
    case Op::Addi: return two("addi", imm_s(in), ea_str(in.dst));
    case Op::Cmpi: return two("cmpi", imm_s(in), ea_str(in.dst));
    case Op::OriCcr: return pad("ori") + ea_str(in.src) + ", CCR";
    case Op::AndiCcr: return pad("andi") + ea_str(in.src) + ", CCR";
    case Op::EoriCcr: return pad("eori") + ea_str(in.src) + ", CCR";
    case Op::OriSr: return pad("ori") + ea_str(in.src) + ", SR";
    case Op::AndiSr: return pad("andi") + ea_str(in.src) + ", SR";
    case Op::EoriSr: return pad("eori") + ea_str(in.src) + ", SR";
    case Op::Btst: case Op::Bchg: case Op::Bclr: case Op::Bset: {
        static const char *n[] = {"btst", "bchg", "bclr", "bset"};
        return pad(n[int(in.op) - int(Op::Btst)]) + ea_str(in.src) + ", " + ea_str(in.dst);
    }
    case Op::Movep:
        return in.to_mem ? pad("movep" + s) + ea_str(in.src) + hexs(", ($%x", uint32_t(in.dst.disp) & 0xffff) + ",A" + std::to_string(in.dst.reg) + ")"
                         : pad("movep" + s) + hexs("($%x", uint32_t(in.src.disp) & 0xffff) + ",A" + std::to_string(in.src.reg) + "), " + ea_str(in.dst);
    case Op::Move: return two("move", ea_str(in.src), ea_str(in.dst));
    case Op::Movea: return two("movea", ea_str(in.src), ea_str(in.dst));
    case Op::Moveq: return pad("moveq") + "#" + sh8(in.src.imm) + ", " + ea_str(in.dst);
    case Op::MoveFromSr: return pad("move") + "SR, " + ea_str(in.dst);
    case Op::MoveToCcr: { Ea e = in.src; if (e.mode == Mode::Imm) e.imm &= 0xff; return pad("move") + ea_str(e) + ", CCR"; }
    case Op::MoveToSr: return pad("move") + ea_str(in.src) + ", SR";
    case Op::MoveUsp: return in.to_mem ? pad("move") + ea_str(in.src) + ", USP" : pad("move") + "USP, " + ea_str(in.dst);
    case Op::Movem: {
        uint16_t m = in.mask;
        if (in.to_mem && in.dst.mode == Mode::PreDec) { // bit 0 is A7
            uint16_t r = 0;
            for (int i = 0; i < 16; ++i) if (m & (1u << i)) r |= uint16_t(1u << (15 - i));
            m = r;
        }
        return in.to_mem ? two("movem", regs(m), ea_str(in.dst)) : two("movem", ea_str(in.src), regs(m));
    }
    case Op::Negx: return pad("negx" + s) + ea_str(in.dst);
    case Op::Clr: return pad("clr" + s) + ea_str(in.dst);
    case Op::Neg: return pad("neg" + s) + ea_str(in.dst);
    case Op::Not: return pad("not" + s) + ea_str(in.dst);
    case Op::Nbcd: return pad("nbcd") + ea_str(in.dst);
    case Op::Tst: return pad("tst" + s) + ea_str(in.src);
    case Op::Tas: return pad("tas") + ea_str(in.dst);
    case Op::Ext: return pad("ext" + s) + ea_str(in.dst);
    case Op::Swap: return pad("swap") + ea_str(in.dst);
    case Op::Pea: return pad("pea") + ea_str(in.src);
    case Op::Lea: return pad("lea") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Chk: return two("chk", ea_str(in.src), ea_str(in.dst));
    case Op::Link: return pad("link") + ea_str(in.dst) + ", #" + sh16(in.data);
    case Op::Unlk: return pad("unlk") + ea_str(in.dst);
    case Op::Trap: return pad("trap") + hexs("#$%x", in.data);
    case Op::Trapv: return "trapv";
    case Op::Reset: return "reset";
    case Op::Nop: return "nop";
    case Op::Stop: return pad("stop") + "#" + sh16(in.data);
    case Op::Rte: return "rte";
    case Op::Rts: return "rts";
    case Op::Rtr: return "rtr";
    case Op::Jsr: return pad("jsr") + ea_str(in.src);
    case Op::Jmp: return pad("jmp") + ea_str(in.src);
    case Op::Addq: return pad("addq" + s) + "#" + std::to_string(in.src.imm) + ", " + ea_str(in.dst);
    case Op::Subq: return pad("subq" + s) + "#" + std::to_string(in.src.imm) + ", " + ea_str(in.dst);
    case Op::Scc: return pad(std::string("s") + kCc[in.cond]) + ea_str(in.dst);
    case Op::Dbcc: return (in.cond == 1 ? pad("dbra") : pad(std::string("db") + kCc[in.cond])) + ea_str(in.dst) + hexs(", $%x", in.target);
    case Op::Bra: return pad("bra") + hexs("$%x", in.target);
    case Op::Bsr: return pad("bsr") + hexs("$%x", in.target);
    case Op::Bcc: return pad(std::string("b") + kCc[in.cond]) + hexs("$%x", in.target);
    case Op::Or: return two("or", ea_str(in.src), ea_str(in.dst));
    case Op::And: return two("and", ea_str(in.src), ea_str(in.dst));
    case Op::Eor: return two("eor", ea_str(in.src), ea_str(in.dst));
    case Op::Add: return two("add", ea_str(in.src), ea_str(in.dst));
    case Op::Sub: return two("sub", ea_str(in.src), ea_str(in.dst));
    case Op::Cmp: return two("cmp", ea_str(in.src), ea_str(in.dst));
    case Op::Adda: return two("adda", ea_str(in.src), ea_str(in.dst));
    case Op::Suba: return two("suba", ea_str(in.src), ea_str(in.dst));
    case Op::Cmpa: return two("cmpa", ea_str(in.src), ea_str(in.dst));
    case Op::Cmpm: return two("cmpm", ea_str(in.src), ea_str(in.dst));
    case Op::Addx: return two("addx", ea_str(in.src), ea_str(in.dst));
    case Op::Subx: return two("subx", ea_str(in.src), ea_str(in.dst));
    case Op::Abcd: return pad("abcd") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Sbcd: return pad("sbcd") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Mulu: return pad("mulu.w") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Muls: return pad("muls.w") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Divu: return pad("divu.w") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Divs: return pad("divs.w") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Exg: return pad("exg") + ea_str(in.src) + ", " + ea_str(in.dst);
    case Op::Asl: case Op::Asr: case Op::Lsl: case Op::Lsr: case Op::Roxl: case Op::Roxr: case Op::Rol: case Op::Ror: {
        static const char *n[] = {"asl", "asr", "lsl", "lsr", "roxl", "roxr", "rol", "ror"};
        const std::string mn = n[int(in.op) - int(Op::Asl)] + s;
        if (in.src.mode == Mode::None) return pad(mn) + ea_str(in.dst);
        return pad(mn) + (in.src.mode == Mode::Imm ? "#" + std::to_string(in.src.imm) : ea_str(in.src)) + ", " + ea_str(in.dst);
    }
    }
    return "?";
}

bool is_branch(const Insn &in) { return in.op == Op::Bra || in.op == Op::Bcc || in.op == Op::Dbcc || in.op == Op::Bsr; }

bool ends_block(const Insn &in) {
    switch (in.op) {
    case Op::Bra: case Op::Jmp: case Op::Rts: case Op::Rte: case Op::Rtr:
    case Op::Illegal: case Op::LineA: case Op::LineF:
        return true;
    default: return false;
    }
}

bool is_indirect(const Insn &in) {
    if (in.op != Op::Jmp && in.op != Op::Jsr) return false;
    return !(in.src.mode == Mode::AbsW || in.src.mode == Mode::AbsL || in.src.mode == Mode::PcDisp);
}

} // namespace m68k
