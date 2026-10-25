// The PSP frontend (PSP-2000 and later: 64 MB, about 52 MB of it for the
// program in its large-memory mode). It follows the Dreamcast's
// (platform/dreamcast/game/main.cpp): the runtime with the Dreamcast's
// memory layout and speed paths (M2_DC_MEMORY, M2_DC_SPIN_SKIP,
// M2_DC_SPEED), ROM read through a page cache (rt::RomSource), here from the
// Memory Stick, with most of the RAM left as the cache; texture RAM in main
// RAM (the GE reads textures from there); every kDrawEvery-th frame drawn by
// the GE (renderer.h); the desktop's native audio (audio.h).
//
// Files, next to EBOOT.PBP: rom/ (the importer's images from your own ROM
// set and the sound program and PCM ROMs, copied there by build_psp.py),
// save/ (the settings EEPROM and backup RAM, written when they change and
// at exit; rom/ may hold a desktop copy to start from) and daytona.log.

// The runtime first (as on the Dreamcast, before the platform's headers).
#include "runtime/game_loop.h"
#include "runtime/rom_source.h"

#include "../../../tools/common/input_script.h"
#include "audio.h"
#include "controls.h"
#include "inputs.h" // build_psp.py: kInputs (a recorded input script, or ""), kDrawEvery
#include "renderer.h"

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspsdk.h>
#include <pspsuspend.h>
#include <pspsysmem.h>

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

// The custom firmware's (SystemCtrlForUser, KUBridge; ARK exports both):
// shown when the extra memory is missing.
extern "C" {
int sctrlHENGetVersion();
int sctrlHENGetMinorVersion();
int kuKernelInitApitype();
void sctrlHENSetSpeed(int cpu, int bus); // (the CPU clock past 333 MHz: see set_cpu_clock)
#ifdef M2_PSP_PROFILE
void *kuKernelMemcpy(void *dest, const void *src, size_t num); // (kernel memory: the profiler's registers)
void kuKernelPokew(void *addr, unsigned int value);
#endif
}

PSP_MODULE_INFO("DaytonaRecomp", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024); // the runtime and the generated code expect a desktop stack
PSP_HEAP_SIZE_KB(-1024);             // all the RAM but 1 MB (the system's own needs)

FILE *g_log = nullptr; // daytona.log next to EBOOT.PBP

// Large C++ allocations (256 KB and up) 64-byte aligned: the tile layers are
// read by the GE as textures (it needs 16), and a cache line is 64 bytes.
// A failure is logged with its size and caller (psp-addr2line on game.elf).
void *operator new(size_t size) {
    if (void *p = size >= 256 * 1024 ? memalign(64, size) : std::malloc(size ? size : 1)) return p;
    char line[96];
    std::snprintf(line, sizeof line, "ALLOC FAILED %u bytes from %08lx\n", unsigned(size),
                  (unsigned long)__builtin_return_address(0));
    std::fputs(line, stdout);
    if (g_log) std::fputs(line, g_log);
    throw std::bad_alloc();
}
// (GCC warns that free is called on what operator new returned: this
// operator new is malloc and memalign, so free is right.)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop

#ifdef M2_PSP_PROFILE
namespace rt { // m2_tgp_board.cpp (Makefile PROFILE_TGP=1): the TGP's time
extern uint64_t (*g_tgp_profile_clock)();
extern uint64_t g_tgp_profile_us;
extern uint64_t g_tgp_profile_runs;
extern void (*g_tgp_profile_enter)(), (*g_tgp_profile_leave)();
// geo.cpp: the geometrizer's parse, on frames with objects [1] and without [0],
// and model2_3d_process_polygon (1 call in 16 timed).
extern uint64_t (*g_geo_profile_clock)();
extern void (*g_geo_profile_enter)(), (*g_geo_profile_leave)();
extern uint64_t g_geo_mode_objects[4], g_geo_mode_links[4];
extern uint64_t g_geo_parse_us[2], g_geo_parses[2];
extern uint64_t g_geo_polygon_calls, g_geo_polygon_sampled_us, g_geo_polygon_samples, g_geo_section_us[4];
} // namespace rt
#define TGP_PROFILE_MS(f) (double(rt::g_tgp_profile_us) / (f))
#else
#define TGP_PROFILE_MS(f) 0.0
#endif

#ifdef M2_PSP_PROFILE
namespace {
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
} // namespace

// The CPU's hardware profiler (pspdebug.h's PspDebugProfilerRegs at
// 0xBC400000: cycles, stall cycles, cache misses, loads and stores),
// kernel memory, read through the custom firmware's kubridge. The counters
// are 32 bits (the cycles wrap in 13 s at 333 MHz): read every frame, the
// differences summed. PPSSPP does not model them (hw_ok stays false there,
// or the figures are zero).
struct HwCounters {
    static constexpr int kN = 19; // the registers after `enable`
    static constexpr const char *kNames[kN] = {
        "systemck", "cpuck", "internal", "memory", "copz", "vfpu", "sleep", "bus_access", "uncached_load",
        "uncached_store", "cached_load", "cached_store", "i_miss", "d_miss", "d_writeback", "cop0_inst", "fpu_inst",
        "vfpu_inst", "local_bus"};
    uint32_t now[kN] = {};
    // [0] the game (run_frame; without the TGP's runs while they are timed,
    // and without the geometrizer's parses with objects), [1] drawing
    // (renderer.draw), [2] the TGP's runs (timed reports only), [3] those
    // parses (geo.cpp's hooks)
    uint64_t sum[4][kN] = {};
    bool ok = false;

    bool read(uint32_t (&to)[kN]) {
        uint32_t regs[kN + 1];
        if (kuKernelMemcpy(regs, reinterpret_cast<const void *>(0xBC400000u), sizeof regs) != regs) return false;
        std::memcpy(to, regs + 1, sizeof to);
        return true;
    }
    void start() {
        kuKernelPokew(reinterpret_cast<void *>(0xBC400000u), 1);
        uint32_t a[kN], b[kN];
        bool got = read(a);
        sceKernelDelayThread(10000);
        got = got && read(b);
        if (!got) {
            say("HW profiler not available (kuKernelMemcpy did not read it)\n");
            return;
        }
        ok = b[1] - a[1] > 0; // cpuck moved
        say("HW profiler %s: %u CPU cycles in 10 ms\n", ok ? "on" : "not counting", unsigned(b[1] - a[1]));
        mark();
    }
    void mark() {
        if (ok) read(now);
    }
    // Adds what the counters moved since the last mark to `part`, and marks.
    void add(int part) {
        if (!ok) return;
        uint32_t v[kN];
        if (!read(v)) return;
        for (int i = 0; i < kN; i++) sum[part][i] += uint32_t(v[i] - now[i]);
        std::memcpy(now, v, sizeof now);
    }
    // One line per part: thousands a frame (per drawn frame for drawing).
    // The TGP's line only when its runs were counted apart (tgp_timed: the
    // game's line then leaves them out).
    void report(int frames, int drawn, int geo_frames, bool tgp_timed) {
        if (!ok) return;
        for (int part = 0; part < 4; part++) {
            if (part == 2 && !tgp_timed) continue;
            static const char *const names[] = {"game", "draw", "TGP", "geometry"};
            char line[512];
            int n = std::snprintf(line, sizeof line, "HW %s%s, thousands per %s:", names[part],
                                  part == 0 ? (tgp_timed ? " without the TGP or the geometry" : " with the TGP, without the geometry") : "",
                                  part == 1 ? "drawn frame" : part == 3 ? "frame with objects" : "frame");
            const double per = part == 1 ? std::max(1, drawn) : part == 3 ? std::max(1, geo_frames) : std::max(1, frames);
            for (int i = 0; i < kN && n < int(sizeof line) - 40; i++)
                n += std::snprintf(line + n, sizeof line - n, " %s %.1f", kNames[i], sum[part][i] / per / 1000.0);
            say("%s\n", line);
            for (uint64_t &s : sum[part]) s = 0;
        }
    }
};
// The TGP's runs counted apart (m2_tgp_board's hooks, set with its clock):
// what the game's part had so far is added to it, the run to the TGP's.
HwCounters *g_hw = nullptr;
static void hw_tgp_enter() { g_hw->add(0); }
static void hw_tgp_leave() { g_hw->add(2); }
static void hw_geo_enter() { g_hw->add(0); }
static void hw_geo_leave() { g_hw->add(3); }
#endif

// The TGP's clock: two reads a TGP run, a few thousand a frame. The
// system time's low word (microseconds, wraps in 71 minutes; m2_tgp_board
// takes the difference in 32 bits) rather than sceRtcGetCurrentTick: both
// are system calls, CLOCK at the start logs what each costs.
static uint64_t tgp_clock() { return sceKernelGetSystemTimeLow(); }

