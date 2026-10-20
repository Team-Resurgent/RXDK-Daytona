// Support for the recompiled sound 68000 (tools/m2sndrecomp output): the
// generated code is native C++ with every instruction's operands resolved at
// recompile time; it calls the snd::Cpu68k helpers for memory, flags and
// interrupts, and snd::Sched at instruction boundaries.
#pragma once

#include "runtime/snd_cpu.h"
#include "runtime/snd_sched.h"

#include <cstdint>

namespace sndgen {

struct Env {
    snd::Cpu68k &c;
    snd::Sched &s;
};

bool has_code(uint32_t addr);   // generated
void run(Env &e);               // generated: runs from c.pc until it leaves recompiled code or the schedule ends
uint64_t native_instructions(); // generated: instructions recompiled (all of them)

} // namespace sndgen
