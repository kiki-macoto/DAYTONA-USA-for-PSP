// PSP self-test: the floating-point results the game depends on, before any
// game code runs on the Allegrex (as platform/dreamcast/selftest). The i960
// code's fast paths go through host double, which the Allegrex has only in
// software (its FPU is single precision; GCC's soft-float library does
// double), and the TGP's through host float, in the FPU; both must be
// bit-exact. Also the int32_t override the build needs (newlib has it as
// long on this target).
//
// Expected values were computed on the PC (IEEE 754, round to nearest).
// Results go to the screen, stdout (PPSSPP's log) and selftest.log next to
// EBOOT.PBP, which build_psp.py reads.

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspsdk.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

PSP_MODULE_INFO("DaytonaSelftest", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024); // as the game: all the RAM but 1 MB

static_assert(sizeof(double) == 8, "the game needs a 64-bit double");
static_assert(sizeof(float) == 4);
static_assert(sizeof(void *) == 4);
static_assert(std::is_same_v<int32_t, int>, "int32_t must be int (the Makefile's INT32 override)");

namespace {

int failures = 0;
FILE *g_log = nullptr;

void line(const char *text) {
    std::printf("%s\n", text);
    pspDebugScreenPrintf("%s\n", text);
    if (g_log) std::fprintf(g_log, "%s\n", text);
}

uint64_t bits(double v) { uint64_t b; std::memcpy(&b, &v, 8); return b; }
uint32_t bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

void check(const char *what, uint64_t got, uint64_t want) {
    char text[96];
    std::snprintf(text, sizeof text, "%-14s %016llx %s", what, (unsigned long long)got, got == want ? "ok" : "FAIL");
    if (got != want) ++failures;
    line(text);
}

// A denormal result: IEEE gives `ieee`; an FPU that flushes gives zero.
// Either is reported, neither fails here: the game does not depend on a
// denormal (tools/ftzcheck, platform/dreamcast/HANDOFF.md).
void denormal(const char *what, uint64_t got, uint64_t ieee) {
    char text[96];
    std::snprintf(text, sizeof text, "%-14s %016llx %s", what, (unsigned long long)got,
                  got == ieee ? "ieee" : got == 0 ? "flushed" : "FAIL");
    if (got != ieee && got != 0) ++failures;
    line(text);
}

} // namespace

int main(int argc, char **argv) {
    pspSdkDisableFPUExceptions(); // as the game (game/main.cpp): IEEE results, no traps
    pspDebugScreenInit();
    std::string dir = argc > 0 && argv[0] ? argv[0] : "";
    dir = dir.substr(0, dir.find_last_of('/') + 1);
    sceIoRemove((dir + "selftest.log").c_str()); // (PPSSPP: fopen's "w" did not truncate)
    g_log = std::fopen((dir + "selftest.log").c_str(), "w");

    // volatile: computed on the PSP at run time, not folded by the compiler
    volatile double one = 1.0, two = 2.0, three = 3.0, tenth = 0.1, fifth = 0.2, a = 1.1, b = 3.3;
    volatile double seven = 7.0, twentytwo = 22.0, tiny = 1e-308, small = 1e-10;
    volatile float onef = 1.0f, threef = 3.0f, twof = 2.0f, af = 1.1f, bf = 3.3f, tinyf = 1e-38f, smallf = 1e-3f;

    line("Daytona USA recomp: PSP self-test");
    check("d 1/3", bits(one / three), 0x3fd5555555555555ull);
    check("d sqrt 2", bits(std::sqrt(double(two))), 0x3ff6a09e667f3bcdull);
    check("d 0.1+0.2", bits(tenth + fifth), 0x3fd3333333333334ull);
    check("d 1.1*3.3", bits(a * b), 0x400d0a3d70a3d70aull);
    check("d 22/7", bits(twentytwo / seven), 0x4009249249249249ull);
    check("f 1/3", bits(float(onef / threef)), 0x3eaaaaabu);
    check("f 1.1*3.3", bits(float(af * bf)), 0x406851ecu);
    check("f sqrt 2", bits(std::sqrt(float(twof))), 0x3fb504f3u);
    denormal("f denormal", bits(float(tinyf * smallf)), 0x00001be0u);
    denormal("d denormal", bits(tiny * small), 0x00000000000316a2ull);

    // printf of a double: newlib's formatting, as the runtime's logs use it
    char formatted[32], text[96];
    std::snprintf(formatted, sizeof formatted, "%.6f %.3e", double(one / three), double(twentytwo / seven));
    const bool printf_ok = !std::strcmp(formatted, "0.333333 3.143e+00");
    if (!printf_ok) ++failures;
    std::snprintf(text, sizeof text, "printf double  \"%s\" %s", formatted, printf_ok ? "ok" : "FAIL");
    line(text);

    // C++ exceptions and the heap (the runtime throws on errors; the game
    // needs tens of MB: PSP-2000 and later, large-memory mode).
    bool caught = false;
    try {
        throw std::string("x");
    } catch (const std::string &) {
        caught = true;
    }
    if (!caught) ++failures;
    line(caught ? "exceptions     ok" : "exceptions     FAIL");
    size_t mb = 0;
    for (size_t m = 64; m > 0 && !mb; --m) {
        void *volatile p = std::malloc(m << 20); // (volatile: GCC elides an unused malloc as successful)
        if (p) {
            std::free(p);
            mb = m;
        }
    }
    std::snprintf(text, sizeof text, "largest block  %u MB %s", unsigned(mb), mb >= 40 ? "ok" : "FAIL (large memory?)");
    if (mb < 40) ++failures;
    line(text);

    std::snprintf(text, sizeof text, "%d failure%s", failures, failures == 1 ? "" : "s");
    line(text);
    line(failures ? "SELFTEST FAIL" : "SELFTEST PASS");
    if (g_log) std::fclose(g_log);
    line("Press START to quit.");
    for (;;) {
        SceCtrlData pad{};
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START) break;
    }
    sceKernelExitGame();
    return 0;
}