#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kReport = 300;       // frames between PROFILE and GAME lines (a recorded race; x6 with the pad)
constexpr int kFrames = 6000;      // a recorded script: then GAME DONE (a race is 6,000 frames); the pad: no end
constexpr int kDrawDistance = 0;   // the runtime's draw distance (-2..2): 0, the game's own
constexpr size_t kReserve = 8u << 20;   // RAM left free after the ROM cache: the runtime's own growth
constexpr size_t kNeeded = 33u << 20;   // free at start (volatile memory included) for the full mode (see run_game)
constexpr size_t kNeededLow = 20u << 20;  // ... for the low-memory mode
constexpr size_t kReserveLow = 12u << 20; // the low-memory mode's reserve: the game's construction (8.5 MB),
                                          // growth in a race and the decoded samples (in a 24 MB PPSSPP: a
                                          // whole race left 1.9 MB; with 11, the attract mode left 0.9)
constexpr size_t kLowSampleCache = 3u << 19; // the low-memory mode's decoded samples (1.5 MB; else 3 MB)
// The ROM cache at most: the reads of every run together are 14.5 MB
// (platform/dreamcast/HANDOFF.md). In PPSSPP the heap is 49 MB; with a
// 26 MB cache the constructed game left 0 MB free.
constexpr size_t kMaxCache = 16u << 20;
// The cars in a race (the player's car included), by course. Revision A's
// race set-up (i960 0x4600-0x4620) takes the count from the main data ROM,
// a word per course at 0x02805278 + 4 * course (through the pointer table at
// 0x02805240; course 0 beginner, 1 expert, 2 advanced, as the laps table at
// 0x02805248 shows: 8, 2, 4), and keeps it at work RAM 0x501080 (count - 1
// at 0x501084). Found with m2run --ram-dump; m2run with these values on all
// three courses: POSITION /20, 10/10, 10/10, the race and GAME OVER as usual;
// the beginner course with 10 too (2026-10-25: /10, the race as usual).
struct Cars { uint32_t offset, arcade, psp; };
constexpr Cars kCars[3] = {{0x805278, 40, 10}, {0x80527c, 30, 10}, {0x805280, 20, 10}};
constexpr double kArcadeHz = 57.52;     // the game's own frame rate: never run faster than this
constexpr int kSaveEvery = 3600;        // frames between checks for changed settings to save
constexpr size_t kCredits = 0x128;      // backup RAM: the credits (Revision A; see load_nvram)
constexpr int kMaxDrawEvery = 4;        // the adaptive draw rate's lowest: every 4th frame (the runtime's limit)
constexpr int kAdaptFrames = 60;        // frames measured before the draw rate is adapted
// Up a step when the frames take over this share of the arcade's frame
// (the game then runs up to 8% slow rather than draw less often: the
// player's choice of smoothness), down a step under kAdaptDown (by the rate
// now: drawing more often costs more, so the margin is larger from every
// 4th than from every 3rd).
constexpr double kAdaptUp = 1.08;
constexpr double kAdaptDown[kMaxDrawEvery + 1] = {0, 0, 0, 0.85, 0.75};
// kDrawEvery 0 (build_psp.py --draw-every 0): adaptive from every 2nd, a
// recorded script too (to measure it); otherwise a recorded script keeps
// kDrawEvery and the buttons adapt from it.
constexpr int kMinDrawEvery = kDrawEvery ? kDrawEvery : 2;

std::string g_dir; // the program's folder (ms0:/PSP/GAME/DAYTONA/), with the slash
volatile bool g_exit = false; // the HOME menu's exit

// The watchdog (a freeze on a console, not seen in PPSSPP): where the game
// thread is, written by it; read by watchdog_thread.
enum Phase : int { kPhaseStart, kPhaseGeWait, kPhaseInput, kPhaseGame, kPhaseSound, kPhaseDraw, kPhasePace, kPhaseSave,
                   kPhaseReport };
const char *const kPhaseNames[] = {"start", "GE wait (renderer.finish)", "input", "game (run_frame)", "sound",
                                   "draw (renderer.draw)", "frame pacing", "save", "report"};
volatile int g_phase = kPhaseStart;
volatile uint32_t g_frames_done = 0;
volatile uint32_t g_board_frame = 0, g_draw_every = 0;
rt::GameLoop *volatile g_game = nullptr; // its i960 and TGP counts, read racily (a diagnosis)
SceUID g_game_thread = -1;

bool save_file(const std::string &path, const uint8_t *data, size_t size);

// Memory Stick writes off the game thread. A write stopped the game for as
// long as the stick took (reported on a console: a short freeze with the
// stick's light): the log was flushed every line, and the backup RAM saved
// every minute (its operating time always changes). The game thread queues
// log lines and saves; this thread writes them, in order. Its priority is
// above the game thread's: it waits in the stick's driver, which leaves the
// CPU to the game, and a lower one would not run while the game is behind
// (it never waits then).
class StickWriter {
public:
    void start() {
        lock_ = sceKernelCreateSema("writer_lock", 0, 1, 1, nullptr);
        work_ = sceKernelCreateSema("writer_work", 0, 0, 1 << 30, nullptr);
        if (lock_ < 0 || work_ < 0) return;
        thread_ = sceKernelCreateThread("writer", thread_main, 0x18, 64 * 1024, PSP_THREAD_ATTR_USER, nullptr);
        if (thread_ < 0) return;
        StickWriter *self = this;
        sceKernelStartThread(thread_, sizeof self, &self);
    }
    bool running() const { return thread_ >= 0; }
    void log(const char *line) {
        sceKernelWaitSema(lock_, 1, nullptr);
        log_ += line;
        sceKernelSignalSema(lock_, 1);
        sceKernelSignalSema(work_, 1);
    }
    void save(std::string path, std::vector<uint8_t> data) {
        sceKernelWaitSema(lock_, 1, nullptr);
        saves_.push_back({std::move(path), std::move(data)});
        sceKernelSignalSema(lock_, 1);
        sceKernelSignalSema(work_, 1);
    }
    // Waits until everything queued is written, then ends the thread (the
    // exit; the log is the caller's again).
    void stop() {
        if (!running()) return;
        sceKernelWaitSema(lock_, 1, nullptr);
        stopping_ = true;
        sceKernelSignalSema(lock_, 1);
        sceKernelSignalSema(work_, 1);
        sceKernelWaitThreadEnd(thread_, nullptr);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
    }

private:
    struct Save { std::string path; std::vector<uint8_t> data; };
    static int thread_main(SceSize, void *argp) {
        StickWriter *self = *static_cast<StickWriter **>(argp);
        std::string text;
        std::vector<Save> saves;
        for (;;) {
            sceKernelWaitSema(self->work_, 1, nullptr);
            sceKernelWaitSema(self->lock_, 1, nullptr);
            text.swap(self->log_);
            saves.swap(self->saves_);
            const bool stopping = self->stopping_;
            sceKernelSignalSema(self->lock_, 1);
            // The saves first: a line queued with them (GAME DONE, which
            // build_psp.py waits for) is then written after them.
            for (const Save &s : saves)
                if (!save_file(s.path, s.data.data(), s.data.size()) && g_log)
                    std::fprintf(g_log, "GAME could not save %s\n", s.path.c_str());
            saves.clear();
            if (!text.empty() && g_log) std::fputs(text.c_str(), g_log);
            if (g_log) std::fflush(g_log);
            text.clear();
            // (stop() set stopping_ after everything was queued: this pass
            // took it all.)
            if (stopping) return 0;
        }
    }
    SceUID lock_ = -1, work_ = -1, thread_ = -1;
    std::string log_;
    std::vector<Save> saves_;
    bool stopping_ = false;
};
StickWriter g_writer;

void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void say(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char line[512];
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    std::fputs(line, stdout); // (PPSSPP's log shows the program's stdout)
    if (g_writer.running()) {
        g_writer.log(line);
    } else if (g_log) {
        std::fputs(line, g_log);
        std::fflush(g_log);
    }
}

