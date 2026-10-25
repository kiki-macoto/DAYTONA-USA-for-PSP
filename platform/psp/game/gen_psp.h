// The PSP's memory-access helpers for the generated i960 code
// (scripts/psp_gen.py rewrites the plain loads and stores to call them).
//
// fast_gen.py's code for a load or store does, inline, at each of about
// 13,000 places:
//
//     c.m_IP = IP; [countdown]; ls.pending = n;
//     { const uint32_t epoch = ls.epoch; ACCESS; if (ls.epoch != epoch) left = 1; }
//
// eleven MIPS instructions besides the access. On a console the game's frame
// is instruction-cache bound, and these are the same few instructions at
// every place: here they run once, in a helper that stays in the cache. The
// call site passes the address, the value, the IP and n, and sets left = 1
// when the helper says the lockstep's next event moved, as before.
//
// The loads return the value in the low word and "the event moved" in bit
// 32 (both in registers on the Allegrex); the stores return the flag.
// Defined out of line (gen_psp.cpp): inlined, they would be the code they
// replace.
#pragma once

#include "runtime/gen_support.h"

namespace gen {

uint64_t psp_ld32(Env &e, uint32_t addr, uint32_t ip, uint32_t n);
uint64_t psp_ld16(Env &e, uint32_t addr, uint32_t ip, uint32_t n);
uint64_t psp_ld8(Env &e, uint32_t addr, uint32_t ip, uint32_t n);
bool psp_st32(Env &e, uint32_t addr, uint32_t value, uint32_t ip, uint32_t n);
bool psp_st16(Env &e, uint32_t addr, uint16_t value, uint32_t ip, uint32_t n);
bool psp_st8(Env &e, uint32_t addr, uint8_t value, uint32_t ip, uint32_t n);
// call to a constant address (ip: the return IP, which the inline code set
// just before do_call) and ret, the same way.
bool psp_call(Env &e, uint32_t target, uint32_t sp, uint32_t ip, uint32_t n);
bool psp_ret(Env &e, uint32_t ip, uint32_t n);

// psp_gen.py's dual_modes: which version of a chunk runs (the fast one,
// counting the lockstep down a basic block at a time, or the precise one,
// an instruction at a time), whether the one that returned handed over (run
// then calls the other at the same IP), and the count until the next event
// it handed over (0: none).
inline bool g_precise = false, g_handoff = false;
inline uint32_t g_left = 0;

} // namespace gen
