// The PSP's Media Engine (the second Allegrex, beside the one the game runs
// on) running a function of ours: audio.h mixes the sound on it.
//
// Started as mcidclan's psp-media-engine-custom-core starts it
// (https://github.com/mcidclan/psp-media-engine-custom-core, read at
// 0197c99e; its meLibHandler, meLibReset and cache routines), in kernel mode
// through kubridge's kuKernelCall instead of its own kernel module: a
// handler copied to the Media Engine's reset vector (0xbfc00000), the
// engine reset (0xbc10004c = 0x14, then 0). The handler, on the engine:
// bus clocks on (0xbc100050 = 0x0f), 0xbc100040 |= 2, the NMI flags cleared
// (0xbc100004), main RAM and the hardware registers opened to it
// (0xbc000000-0xbc000044 = -1), the FPU on (Status 0x30000000), a stack in
// its own eDRAM (0x80400000 when the system's ME image's witness word at
// 0x88300018 is 0x279c637c, else 0x80200000), then a jump to entry() at its
// cached kernel address. The system's own Media Engine code is gone after
// this (nothing in the game uses it; the sound output then uses the plain
// 44.1 kHz channel rather than the sample-rate converter, in case that leans
// on it). Not run in PPSSPP (it does not run Media Engine code).
//
// The engine has caches of its own (8 KB walked here, as the library does):
// what it reads must be in memory (written back by the game's processor),
// and what it writes is in memory only once it writes its cache back.
//
// Adapted parts: MIT License, Copyright (c) 2026 mcidclan (m-c/d, m-cid).
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions: The above copyright notice and this permission
// notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
#pragma once

#include <pspkernel.h>

#include <cstdint>

extern "C" {
struct PspMeKernelCallArg { // kubridge's KernelCallArg
    uint32_t arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12;
    uint32_t ret1, ret2;
};
int kuKernelCall(void *func_addr, PspMeKernelCallArg *args);
extern const uint32_t psp_me_handler[], psp_me_handler_end[];
void psp_me_entry();
}

namespace psp::me {

// The function the engine runs (forever), set before start.
inline void (*g_entry)() = nullptr;

// On the engine: its whole data cache written back and invalidated, its
// instruction cache invalidated (by index, as the library's).
inline void dcache_writeback_invalidate_all() {
    __asm__ __volatile__("sync" ::: "memory");
    for (uint32_t i = 0; i < 8192; i += 64) __asm__ __volatile__("cache 0x14, 0(%0)\ncache 0x14, 0(%0)" ::"r"(i));
    __asm__ __volatile__("sync" ::: "memory");
}
inline void icache_invalidate_all() {
    __asm__ __volatile__("sync" ::: "memory");
    for (uint32_t i = 0; i < 8192; i += 64) __asm__ __volatile__("cache 0x04, 0(%0)\ncache 0x04, 0(%0)" ::"r"(i));
    __asm__ __volatile__("sync" ::: "memory");
}
// A wait that touches no memory: about 8,000 cycles (25 us at 333 MHz) of
// a register count. (Reading an uncached flag back to back kept the bus
// busy: the game's processor waited on memory twice as long, its core 11 ms
// a frame became 29.)
inline void pause() {
    uint32_t n = 2048;
    __asm__ __volatile__(".set push\n.set noreorder\n1: bnez %0, 1b\naddiu %0, %0, -1\n.set pop" : "+r"(n));
}

// Kernel mode (kuKernelCall): the handler to the reset vector, the reset.
inline int boot_kernel() {
    volatile uint32_t *to = reinterpret_cast<volatile uint32_t *>(0xbfc00000u);
    for (const uint32_t *from = psp_me_handler; from < psp_me_handler_end; ++from) *to++ = *from;
    volatile uint32_t &reset = *reinterpret_cast<volatile uint32_t *>(0xbc10004cu);
    __asm__ __volatile__("sync" ::: "memory");
    reset = 0x14;
    reset = 0;
    __asm__ __volatile__("sync" ::: "memory");
    return 0;
}

// Starts entry on the engine; true once *alive (uncached, set by entry)
// turns non-zero within timeout_us.
inline bool start(void (*entry)(), volatile uint32_t *alive, uint32_t timeout_us) {
    g_entry = entry;
    *alive = 0;
    sceKernelDcacheWritebackInvalidateAll(); // (the code and g_entry in memory for the engine)
    sceKernelIcacheInvalidateAll();
    PspMeKernelCallArg args = {};
    kuKernelCall(reinterpret_cast<void *>(&boot_kernel), &args);
    for (uint32_t waited = 0; waited < timeout_us; waited += 1000) {
        if (*alive) return true;
        sceKernelDelayThread(1000);
    }
    return *alive != 0;
}

} // namespace psp::me

// The handler (copied to 0xbfc00000: position-independent but for la).
__asm__(".pushsection .text\n"
        ".set push\n.set noreorder\n.set noat\n"
        ".align 2\n.global psp_me_handler\npsp_me_handler:\n"
        "lui $k0, 0xbc10\n"
        "li $k1, 0x0f\nsw $k1, 0x50($k0)\n"              // bus clocks on
        "lw $k1, 0x40($k0)\nori $k1, $k1, 2\nsw $k1, 0x40($k0)\n"
        "li $k1, -1\nsw $k1, 0x04($k0)\n"                // NMI flags cleared
        "sync\n"
        "lui $k0, 0xbc00\nli $k1, -1\n"                  // main RAM and hardware registers opened
        "sw $k1, 0x00($k0)\nsw $k1, 0x04($k0)\nsw $k1, 0x08($k0)\nsw $k1, 0x0c($k0)\n"
        "sw $k1, 0x10($k0)\nsw $k1, 0x14($k0)\nsw $k1, 0x18($k0)\nsw $k1, 0x1c($k0)\n"
        "sw $k1, 0x20($k0)\nsw $k1, 0x24($k0)\nsw $k1, 0x28($k0)\nsw $k1, 0x2c($k0)\n"
        "sw $k1, 0x30($k0)\nsw $k1, 0x34($k0)\nsw $k1, 0x38($k0)\nsw $k1, 0x3c($k0)\n"
        "sw $k1, 0x40($k0)\nsw $k1, 0x44($k0)\n"
        "sync\n"
        "li $k0, 0x30000000\nmtc0 $k0, $12\nsync\n"       // Status: CU0, CU1 (the FPU)
        "li $k0, 0x279c637c\nlui $k1, 0x8830\nlw $k1, 0x18($k1)\n"
        "bne $k0, $k1, 1f\n"
        "lui $sp, 0x8020\n"                              // (the delay slot: 0x80200000)
        "lui $sp, 0x8040\n"
        "1:\n"
        "lui $k0, %hi(psp_me_entry)\naddiu $k0, $k0, %lo(psp_me_entry)\n"
        "lui $k1, 0x8000\nor $k0, $k0, $k1\n"
        "cache 0x8, 0($k0)\nsync\n"
        "jr $k0\nnop\n"
        ".global psp_me_handler_end\npsp_me_handler_end:\n"
        ".set pop\n.popsection\n");

// On the engine: its caches cleared, then entry, forever. (Named by the
// handler's asm alone, so kept by "used". This header is in main.cpp only.)
extern "C" __attribute__((used, noinline)) void psp_me_entry() {
    psp::me::dcache_writeback_invalidate_all();
    psp::me::icache_invalidate_all();
    // (At its cached kernel address, as the handler jumps here: the calls
    // from it then stay in that segment, as the library's code runs.)
    reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(psp::me::g_entry) | 0x80000000u)();
    for (;;) psp::me::pause();
}