// The ROM regions from the Memory Stick, a page at a time, through an LRU
// cache whose pages are in `storage`. A miss reads one page, or kRun pages
// when it carries on from the last read in that region (as the Dreamcast's
// DiscRom, which this is with sceIo reads instead of the CD).
class StickRom : public rt::RomSource {
public:
    StickRom(uint8_t *storage, size_t cache_pages) : slots_(cache_pages), data_(storage) {
        static const char *names[] = {"program", "main_data", "polygons", "textures", "copro_data"};
        for (int r = 0; r < 5; r++) {
            const std::string path = g_dir + "rom/" + names[r] + ".bin";
            files_[r] = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
            if (files_[r] < 0) throw std::runtime_error("cannot open " + path);
            sizes_[r] = uint32_t(sceIoLseek(files_[r], 0, PSP_SEEK_END));
            index_[r].assign(sizes_[r] >> kPageBits, -1);
            used_[r].assign(sizes_[r] >> kPageBits, 0);
        }
    }
    ~StickRom() override {
        for (SceUID f : files_)
            if (f >= 0) sceIoClose(f);
    }
    uint32_t size(rt::RomRegion region) const override { return sizes_[int(region)]; }
    const uint8_t *page(rt::RomRegion region, uint32_t index) override {
        const int r = int(region);
        int32_t &slot = index_[r][index];
        if (slot < 0) load(r, index);
        Slot &s = slots_[size_t(slot)];
        s.used = ++clock_;
        uint8_t &u = used_[r][index];
        if (!u) {
            u = 1;
            ++used_count_;
        }
        return &data_[size_t(slot) * kPageSize];
    }
    uint64_t misses = 0, pages_read = 0, read_us = 0;

    // 32-bit little-endian words changed as their pages are read (set before
    // anything is read; the files stay as they are). Each is changed only
    // where it holds `expected` (another ROM revision: left alone).
    struct Patch { int region; uint32_t offset, expected, value; };
    std::vector<Patch> patches;

    // The pages the game has used (one 32-bit word a page, region << 24 |
    // index): saved as save/rom_pages.bin and read at the next start, where
    // they are loaded before the game runs (preload): a miss stops the game
    // for the stick's read, the start's reads are in long runs.
    bool used_changed() const { return used_count_ != saved_count_; }
    void used_saved() { saved_count_ = used_count_; }
    std::vector<uint8_t> used_list() const {
        std::vector<uint8_t> out;
        out.reserve(used_count_ * 4);
        for (uint32_t r = 0; r < 5; r++)
            for (uint32_t i = 0; i < used_[r].size(); i++)
                if (used_[r][i]) {
                    const uint32_t w = r << 24 | i;
                    for (int b = 0; b < 4; b++) out.push_back(uint8_t(w >> (8 * b)));
                }
        return out;
    }
    // Loads the listed pages (sorted, in runs of up to kPreloadRun read at
    // once, straight into the slots: the cache fills in order) while the
    // cache has room, keeping an eighth (at most 1 MB) free for the pages
    // not listed. They count as used. Returns the pages read.
    size_t preload(std::vector<uint32_t> pages) {
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        const size_t limit = slots_.size() - std::min(slots_.size() / 8, size_t(256));
        size_t i = 0, read = 0;
        while (i < pages.size() && filled_ < limit) {
            const uint32_t r = pages[i] >> 24, first = pages[i] & 0xffffff;
            if (r >= 5 || first >= index_[r].size() || index_[r][first] >= 0) {
                ++i;
                continue;
            }
            uint32_t n = 1;
            while (n < kPreloadRun && i + n < pages.size() && pages[i + n] == pages[i] + n && filled_ + n < limit &&
                   first + n < index_[r].size() && index_[r][first + n] < 0)
                ++n;
            sceIoLseek(files_[r], SceOff(first) << kPageBits, PSP_SEEK_SET);
            if (sceIoRead(files_[r], &data_[filled_ * kPageSize], int(n * kPageSize)) != int(n * kPageSize))
                throw std::runtime_error("ROM read failed (preload)");
            patch(int(r), first, n, &data_[filled_ * kPageSize]);
            for (uint32_t k = 0; k < n; k++) {
                const size_t slot = filled_++;
                slots_[slot] = {int(r), first + k, ++clock_};
                index_[r][first + k] = int32_t(slot);
                if (!used_[r][first + k]) {
                    used_[r][first + k] = 1;
                    ++used_count_;
                }
            }
            i += n;
            read += n;
        }
        forget();
        saved_count_ = used_count_;
        return read;
    }

private:
    static constexpr uint32_t kRun = 8;
    static constexpr uint32_t kPreloadRun = 64; // 256 KB a read
    struct Slot { int region = -1; uint32_t index = 0; uint64_t used = 0; };

    void load(int r, uint32_t index) {
        ++misses;
        forget(); // pages may be evicted below: no remembered pointer may outlive them
        uint32_t n = 1;
        if (index == next_[r])
            while (n < kRun && index + n < index_[r].size() && index_[r][index + n] < 0) ++n;
        next_[r] = index + n;
        const uint64_t t0 = psp::now_us();
        sceIoLseek(files_[r], SceOff(index) << kPageBits, PSP_SEEK_SET);
        const int got = sceIoRead(files_[r], run_, int(n * kPageSize));
        read_us += psp::now_us() - t0;
        if (got != int(n * kPageSize)) throw std::runtime_error("ROM read failed");
        patch(r, index, n, run_);
        for (uint32_t k = 0; k < n; k++) {
            const size_t slot = victim();
            Slot &s = slots_[slot];
            if (s.region >= 0) index_[s.region][s.index] = -1;
            s = {r, index + k, ++clock_};
            index_[r][index + k] = int32_t(slot);
            std::memcpy(&data_[slot * kPageSize], &run_[size_t(k) * kPageSize], kPageSize);
            ++pages_read;
        }
    }
    // The patches that fall in pages first..first+n-1 of region r, read to `at`.
    void patch(int r, uint32_t first, uint32_t n, uint8_t *at) {
        for (const Patch &p : patches) {
            const uint32_t lo = first << kPageBits, hi = (first + n) << kPageBits;
            if (p.region != r || p.offset < lo || p.offset + 4 > hi) continue;
            uint8_t *w = at + (p.offset - lo);
            uint32_t v;
            std::memcpy(&v, w, 4);
            if (v == p.expected) std::memcpy(w, &p.value, 4);
        }
    }
    // A free slot while there are any (the cache fills in order), then the
    // least recently used.
    size_t victim() {
        if (filled_ < slots_.size()) return filled_++;
        size_t best = 0;
        for (size_t i = 1; i < slots_.size(); i++)
            if (slots_[i].used < slots_[best].used) best = i;
        return best;
    }

    SceUID files_[5] = {-1, -1, -1, -1, -1};
    uint32_t sizes_[5] = {};
    uint32_t next_[5] = {};
    std::vector<int32_t> index_[5]; // page -> cache slot, or -1
    std::vector<uint8_t> used_[5];  // page -> used by the game (or preloaded)
    size_t used_count_ = 0, saved_count_ = 0;
    std::vector<Slot> slots_;
    size_t filled_ = 0;
    uint8_t *data_;
    uint64_t clock_ = 0;
    alignas(64) uint8_t run_[kRun * kPageSize];
};

// The PCM ROMs from the Memory Stick (without the extra memory): their sample
// tables read at start, a sample's bytes when it is first played (audio.h).
class StickPcm {
public:
    StickPcm() {
        for (int r = 0; r < 2; r++) {
            const std::string path = g_dir + "rom/pcm" + char('1' + r) + ".bin";
            files_[r] = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
            if (files_[r] < 0) throw std::runtime_error("cannot open " + path);
            sizes_[r] = size_t(sceIoLseek(files_[r], 0, PSP_SEEK_END));
            headers_[r].resize(psp::FastSoundEngine::kHeaderBytes);
            if (!read(unsigned(r), 0, headers_[r].data(), headers_[r].size()))
                throw std::runtime_error("cannot read " + path);
        }
        reads = read_us = 0;
    }
    ~StickPcm() {
        for (SceUID f : files_)
            if (f >= 0) sceIoClose(f);
    }
    StickPcm(const StickPcm &) = delete;
    StickPcm &operator=(const StickPcm &) = delete;

    size_t size(unsigned rom) const { return sizes_[rom]; }
    std::vector<uint8_t> header(unsigned rom) const { return headers_[rom]; }
    bool read(unsigned rom, uint32_t offset, uint8_t *to, size_t n) {
        const uint64_t t0 = psp::now_us();
        ++reads;
        const bool ok = sceIoLseek(files_[rom], SceOff(offset), PSP_SEEK_SET) == SceOff(offset) &&
                        sceIoRead(files_[rom], to, int(n)) == int(n);
        read_us += psp::now_us() - t0;
        return ok;
    }
    uint64_t reads = 0, read_us = 0;

private:
    SceUID files_[2] = {-1, -1};
    size_t sizes_[2] = {};
    std::vector<uint8_t> headers_[2];
};

