// Motorola 68000 instruction decoder for the sound CPU's static recompiler
// (tools/m2sndrecomp). Decodes one instruction into its operation, size and
// fully resolved operands (extension words read, PC-relative addresses and
// branch targets computed), so the recompiler emits each instruction with
// nothing left to decode at run time.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace m68k {

// Effective-address forms.
enum class Mode : uint8_t {
    None,
    Dn,      // Dn
    An,      // An
    Ind,     // (An)
    PostInc, // (An)+
    PreDec,  // -(An)
    Disp,    // (d16,An)
    Index,   // (d8,An,Xn)
    AbsW,    // (xxx).w
    AbsL,    // (xxx).l
    PcDisp,  // (d16,PC)
    PcIndex, // (d8,PC,Xn)
    Imm,     // #imm
};

struct Ea {
    Mode mode = Mode::None;
    uint8_t reg = 0;    // Dn / An number
    int32_t disp = 0;   // Disp, Index, PcDisp, PcIndex (sign-extended)
    uint8_t xreg = 0;   // index register: 0-7 D0-D7, 8-15 A0-A7
    bool xlong = false; // index register used as .l (else sign-extended .w)
    uint32_t addr = 0;  // AbsW (sign-extended) / AbsL address; PcDisp resolved address; PcIndex base (PC + d8)
    uint32_t imm = 0;   // Imm value (already truncated to the operand size)

    bool is_mem() const { return mode != Mode::None && mode != Mode::Dn && mode != Mode::An && mode != Mode::Imm; }
};

enum class Op : uint8_t {
    Illegal, LineA, LineF,
    // immediate / bit group
    Ori, Andi, Subi, Addi, Eori, Cmpi,
    OriCcr, OriSr, AndiCcr, AndiSr, EoriCcr, EoriSr,
    Btst, Bchg, Bclr, Bset, // src: Imm or Dn
    Movep,
    // moves
    Move, Movea, Moveq, MoveFromSr, MoveToCcr, MoveToSr, MoveUsp, Movem,
    // misc
    Negx, Clr, Neg, Not, Nbcd, Tst, Tas, Ext, Swap, Pea, Lea, Chk,
    Link, Unlk, Trap, Trapv, Reset, Nop, Stop, Rte, Rts, Rtr, Jsr, Jmp,
    // quick / branches
    Addq, Subq, Scc, Dbcc, Bra, Bsr, Bcc,
    // arithmetic / logic
    Or, And, Eor, Add, Sub, Cmp, Adda, Suba, Cmpa, Cmpm,
    Addx, Subx, Abcd, Sbcd, Mulu, Muls, Divu, Divs, Exg,
    // shifts and rotates (register form: src Imm count or Dn; memory form: src None)
    Asl, Asr, Lsl, Lsr, Roxl, Roxr, Rol, Ror,
};

struct Insn {
    uint32_t addr = 0;
    uint32_t length = 0; // bytes, opcode and extension words
    uint16_t opcode = 0;
    Op op = Op::Illegal;
    uint8_t size = 0;   // operand size in bytes: 1, 2, 4 (0: unsized)
    Ea src, dst;
    uint8_t cond = 0;   // Bcc / DBcc / Scc condition (0-15)
    uint16_t mask = 0;  // Movem register list as encoded (reversed for -(An))
    bool to_mem = false; // Movem/Movep register-to-memory; MoveUsp An->USP
    uint32_t target = 0; // Bra/Bsr/Bcc/Dbcc target
    uint32_t data = 0;   // Trap vector, Stop/Link immediate
};

// Reads the 16-bit big-endian word at a guest address, or nullopt if the
// address is not in the image.
using ReadWord = std::function<std::optional<uint16_t>(uint32_t addr)>;

// Decodes the instruction at addr. Returns false when a word is unmapped;
// an undefined opcode decodes as Op::Illegal (or LineA / LineF).
bool decode(uint32_t addr, const ReadWord &read, Insn &out);

// Text in MAME's m68k disassembler format (m68kdasm.cpp), so decodes can be
// checked against it word for word.
std::string format_mame(const Insn &in);

// Control flow.
bool is_branch(const Insn &in);      // Bra, Bcc, Dbcc, Bsr: direct target
bool ends_block(const Insn &in);     // no fall-through: Bra, Jmp, Rts, Rte, Rtr, Illegal, Line*, Stop...
bool is_indirect(const Insn &in);    // Jmp / Jsr whose target is not known at recompile time

} // namespace m68k