bool exists(const std::string &path) {
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

std::vector<uint8_t> load_file(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> d(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) throw std::runtime_error("cannot read " + path);
    std::fclose(f);
    return d;
}

bool save_file(const std::string &path, const uint8_t *data, size_t size) {
    const std::string tmp = path + ".new";
    sceIoRemove(tmp.c_str()); // (fopen's "w" may not truncate: see main)
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok) return false;
    sceIoRemove(path.c_str());
    return sceIoRename(tmp.c_str(), path.c_str()) >= 0;
}

// tools::Script::load's format, without iostreams (as the Dreamcast's). The
// script's "frames N" line is returned (kFrames if it has none).
int parse_script(const char *text, tools::Script &script) {
    int frames = kFrames;
    for (const char *line = text; *line;) {
        const char *end = std::strchr(line, '\n');
        const std::string s(line, end ? size_t(end - line) : std::strlen(line));
        line = end ? end + 1 : line + s.size();
        if (s.compare(0, 6, "frames") == 0) frames = std::atoi(s.c_str() + 6);
        if (s.empty() || s[0] == '#' || s.compare(0, 6, "frames") == 0) continue;
        const char *p = s.c_str();
        char *after = nullptr;
        tools::Script::Line l;
        l.from = std::strtoull(p, &after, 10);
        l.to = *after == '-' ? std::strtoull(after + 1, &after, 10) : l.from;
        while (*after == ' ' || *after == '\t') ++after;
        const char *eq = std::strchr(after, '=');
        if (!eq) continue;
        l.name.assign(after, size_t(eq - after));
        l.value = unsigned(std::strtoul(eq + 1, nullptr, 0));
        script.lines.push_back(l);
    }
    return frames > 0 ? frames : kFrames;
}

double heap_mb() { return mallinfo().uordblks / 1048576.0; }

// The largest block malloc can still give, to a megabyte (probed). The
// pointer goes through a volatile: GCC removes a malloc whose result is only
// tested and freed, taking it as successful (the first version reported
// 64 MB, then 128, whatever the heap held).
size_t largest_free() {
    for (size_t mb = 64; mb > 0; --mb) {
        void *volatile p = std::malloc(mb << 20);
        if (p) {
            std::free(p);
            return mb << 20;
        }
    }
    return 0;
}

// RAM outside the heap. The heap is one block of the user memory, the
// largest free one at the first malloc; the user memory may have more than
// one free block (the extra memory added on to it), and the others are only
// reached through the kernel. kKernelSlack is left for the system's own
// needs (thread stacks). Blocks are kept to the end (the program's exit
// frees them).
constexpr size_t kKernelSlack = 1u << 20;

size_t outside_free() {
    const size_t m = sceKernelMaxFreeMemSize();
    return m > kKernelSlack + 64 ? m - kKernelSlack - 64 : 0;
}

uint8_t *outside_alloc(size_t size, const char *name) {
    if (outside_free() < size) return nullptr;
    const SceUID id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, name, PSP_SMEM_Low, size + 64, nullptr);
    if (id < 0) return nullptr;
    const auto a = reinterpret_cast<uintptr_t>(sceKernelGetBlockHeadAddr(id));
    return reinterpret_cast<uint8_t *>((a + 63) & ~uintptr_t(63));
}

// The 4 MB of "volatile" memory every PSP has besides the user memory (the
// system's buffer for sleep mode, free while a program runs): texture RAM.
uint8_t *volatile_mem() {
    void *p = nullptr;
    int size = 0;
    if (sceKernelVolatileMemTryLock(0, &p, &size) != 0) return nullptr;
    if (size >= 0x400000) return static_cast<uint8_t *>(p);
    sceKernelVolatileMemUnlock(0);
    return nullptr;
}

// The settings EEPROM and backup RAM: save/ if the PSP has saved them, else
// rom/ (a copy from the desktop's data folder, put there by build_psp.py),
// else the game's factory settings.
struct Nvram {
    std::string eeprom_path, backup_path;
    std::vector<uint8_t> eeprom, backup; // as last saved
};

// A recorded race (fixed) starts from rom/ only and saves nothing: the
// records a race writes to the backup RAM change the next run (a second
// race_basic went to a time attack), and the run must match the desktop's.
void load_nvram(rt::GameLoop &game, Nvram &nv, bool fixed) {
    if (!fixed) sceIoMkdir((g_dir + "save").c_str(), 0777);
    nv.eeprom_path = fixed ? std::string() : g_dir + "save/ioboard_eeprom.bin";
    nv.backup_path = fixed ? std::string() : g_dir + "save/backup_ram.bin";
    auto pick = [](const std::string &saved, const std::string &start) {
        return !saved.empty() && exists(saved) ? saved : exists(start) ? start : std::string();
    };
    const std::string e = pick(nv.eeprom_path, g_dir + "rom/ioboard_eeprom.bin");
    const std::string b = pick(nv.backup_path, g_dir + "rom/backup_ram.bin");
    auto &eeprom = game.board().io().eeprom;
    auto &backup = game.board().backup_ram();
    if (!e.empty()) {
        const auto d = load_file(e);
        if (d.size() == eeprom.size()) std::copy(d.begin(), d.end(), eeprom.begin());
    }
    if (!b.empty()) {
        const auto d = load_file(b);
        if (d.size() == backup.size()) std::copy(d.begin(), d.end(), backup.begin());
    }
    // No credit at power-on (the player's choice; the cabinet keeps them):
    // credits left from the last session would start a game straight from
    // the attract mode. The bookkeeping (Revision A, backup RAM 0x100-0x17f
    // and its copy at 0x180; found by m2run runs with coins and starts):
    // 0x110 and 0x118 coins, 0x128 credits, 0x138 and 0x13c plays, 0x108
    // operating time, 0x104 a checksum (formula not known). The credits
    // byte is cleared in both copies; the game then finds the checksum
    // wrong and clears the whole bookkeeping (coins, plays, time: test mode's
    // BOOKKEEPING), the records and settings elsewhere are kept (checked on
    // the desktop: m2run from such a backup RAM shows the attract mode with
    // CREDIT 0/1 and saves a zeroed block).
    if (!fixed && backup.size() >= 0x200 && (backup[kCredits] || backup[kCredits + 0x80])) {
        say("GAME %u credits left: cleared (with the bookkeeping)\n", unsigned(backup[kCredits]));
        backup[kCredits] = backup[kCredits + 0x80] = 0;
    }
    nv.eeprom.assign(eeprom.begin(), eeprom.end());
    nv.backup.assign(backup.begin(), backup.end());
    say("GAME settings from %s, records from %s\n", e.empty() ? "the factory" : e.c_str(),
        b.empty() ? "the factory" : b.c_str());
}

// Write whichever of the two changed since it was last saved (queued for
// the writer thread; a failed write is logged there and not tried again
// until the data changes).
void save_nvram(rt::GameLoop &game, Nvram &nv) {
    if (nv.eeprom_path.empty()) return; // a recorded race
    const auto &eeprom = game.board().io().eeprom;
    const auto &backup = game.board().backup_ram();
    auto save = [](const std::string &path, std::vector<uint8_t> &last, const uint8_t *data, size_t size) {
        last.assign(data, data + size);
        if (g_writer.running()) g_writer.save(path, last);
        else save_file(path, data, size);
    };
    if (!std::equal(eeprom.begin(), eeprom.end(), nv.eeprom.begin(), nv.eeprom.end())) {
        save(nv.eeprom_path, nv.eeprom, eeprom.data(), eeprom.size());
        say("GAME settings saved\n");
    }
    if (!std::equal(backup.begin(), backup.end(), nv.backup.begin(), nv.backup.end())) {
        save(nv.backup_path, nv.backup, backup.data(), backup.size());
        say("GAME backup RAM saved\n");
    }
}

// The ROM pages used (StickRom::used_list), saved with the settings when
// new ones were used; recorded races too (the list changes nothing the game
// computes). rom/rom_pages.bin (build_psp.py, from recorded runs) and
// save/rom_pages.bin are read at the start.
void save_rom_pages(StickRom &rom) {
    if (!rom.used_changed()) return;
    rom.used_saved();
    const std::string path = g_dir + "save/rom_pages.bin";
    if (g_writer.running()) g_writer.save(path, rom.used_list());
    else {
        const std::vector<uint8_t> list = rom.used_list();
        save_file(path, list.data(), list.size());
    }
}

std::vector<uint32_t> load_rom_pages(const std::string &path) {
    std::vector<uint32_t> pages;
    if (!exists(path)) return pages;
    const std::vector<uint8_t> d = load_file(path);
    for (size_t i = 0; i + 4 <= d.size(); i += 4)
        pages.push_back(uint32_t(d[i]) | uint32_t(d[i + 1]) << 8 | uint32_t(d[i + 2]) << 16 | uint32_t(d[i + 3]) << 24);
    return pages;
}

int watchdog_thread(SceSize, void *);

void run_game() {
    const size_t free_at_start = largest_free(); // (the first malloc: the heap is made)
    // Texture RAM (tex0, tex1, 4 MB): the volatile memory, else outside the
    // heap, else in it. The game's writes reach only the first 1 MB of each
    // sheet; the other halves are the renderer's texture cache (as on the
    // Dreamcast: nothing reads them; dcmemcheck --fill-upper matches the
    // desktop for a whole race). No frame buffer RAM: the game never writes it.
    uint8_t *texture_ram = volatile_mem();
    const char *texture_where = "volatile memory";
    const size_t outside_at_start = outside_free();
    say("GAME start, heap %.2f MB, largest free block %u MB, outside the heap %u KB (kernel's total free %u KB), "
        "volatile memory %s\n",
        heap_mb(), unsigned(free_at_start >> 20), unsigned(outside_at_start >> 10),
        unsigned(sceKernelTotalFreeMemSize() >> 10), texture_ram ? "4 MB" : "not available");
    // The PSP-2000/3000/Go's extra memory (MEMSIZE=1 in PARAM.SFO) gives the
    // program about 49 MB; without it (a firmware that does not unlock it)
    // it has about 17, the PSP-1000's 24 MB less the program. Measured in
    // PPSSPP: 22.6 MB besides the ROM cache once constructed (texture RAM
    // included), 26.3 MB in a race (decoded samples, the scrolled layer).
    // Without kNeeded, the low-memory mode: the PCM ROMs read from the
    // Memory Stick (8 MB less), a smaller decoded-sample cache, and the ROM
    // cache what is left (more reads from the stick).
    const size_t have = free_at_start + outside_at_start + (texture_ram ? 0x400000 : 0);
    const bool low_memory = have < kNeeded;
    if (low_memory && have < kNeededLow) {
        const unsigned cfw = unsigned(sctrlHENGetVersion()), cfw_minor = unsigned(sctrlHENGetMinorVersion());
        const unsigned apitype = unsigned(kuKernelInitApitype());
        say("GAME not enough RAM; CFW version %x.%x, apitype 0x%x\n", cfw, cfw_minor, apitype);
        char what[600];
        std::snprintf(what, sizeof what,
                      "Not enough RAM: %u MB free, about %u MB needed.\n"
                      "(largest block %u MB, other blocks %u MB, volatile %u MB;\n"
                      " CFW version %x.%x, apitype 0x%x)\n\n"
                      "The game needs a PSP-2000, 3000 or Go.",
                      unsigned(have >> 20), unsigned(kNeededLow >> 20), unsigned(free_at_start >> 20),
                      unsigned(outside_at_start >> 20), texture_ram ? 4u : 0u, cfw, cfw_minor, apitype);
        throw std::runtime_error(what);
    }
    if (low_memory) say("GAME low-memory mode: sound samples read from the Memory Stick\n");
    if (!texture_ram) {
        texture_ram = outside_alloc(0x400000, "texture_ram");
        texture_where = "outside the heap";
    }
    if (!texture_ram) {
        texture_ram = static_cast<uint8_t *>(memalign(64, 0x400000));
        texture_where = "the heap";
    }
    if (!texture_ram) throw std::runtime_error("no RAM for texture RAM");
    say("GAME texture RAM in %s\n", texture_where);
    std::memset(texture_ram, 0, 0x400000);
    psp::Renderer renderer({{texture_ram + 0x100000, 0x100000}, {texture_ram + 0x300000, 0x100000}});
    // overlay.bin next to EBOOT.PBP, when there is one: the picture at the
    // sides of the screen (build_psp.py --overlay, scripts/make_overlay.py).
    if (FILE *f = std::fopen((g_dir + "overlay.bin").c_str(), "rb")) {
        const size_t bytes = psp::Renderer::overlay_bytes();
        auto *pixels = static_cast<uint16_t *>(memalign(64, bytes));
        const bool whole = pixels && std::fread(pixels, 1, bytes, f) == bytes && std::fgetc(f) == EOF;
        std::fclose(f);
        if (whole) renderer.set_overlay(pixels);
        say(whole ? "GAME overlay.bin shown\n" : "GAME overlay.bin left out: not 480 x 272 RGB565 (%u bytes)\n",
            unsigned(bytes));
        std::free(pixels);
    }

    // The sound, before the ROM cache takes what is left: the sound program
    // and both PCM ROMs (8 MB) in RAM, or in the low-memory mode read from
    // the stick (stick_pcm outlives audio: declared first).
    std::unique_ptr<StickPcm> stick_pcm;
    std::unique_ptr<psp::Audio> audio;
    // The Media Engine mixes the sound (audio.h) unless settings.txt says
    // "me 0", or it does not answer (PPSSPP does not run its code). On the
    // console it slows the core a little (2-3 ms a frame in a busy race) but
    // takes 3-4 ms of mixing off: about 1.5 ms a frame better (HANDOFF.md).
    psp::MeLink *me = nullptr;
    int me_wanted = 1;
    if (FILE *f = std::fopen((g_dir + "settings.txt").c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, f))
            if (line[0] != '#') std::sscanf(line, " me %d", &me_wanted);
        std::fclose(f);
    }
    if (me_wanted && !kInputs[0]) { // (a recorded run keeps the 32 kHz sound it was checked with)
        if (void *block = memalign(64, sizeof(psp::MeLink))) {
            std::memset(block, 0, sizeof(psp::MeLink));
            sceKernelDcacheWritebackInvalidateRange(block, sizeof(psp::MeLink));
            auto *link = reinterpret_cast<psp::MeLink *>(reinterpret_cast<uintptr_t>(block) | 0x40000000u);
            psp::g_me_link = link;
            if (psp::me::start(&psp::me_worker, &link->alive, 200000)) me = link;
        }
        say(me ? "GAME Media Engine started: it mixes the sound\n"
               : "GAME Media Engine did not answer in 200 ms: the sound is mixed here\n");
    }
    try {
        if (low_memory) {
            stick_pcm = std::make_unique<StickPcm>();
            StickPcm *pcm = stick_pcm.get();
            audio = std::make_unique<psp::Audio>(
                load_file(g_dir + "rom/sound_program.bin"), pcm->header(0), pcm->size(0), pcm->header(1), pcm->size(1),
                [pcm](unsigned r, uint32_t offset, uint8_t *to, size_t n) { return pcm->read(r, offset, to, n); },
                kLowSampleCache, me);
        } else {
            audio = std::make_unique<psp::Audio>(load_file(g_dir + "rom/sound_program.bin"),
                                                 load_file(g_dir + "rom/pcm1.bin"), load_file(g_dir + "rom/pcm2.bin"),
                                                 me);
        }
        say("GAME sound on, %u Hz, heap %.2f MB\n", audio->rate(), heap_mb());
    } catch (const std::exception &e) {
        say("GAME sound off: %s\n", e.what());
    }

    // The ROM page cache: outside the heap, or in the heap what is left but
    // the reserve (held while the cache is sized: a probe of the largest
    // block says nothing of what the runtime's many smaller allocations will
    // find), whichever is larger. In the low-memory mode the reserve is all
    // the rest of the game's needs and the cache has no cap of its own.
    void *reserve = std::malloc(low_memory ? kReserveLow : kReserve);
    if (!reserve) throw std::runtime_error("not enough RAM (a PSP-2000 or later is needed)");
    const size_t cap = low_memory ? kMaxCache * 2 : kMaxCache;
    const size_t free_now = largest_free();
    const size_t heap_bytes = std::min(cap, free_now > (1u << 20) ? free_now - (1u << 20) : 0);
    const size_t outside_bytes = std::min(cap, outside_free());
    const bool outside = outside_bytes > heap_bytes;
    const size_t cache_pages = (outside ? outside_bytes : heap_bytes) / rt::RomSource::kPageSize;
    const size_t cache_size = cache_pages * rt::RomSource::kPageSize;
    uint8_t *cache = nullptr;
    if (cache_pages >= 512)
        cache = outside ? outside_alloc(cache_size, "rom_cache") : static_cast<uint8_t *>(memalign(64, cache_size));
    std::free(reserve);
    if (!cache) throw std::runtime_error("no RAM for the ROM cache");
    auto rom = std::make_unique<StickRom>(cache, cache_pages);
    say("GAME ROM cache: %u pages (%.1f MB) in %s\n", unsigned(cache_pages), cache_size / 1048576.0,
        outside ? "a block outside the heap" : "the heap");
    // Fewer cars in a race (the player's request: the console is slower with
    // the whole field). Played games only: a recorded race must match the
    // desktop's.
    if (kInputs[0] == 0) {
        for (const auto &c : kCars)
            rom->patches.push_back({int(rt::RomRegion::MainData), c.offset, c.arcade, c.psp});
        say("GAME cars in a race: beginner %u, advanced %u, expert %u (the arcade: 40, 20, 30)\n", kCars[0].psp,
            kCars[2].psp, kCars[1].psp);
    }
    {
        sceIoMkdir((g_dir + "save").c_str(), 0777);
        std::vector<uint32_t> pages = load_rom_pages(g_dir + "rom/rom_pages.bin");
        const size_t shipped = pages.size();
        const std::vector<uint32_t> own = load_rom_pages(g_dir + "save/rom_pages.bin");
        pages.insert(pages.end(), own.begin(), own.end());
        const uint64_t p0 = psp::now_us();
        const size_t read = rom->preload(std::move(pages));
        say("GAME ROM pages preloaded: %u (%.1f MB) in %.1f s; listed %u in rom/, %u in save/\n", unsigned(read),
            read * double(rt::RomSource::kPageSize) / 1048576.0, (psp::now_us() - p0) / 1e6, unsigned(shipped),
            unsigned(own.size()));
    }
    if (!rom->patches.empty()) // (as the game will read them)
        say("GAME cars read back from the ROM: beginner %u, advanced %u, expert %u\n",
            unsigned(rom->dword(rt::RomRegion::MainData, kCars[0].offset)),
            unsigned(rom->dword(rt::RomRegion::MainData, kCars[2].offset)),
            unsigned(rom->dword(rt::RomRegion::MainData, kCars[1].offset)));

    rt::M2Board::Images img;
    img.copro_tables = load_file(g_dir + "rom/copro_tables.bin");
    img.rom = rom.get();
    img.texture_ram = texture_ram;
    rt::GameLoop game(std::move(img), false);
    game.board().video().set_external_3d(true); // the GE draws the 3D
    // The race's scrolled scenery layer is drawn by the GE from a texture
    // (renderer.h, as the Dreamcast's): the runtime leaves it out of the back
    // layer, which it then need not compose again as the scenery scrolls.
    game.board().video().set_scroll_layer(true);
    // The desktop's draw mode: the screen (the tile layers) is updated every
    // draw_every-th frame, the one drawn; the game runs every frame. With the
    // buttons, draw_every adapts (from kDrawEvery up to kMaxDrawEvery) so the
    // game keeps the arcade's speed (below); a recorded script keeps
    // kDrawEvery, to compare with the desktop.
    int draw_every = kMinDrawEvery;
    game.set_frame_skip(draw_every - 1);
    rt::GameLoop::set_draw_distance(kDrawDistance);
    game.set_profile_clock([]() -> uint64_t { return psp::now_us(); });
#ifdef M2_PSP_PROFILE
    // What a clock read costs (the console's system calls are not PPSSPP's):
    // the TGP's timing is that times two per run. It is on in every other
    // report only, so the reports without it show the core as it is.
    {
        constexpr int n = 2000;
        uint64_t sink = 0;
        const uint64_t c0 = psp::now_us();
        for (int i = 0; i < n; i++) sink += psp::now_us();
        const uint64_t c1 = psp::now_us();
        for (int i = 0; i < n; i++) sink += tgp_clock();
        const uint64_t c2 = psp::now_us();
        say("CLOCK sceRtcGetCurrentTick %.2f us a call, sceKernelGetSystemTimeLow %.2f us a call (%u)\n",
            double(c1 - c0) / n, double(c2 - c1) / n, unsigned(sink & 1));
    }
    rt::g_tgp_profile_clock = tgp_clock;
    rt::g_geo_profile_clock = tgp_clock;
    bool tgp_timed = true;
    if (audio) audio->set_profile_clock([]() -> uint64_t { return psp::now_us(); }); // (PROFILE audio: the mixing)
    HwCounters hw;
    hw.start();
    g_hw = &hw;
    auto set_tgp_hooks = [&](bool on) {
        rt::g_tgp_profile_enter = on && hw.ok ? hw_tgp_enter : nullptr;
        rt::g_tgp_profile_leave = on && hw.ok ? hw_tgp_leave : nullptr;
    };
    set_tgp_hooks(tgp_timed);
    rt::g_geo_profile_enter = hw.ok ? hw_geo_enter : nullptr;
    rt::g_geo_profile_leave = hw.ok ? hw_geo_leave : nullptr;
#endif
    const bool scripted = kInputs[0] != 0;
    Nvram nvram;
    load_nvram(game, nvram, scripted);

    tools::Script script;
    int frames = kFrames;
    if (scripted) {
        frames = parse_script(kInputs, script);
        say("GAME inputs from the recorded script (%u lines, %d frames)\n", unsigned(script.lines.size()), frames);
    }
    // Frame dumps (build_psp.py --dump-every N, a recorded script): a drawn
    // frame every N, to shots/.
    if (scripted && kDumpEvery) sceIoMkdir((g_dir + "shots").c_str(), 0777);
    char dump_path[256];
    psp::Controls controls;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    say("GAME constructed, heap %.2f MB, largest free block %u MB\n", heap_mb(), unsigned(largest_free() >> 20));
    g_game = &game;
    g_game_thread = sceKernelGetThreadId();
    sceIoRemove((g_dir + "freeze.log").c_str()); // (the last run's)
    const SceUID watchdog = sceKernelCreateThread("watchdog", watchdog_thread, 0x16, 32 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    if (watchdog >= 0) sceKernelStartThread(watchdog, 0, nullptr);

    const int report = scripted ? kReport : kReport * 6;
    uint64_t prof_core = 0, prof_geo = 0, prof_video = 0, prof_draw = 0, prof_audio = 0, prof_wait = 0;
    const uint64_t t0 = psp::now_us();
    uint64_t report_t0 = t0, next_us = t0;
    const uint64_t frame_us = uint64_t(1e6 / kArcadeHz);
    uint64_t busy_us = 0, frame_start = t0; // the frames' own time (not the pacing's wait), since the last adapt
    int busy_frames = 0, drawn_frames = 0;
    for (int frame = 1; !g_exit && (!scripted || frame <= frames); frame++) {
        // The draw rate, adapted every kAdaptFrames: up when the frames
        // take longer than the arcade's, down when there is room. Changed
        // only on a frame that every rate (2, 3, 4) draws (a multiple of
        // 12): the runtime's frame skip decides at each frame whether the
        // next one is drawn (geometrizer object parsing), so a change
        // elsewhere would lose one picture's polygons.
        if ((!scripted || !kDrawEvery) && busy_frames >= kAdaptFrames && game.board().frame() % 12 == 0) {
            const double busy = double(busy_us) / busy_frames;
            const int before = draw_every;
            if (busy > frame_us * kAdaptUp && draw_every < kMaxDrawEvery) ++draw_every;
            else if (busy < frame_us * kAdaptDown[draw_every] && draw_every > kMinDrawEvery) --draw_every;
            if (draw_every != before) game.set_frame_skip(draw_every - 1);
            busy_us = 0;
            busy_frames = 0;
        }
        // This frame composes the tile layers again: the GE must be done
        // with the last picture's.
        g_frames_done = uint32_t(frame - 1);
        g_board_frame = uint32_t(game.board().frame());
        g_draw_every = uint32_t(draw_every);
        g_phase = kPhaseGeWait;
        if (game.board().frame() % uint64_t(draw_every) == 0) renderer.finish();
        g_phase = kPhaseInput;
        rt::Inputs in;
        if (scripted) {
            in = script.at(game.board().frame());
        } else {
            SceCtrlData pad{};
            sceCtrlPeekBufferPositive(&pad, 1);
            const psp::Input c = controls.sample({pad.Buttons, pad.Lx, game.board().backup_ram()[kCredits] != 0, pad.Ly});
            in = {c.steer, c.accel, c.brake, c.in0, c.in1, c.in2};
        }
        g_phase = kPhaseGame;
#ifdef M2_PSP_PROFILE
        hw.mark();
        game.run_frame(in);
        hw.add(0);
#else
        game.run_frame(in);
#endif
        g_phase = kPhaseSound;
        {
            const rt::FrameProfile &fp = game.last_profile();
            prof_core += fp.core();
            prof_geo += fp.geometry;
            prof_video += fp.video;
        }
        const uint64_t a0 = psp::now_us();
        if (audio) audio->frame(game.board().take_sound_bytes());
        else (void)game.board().take_sound_bytes();
        prof_audio += psp::now_us() - a0;
        // The lockstep check: the same line as tools/tracecheck on the
        // desktop (platform/dreamcast/tools), with a recorded race only.
        if (scripted && frame % 60 == 0) {
            uint64_t h = 0xcbf29ce484222325ULL;
            const uint32_t *buffer = game.board().tgp().buffer_data();
            for (int i = 0; i < 0x8000; i++) h = (h ^ buffer[i]) * 0x100000001b3ULL;
            say("TRACE %d i960 %llu tgp %llu buffer %016llx\n", frame, (unsigned long long)game.instructions(),
                (unsigned long long)game.board().tgp().tgp_instructions(), (unsigned long long)h);
        }
        // Just after a screen update (vblank_end updates when frame %
        // draw_every is 0, then counts on).
        if (game.board().frame() % uint64_t(draw_every) == 1 % uint64_t(draw_every)) {
            renderer.dump_path = nullptr;
            ++drawn_frames;
            // (A drawn frame: every frame with --draw-every 1, the odd ones
            // with 2; m2run --frame-skip 1 --every N dumps the frame before.)
            if (scripted && kDumpEvery && game.board().frame() % kDumpEvery == 1 % uint64_t(draw_every)) {
                std::snprintf(dump_path, sizeof dump_path, "%sshots/frame_%05llu.raw", g_dir.c_str(),
                              (unsigned long long)game.board().frame());
                renderer.dump_path = dump_path;
            }
            const uint64_t d0 = psp::now_us();
            g_phase = kPhaseDraw;
#ifdef M2_PSP_PROFILE
            hw.mark();
            renderer.draw(game.board().video());
            hw.add(1);
#else
            renderer.draw(game.board().video());
#endif
            prof_draw += psp::now_us() - d0;
        }
        g_phase = kPhasePace;
        // Never faster than the arcade (a recorded race runs flat out).
        const uint64_t now = psp::now_us();
        busy_us += now - frame_start;
        ++busy_frames;
        next_us += frame_us;
        if (!scripted && next_us > now) {
            sceKernelDelayThread(SceUInt(next_us - now));
            prof_wait += next_us - now;
        } else if (now > next_us + 4 * frame_us) {
            next_us = now; // behind: do not try to catch up
        }
        frame_start = psp::now_us();
        g_phase = kPhaseSave;
        if (frame % kSaveEvery == 0) {
            save_nvram(game, nvram);
            save_rom_pages(*rom);
        }
        g_phase = kPhaseReport;
        if (frame % report == 0) {
            const double f = report * 1e3, drawn = std::max(1, drawn_frames) * 1e3; // us -> ms per frame
            say("PROFILE %d ms/frame: core %.1f (TGP %.1f) geometry %.1f video %.1f draw %.1f (%.1f each drawn frame) audio %.1f "
                "idle %.1f, total %.1f; every %d%s frame drawn now (%d drawn); frame wait skipped %.0f%% of i960 "
                "instructions\n",
                frame, prof_core / f, TGP_PROFILE_MS(f), prof_geo / f, prof_video / f, prof_draw / f, prof_draw / drawn, prof_audio / f,
                prof_wait / f, (psp::now_us() - report_t0) / f, draw_every,
                draw_every == 1 ? "st" : draw_every == 2 ? "nd" : draw_every == 3 ? "rd" : "th", drawn_frames,
                100.0 * double(game.board().spin_skipped()) / double(game.instructions()));
#ifdef M2_PSP_PROFILE
            hw.report(report, drawn_frames, int(rt::g_geo_parses[1]), tgp_timed);
            if (audio)
                say(audio->media_engine() ? "PROFILE audio, ms per frame: writing the mixing jobs %.1f (the rest of audio "
                                            "above: the sequencer)\n"
                                          : "PROFILE audio, ms per frame: mixing %.1f (the rest of audio above: the sequencer)\n",
                    double(audio->take_mix_us()) / f);
#endif
            if (audio && audio->media_engine())
                say("PROFILE Media Engine mixing: the game waiting for it %.2f ms per frame; %llu jobs did not fit\n",
                    double(audio->take_me_wait_us()) / 1000.0 / f, (unsigned long long)audio->job_overflows());
            else if (audio && audio->me_given_up)
                say("PROFILE Media Engine given up (a batch not done in 200 ms): the sound is mixed here\n");
            drawn_frames = 0;
            say("PROFILE draw, ms per drawn frame: layers %.1f sort+textures %.1f list %.1f GE wait %.1f; "
                "ROM reads %.2f ms/frame\n",
                renderer.step_us[0] / drawn, renderer.step_us[1] / drawn, renderer.step_us[2] / drawn,
                renderer.step_us[3] / drawn, rom->read_us / f);
            say("GAME frame %d: %u polygons drawn (%u textured, %u checkered, flat for want of a build %u / a palette %u, "
                "%u skipped, %u KB of list; %u palettes, %u textures, %u flushes, %u rebuilt), i960 %" PRIu64 " (%.1f s, ROM misses %" PRIu64
                ", pages read %" PRIu64 ", heap %.2f MB; sound: %u voices, %u KB of samples decoded, %llu underruns)\n",
                frame, renderer.drawn, renderer.textured, renderer.checkered, renderer.no_build, renderer.no_palette,
                renderer.skipped, unsigned(renderer.list_bytes / 1024), renderer.palettes(), unsigned(renderer.sources()),
                renderer.flushes, renderer.rebuilds, game.instructions(), (psp::now_us() - t0) / 1e6, rom->misses, rom->pages_read, heap_mb(),
                audio ? audio->voices() : 0u, audio ? unsigned(audio->cached_bytes() / 1024) : 0u,
                audio ? (unsigned long long)audio->underruns : 0ull);
            if (stick_pcm)
                say("GAME sound samples read from the stick: %llu (%.1f ms in all)\n",
                    (unsigned long long)stick_pcm->reads, stick_pcm->read_us / 1000.0);
            for (uint64_t &us : renderer.step_us) us = 0;
            prof_core = prof_geo = prof_video = prof_draw = prof_audio = prof_wait = 0;
#ifdef M2_PSP_PROFILE
            say("PROFILE TGP timing %s in this report: %.0f TGP runs a frame; the next report %s\n",
                tgp_timed ? "on" : "off (TGP 0.0 above; core without the clock reads)",
                double(rt::g_tgp_profile_runs) / report, tgp_timed ? "without it" : "with it");
            tgp_timed = !tgp_timed;
            rt::g_tgp_profile_clock = tgp_timed ? tgp_clock : nullptr;
            set_tgp_hooks(tgp_timed);
            rt::g_tgp_profile_us = 0;
            rt::g_tgp_profile_runs = 0;
            {
                const double with = double(std::max<uint64_t>(rt::g_geo_parses[1], 1));
                const double polygon_us = rt::g_geo_polygon_samples ? double(rt::g_geo_polygon_sampled_us) /
                                                                          rt::g_geo_polygon_samples *
                                                                          rt::g_geo_polygon_calls
                                                                    : 0.0;
                const double scale = rt::g_geo_polygon_samples
                                         ? double(rt::g_geo_polygon_calls) / rt::g_geo_polygon_samples / 1000.0 / with
                                         : 0.0;
                say("PROFILE geometry: %.1f ms each frame with objects (%llu), %.2f each without (%llu); %.0f polygons "
                    "a frame with objects, %.1f ms of it projecting and clipping them (1 in 16 timed: vertices and "
                    "culling %.1f, texture reads %.1f, clipping %.1f, writing the polygon %.1f)\n",
                    rt::g_geo_parse_us[1] / 1000.0 / with, (unsigned long long)rt::g_geo_parses[1],
                    rt::g_geo_parse_us[0] / 1000.0 / double(std::max<uint64_t>(rt::g_geo_parses[0], 1)),
                    (unsigned long long)rt::g_geo_parses[0], double(rt::g_geo_polygon_calls) / with,
                    polygon_us / 1000.0 / with, rt::g_geo_section_us[0] * scale, rt::g_geo_section_us[1] * scale,
                    rt::g_geo_section_us[2] * scale, rt::g_geo_section_us[3] * scale);
                say("PROFILE geometry modes, objects (links) a frame with objects: np_ns %.0f (%.0f), np_s %.0f (%.0f), "
                    "nn_ns %.0f (%.0f), nn_s %.0f (%.0f)\n",
                    rt::g_geo_mode_objects[0] / with, rt::g_geo_mode_links[0] / with, rt::g_geo_mode_objects[1] / with,
                    rt::g_geo_mode_links[1] / with, rt::g_geo_mode_objects[2] / with, rt::g_geo_mode_links[2] / with,
                    rt::g_geo_mode_objects[3] / with, rt::g_geo_mode_links[3] / with);
                for (uint64_t &n : rt::g_geo_mode_objects) n = 0;
                for (uint64_t &n : rt::g_geo_mode_links) n = 0;
                for (uint64_t &us : rt::g_geo_section_us) us = 0;
                rt::g_geo_parse_us[0] = rt::g_geo_parse_us[1] = rt::g_geo_parses[0] = rt::g_geo_parses[1] = 0;
                rt::g_geo_polygon_calls = rt::g_geo_polygon_sampled_us = rt::g_geo_polygon_samples = 0;
            }
#endif
            rom->read_us = 0;
            report_t0 = psp::now_us();
        }
    }
    g_game = nullptr; // (the watchdog: the game goes at the end of this function)
#ifdef M2_PSP_PROFILE
    set_tgp_hooks(false); // (hw goes at the end of this function)
    rt::g_geo_profile_enter = rt::g_geo_profile_leave = nullptr;
    g_hw = nullptr;
#endif
    g_phase = kPhaseStart;
    renderer.finish();
    save_nvram(game, nvram);
    save_rom_pages(*rom);
    say("GAME DONE\n");
}

// The watchdog: when the game thread has finished no frame for 3 s, it
// writes freeze.log next to EBOOT.PBP (opened and closed each time: the
// console may then only be switched off), saying where the game thread is
// (its phase, its thread status) and whether the i960 and TGP code still
// run (their counts 1 s apart). Rewritten every 2 s while it lasts.
int watchdog_thread(SceSize, void *) {
    uint32_t last = g_frames_done;
    int still = 0, reports = 0;
    uint64_t i960_before = 0, tgp_before = 0;
    while (!g_exit) {
        sceKernelDelayThread(1000 * 1000);
        rt::GameLoop *game = g_game;
        const uint64_t i960 = game ? game->instructions() : 0;
        const uint64_t tgp = game ? game->board().tgp().tgp_instructions() : 0;
        const uint32_t done = g_frames_done;
        if (done != last || !game) {
            last = done;
            still = 0;
        } else if (++still >= 3 && still % 2 == 1 && reports < 20) {
            SceKernelThreadInfo info{};
            info.size = sizeof info;
            const int refer = sceKernelReferThreadStatus(g_game_thread, &info);
            char text[640];
            const int n = std::snprintf(
                text, sizeof text,
                "FREEZE (report %d): no frame finished for %d s after game-thread frame %u (board frame %u, every %u "
                "drawn)\nphase: %s\ni960 instructions %llu -> %llu, TGP instructions %llu -> %llu (1 s apart)\n"
                "game thread: refer %d, status 0x%x, wait type %d, wait id 0x%x, priority 0x%x\n",
                reports + 1, still, unsigned(done), unsigned(g_board_frame), unsigned(g_draw_every),
                kPhaseNames[g_phase], (unsigned long long)i960_before, (unsigned long long)i960,
                (unsigned long long)tgp_before, (unsigned long long)tgp, refer, unsigned(info.status), info.waitType,
                unsigned(info.waitId), unsigned(info.currentPriority));
            // (No malloc here: the game thread may be stuck holding its lock.)
            char path[256];
            std::snprintf(path, sizeof path, "%sfreeze.log", g_dir.c_str());
            sceIoRemove(path);
            const SceUID f = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (f >= 0) {
                sceIoWrite(f, text, std::min(n, int(sizeof text) - 1));
                sceIoClose(f);
            }
            ++reports;
        }
        i960_before = i960;
        tgp_before = tgp;
    }
    return 0;
}

// The CPU clock (the bus at half). settings.txt next to EBOOT.PBP may ask
// for one ("cpu 400", 222-444 MHz); without it, the clock the custom
// firmware already runs at when that is past 333 (its overclock: the game
// used to set 333 over it), else 333, the system's highest. Past 333 MHz
// only the custom firmware can set it (sctrlHENSetSpeed; pspdev's stubs
// have no sctrlHENGetSpeed: scePower reads the clock), and ARK-5 on a
// PSP-3000 did not (asked 440, it stayed at 333). Returns a line for the
// log.
std::string set_cpu_clock() {
    int asked = 0;
    if (FILE *f = std::fopen((g_dir + "settings.txt").c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, f))
            if (line[0] != '#') std::sscanf(line, " cpu %d", &asked);
        std::fclose(f);
    }
    const int before = scePowerGetCpuClockFrequency();
    const int mhz = asked ? std::clamp(asked, 222, 444) : before > 333 ? before : 333;
    const char *how = asked ? "settings.txt" : mhz > 333 ? "the custom firmware's overclock, kept" : "the default";
    if (!asked && before >= 333) {
        // Already there: not set again. ARK-5's overclock left 333 to read
        // here, and setting 333 over it ran the game ~25% slower.
        if (before == 333) how = "already 333 (the custom firmware's), left as it is";
    } else if (mhz > 333) {
        if (mhz != before) sctrlHENSetSpeed(mhz, mhz / 2);
        // Not set (no such call: PPSSPP left 222): 333 rather than slower.
        if (scePowerGetCpuClockFrequency() < 333) {
            scePowerSetClockFrequency(333, 333, 166);
            how = "the custom firmware did not set it: 333";
        } else if (scePowerGetCpuClockFrequency() < mhz) {
            how = "the custom firmware refused it"; // (ARK-5 on a PSP-3000 stayed at 333 for 440)
        }
    } else {
        scePowerSetClockFrequency(mhz, mhz, mhz / 2);
    }
    // The real speed, whatever scePower reads: a loop of 20 million taken
    // branches with an add in the delay slot, timed. 4 cycles an iteration
    // on the console (PSP-3000, ARK-5, at its full speed: 165 MHz read with
    // 2); PPSSPP takes 2, and reads double.
    unsigned n = 20000000;
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    __asm__ __volatile__(".set push\n.set noreorder\n1: bnez %0, 1b\naddiu %0, %0, -1\n.set pop" : "+r"(n));
    const uint32_t us = std::max<uint32_t>(sceKernelGetSystemTimeLow() - t0, 1);
    char text[240];
    std::snprintf(text, sizeof text, "CPU %d MHz, bus %d MHz (asked %d: %s; it was %d); measured about %u MHz",
                  scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency(), mhz, how, before,
                  unsigned(80000000ull / us));
    return text;
}

int exit_callback(int, int, void *) {
    g_exit = true;
    return 0;
}
int callback_thread(SceSize, void *) {
    const int cb = sceKernelCreateCallback("exit", exit_callback, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

// An error before or instead of the game, on the screen until HOME and Exit.
// (The renderer, if it was made, has been destroyed by then: the debug
// screen sets up the display itself.)
void show_error(const char *what) {
    pspDebugScreenInit();
    pspDebugScreenPrintf("Daytona USA recomp (PSP)\n\n%s\n\n", what);
    pspDebugScreenPrintf("rom/ next to EBOOT.PBP must hold the files build_psp.py copies there\n");
    pspDebugScreenPrintf("from your own ROM set (see platform/psp/README.md).\n\nPress HOME to quit.\n");
    while (!g_exit) sceDisplayWaitVblankStart();
}

} // namespace

int main(int argc, char **argv) {
    // The FPU's exceptions off (IEEE results: infinity, NaN), as on the
    // desktop and in PPSSPP, which does not model them. A console traps a
    // float divide by zero or invalid operation by default, and with no
    // handler the whole PSP stops: a PSP-3000 froze just before the race
    // started (no watchdog report), where PPSSPP played on. Per thread: the
    // audio thread does the same.
    pspSdkDisableFPUExceptions();
    const SceUID cb = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0x1000, 0, nullptr);
    if (cb >= 0) sceKernelStartThread(cb, 0, nullptr);
    // The folder EBOOT.PBP was started from (argv[0] is its path).
    g_dir = argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/DAYTONA/EBOOT.PBP";
    g_dir = g_dir.substr(0, g_dir.find_last_of('/') + 1);
    const std::string clock = set_cpu_clock();
    // Removed first: in PPSSPP (1.19.3) fopen's "w" did not truncate, and a
    // shorter run's lines sat on top of the last run's.
    sceIoRemove((g_dir + "daytona.log").c_str());
    g_log = std::fopen((g_dir + "daytona.log").c_str(), "w");
    say("GAME main: %s, %s\n", g_dir.c_str(), clock.c_str());
    g_writer.start();
    try {
        run_game();
    } catch (const std::exception &e) {
        say("GAME FAILED: %s\n", e.what());
        g_writer.stop(); // (written before the error screen, which waits for a button)
        show_error(e.what());
    }
    g_writer.stop();
    if (g_log) std::fclose(g_log);
    sceKernelExitGame();
    return 0;
}
