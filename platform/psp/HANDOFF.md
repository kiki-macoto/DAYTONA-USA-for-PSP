# PSP port: handoff

## Rules for this port

- PSP frontend, build and tools are in `platform/psp/`.
- The shared runtime is used with the Dreamcast's defines (`M2_DC_MEMORY`,
  `M2_DC_SPIN_SKIP`, `M2_DC_SPEED`): their code paths are not SH-4 specific
  (the one that is, `M2_DC_NATIVE_GEO`, is guarded by `__sh__` and not set
  here). Changes the PSP needs in `src/runtime` go behind `M2_PSP_*`
  defines (`M2_PSP_BGR` in video.h, `M2_PSP_TEX_DIRTY` in m2_board and
  raster.h); the desktop builds compile exactly what they did before.
- The generated game code is used as the desktop recompile writes it, through
  `platform/dreamcast/scripts/fast_gen.py`'s rewrite (target-independent).
- ROM set: `daytona` (Revision A) only, as the Dreamcast port
  (`M2_DC_SPIN_SKIP`'s frame-wait address, 0x1394, is Revision A's).
- PSP-2000 and later only (large-memory mode).
- PPSSPP first, a console after. Speed and floating-point results from
  PPSSPP are stated as PPSSPP results.

## Geometry: culled polygons apart; 10 cars on every course (2026-10-25)

PROFILE geometry modes: Revision A's objects are all np_s (normals,
specular). HW geometry (geo.cpp's hooks around a parse with objects): it
is compute-bound (a busy frame: 5.5 M cycles, 1.2 M of them memory, 9 K
instruction misses, 800 K FPU instructions for ~3,500 polygons handed
over). model2_3d_culled_polygon (M2_PSP_FAST_GEO): a polygon geo_parse_np_s
already knows check_culling culls by its first two tests skips
model2_3d_process_polygon for exactly its side effects (rope, polygon z,
texture addresses, linking); np_s reads the normal and the point in one
GeoPtr::read. PPSSPP: frame dumps 3000/4500/6000 byte-identical, TRACE
exact. Console (ARK-4 now really ~450 MHz: "measured about 452 MHz"): a
light frame with objects 8.0 -> 6.4 ms; a busy one 12.3 ms (~16 expected
before); totals 22.1 (light, 3.9 idle) and 22.9-25.0 ms (busy).

The busy scenes' core (i960 + TGP, 16.6-17.2 ms) is the rest: the player
chose 10 cars on every course (kCars beginner 20 -> 10; m2run on patched
ROM images: POSITION /10, 10/10, the race as usual).

## The TGP's registers in a local: tried, worse (2026-10-25)

Tried: tgp.h's operations moved into an include shared by Tgp and a
TgpLocal (the scalar registers copied into a local, the board side through
references, I/O through Tgp's io_r / io_w), the generated run on `L.`
instead of `t.`, the copy stored back before every return. TRACE exact,
but tgpgen::run grew 132 -> 155 KB and its loads 5,100 -> 7,800: every one
of the 2,024 instructions is a dispatch target (the TGP resumes where it
stalled, and it jumps to computed addresses), so at every one the copy's
fields must be where the others expect them: GCC put them on the stack and
moved them at each. Taken out; tgp.h is as it was (no tgp_ops.inc).
Registers would need fewer entries (an interpreter or a precise fallback
for computed targets, as dual_modes does for the i960) first.

Build note: going between PROFILE_TGP=1 and plain builds after a runtime
header changed relinks objects built the other way (.SECONDARY; a flag
change is not a change): the link then misses rt::g_tgp_profile_*. Touch
the M2_PSP_PROFILE files (src/runtime/geo.cpp, m2_tgp_board.cpp) first.

## The console with the new i960 code; the TGP tried (2026-10-23)

Console, a busy race (553 TGP runs a frame): i960 instruction misses 75.8 K
-> 59.8 K a frame, memory stalls 6.4 M -> 5.3 M cycles, core 28.9 -> 25.0
ms, total 39.9 -> 36.5 ms. A light one (~200 runs): 30.0 -> 29.7. No
problems seen; 8 MB the largest free block after construction.

The TGP (HW TGP: ~25 K instruction misses a frame in a busy race): its
count added a basic block at once (a block's length at its first
instruction, an entry stub for a run resumed inside a block, a stall's
return less the block's instructions from it on; TRACE exact) made
tgpgen::run larger, 132 -> 141 KB: GCC already folded the per-instruction
`++count` of straight runs, and the stubs and their branches were extra.
Taken out. Its code is loads and stores of the Tgp's state (5,100 lw and
3,700 sw of 35,000 instructions): the next thing to look at there is
tgp.h's templates keeping that state in registers.

## The i960 code: jump tables and two versions of each chunk (2026-10-23)

chunk_000's MIPS, disassembled: m2recomp's dispatch (a switch by 128-byte
block, then by IP) had become compare trees (li / beq / sltiu per case, a
quarter of the function), and GCC had laid them out among the
instructions' code. Two passes in scripts/psp_gen.py, the same instructions
run and the events at the same counts (race_basic, race_expert and
attract_long: TRACE as before):

- **flat_dispatch**: one table of the labels' addresses per run of IPs
  (GCC's labels as values: a switch became a compare tree again in the fast
  version, where most cases go to the same place); chunk_017's IPs are in
  three runs.
- **dual_modes**: each chunk twice. chunk_NNN_p is the code as it was
  (`left` counted down before every instruction); chunk_NNN counts a basic
  block at once: at its first instruction, `if (left <= k) hand over` else
  `left -= k`. A block ends at a jump target and after an instruction that
  branches, returns, stores c.m_IP or calls the runtime other than through a
  psp_ load/store helper; inside a block a helper reads `n_c - (left +
  instructions after it)`, and a moved event hands over at the next
  instruction. Handing over (to_precise; the precise code's recheck hands
  back once more than 64 are left): g_left / g_precise / g_handoff
  (gen_psp.h), and gen_table.cpp's run (dual_table) calls the version asked
  for, again while one hands over. Entered at an IP that starts no block
  (an interrupt's return), the fast code hands over.

Text of the i960 code: 2.36 MB -> 1.60 MB for the fast versions (chunk_000
85 -> 48 KB), 2.0 MB more for the precise ones (cold: run only up to an
event); EBOOT.PBP 5.2 -> 7.4 MB (PPSSPP: largest free block 14 MB after
construction). PPSSPP's time is the same (97 s); the console is the test.

## The Media Engine re-weighed; --fast-inaccuracy tried (2026-10-23)

**The Media Engine is worth it after all** (on by default again). The core
depends mostly on the scene (TGP runs a frame): at 515-553 runs, without
it core 25.9 + audio 5.5 (mixing 4.5) = 41.4 ms total; with it core 28.9 +
audio 1.6 = 39.9. At 190-220 runs: 31.3 without, 30.0 with. The "5-11 ms
slower" read before compared reports of different scenes.

**Console geometry** (PSP-3000, the clock-read cost of 0.56 us a section
taken off): a busy race frame with objects, 3,031 polygons, ~10 ms of
model2_3d_process_polygon: vertices and culling ~5.7, clipping ~2.6,
texture reads ~2.2, writing the polygon ~1.1. About 600 cycles a polygon in
the simplest part: instruction misses rather than arithmetic.

**--fast-inaccuracy** (build_psp.py, kept as an option; objects in
build-daytona/psp_fi): the Vita's code (m2recomp --fast_inaccuracy with
direct work RAM, m2tgprecomp --fast_inaccuracy). Not smaller: i960 2.49 MB
of text against the exact rewrite's 2.36 MB (psp_gen.py's helpers already
made it compact), the TGP 331 KB against 142 KB; PPSSPP 138 s against 143 s
for race_basic with every frame drawn and dumped. Its race ran through:
TGP counts and buffer hashes at 1500, 4500 and 6000 the exact build's, the
i960 counts a few instructions off. Not sent to the console.

## Where the geometry goes (2026-10-23)

The player: the sound is right with the Media Engine now, the game still
slow. PROFILE geometry (PROFILE_TGP=1; geo.cpp's g_geo_* under
M2_PSP_PROFILE): parse on frames with objects / without, and
model2_3d_process_polygon timed on 1 call in 16. PPSSPP, race_basic,
every 4th frame drawn: 7-12 ms a frame with objects (2,200-3,800 polygons
handed to the rasterizer, 1,100-1,800 kept), 6-7 tenths of it projecting
and clipping; 0.2-0.3 ms a frame without (skip_objects works). At one
frame in four that is 2.5-3 ms a frame on average: a third off the
polygon path would save about 1 ms. The core (i960 + TGP, 11-14 ms on the
console, about half of it memory stalls from 30 K instruction misses a
frame) is the larger part. Console numbers for the split: asked for.

## The Media Engine's first run on the console (2026-10-23)

It started and mixed (audio on the game thread 1.0 / 1.4 ms, from 2.1-3.8),
but the sound broke up (underruns 196 / 426, from 65) and the core slowed
to 18.2 / 29.3 ms (from 11-13.5): memory stalls 6.5 M cycles a frame (from
2.3 M). The engine's wait read the uncached seq back to back (a few nops
between): the bus was kept busy. Now me::pause is ~8,000 cycles of a
register count between reads. The underruns: its batch counted as written
only at the next frame (one frame of the ring lost; frames of 30-40 ms):
now the output thread plays it once done == seq (posted_end_), and the
target is 5 blocks with the engine. Its CP0 Count read 0 (not counting on
the engine): the mixing time is no longer printed.

## The sound mixed on the Media Engine (2026-10-23)

Console (ARK-4, clock left alone): total 25.0 / 21.9 / 24.2 ms in a race;
mixing 1.4 / 2.5 / 3.1 ms a frame, all on the game thread. Now the Media
Engine mixes (game/media_engine.h, audio.h's me_worker): started as
mcidclan's psp-media-engine-custom-core starts it (MIT; THIRD_PARTY.md),
through kubridge's kuKernelCall (the handler to 0xbfc00000, reset
0xbc10004c 0x14 then 0), not its kernel module. The game thread still runs
the sequencer and the voices' bookkeeping; FastSoundEngine::offload makes
render() write a job per stretch between events (output pointer, frames,
copies of the playing voices) and move the voices on without samples
(advance: the envelope per block, the phase by n steps at once).
run_jobs mixes them on the engine, block for block as mix() does:
tools/test_fast_mixer.cpp's "as jobs" check, 0 samples differ on all 8
sequences. A batch a frame (seq / done in an uncached MeLink); the next
frame waits for it before its frames count (ring target 4 blocks, one
more); samples a batch names are not evicted in that batch; the decoded
samples and the jobs are written back for the engine; it invalidates its
8 KB cache before a batch and writes it back after; the output thread
reads the ring uncached. 44.1 kHz through sceAudioChReserve's plain
channel with the engine (sceAudioSRC may use the system's Media Engine
code, which the reset ends). Fallbacks: no answer in 200 ms at start
(PPSSPP: does not run engine code), or a batch not done in 200 ms: the game
thread mixes. settings.txt "me 0" turns it off; recorded runs (--inputs)
never use it (race_basic TRACE 6000 unchanged). Log: "GAME Media Engine
started" and "PROFILE Media Engine, ms per frame: mixing X (its CP0 Count
as ms at the CPU clock), the game waiting Y". Not yet run on a console.

## ARK-4's overclock and the clock left alone (2026-10-23)

The player went back from ARK-5 to ARK-4 (final), with its overclock on.
The draw change worked there (list 2.0 / 3.9 ms, from 3.3 / 9.0; draw
cycles 2.2 M / 3.9 M, from 2.6 M / 6.9 M; uncached stores 6.6 K / 13 K,
from 20 K / 54 K), but everything else ran 1.3-3x slower: core 20.7 / 25.7
(from 12.2 / 17.9), mixing 2.9 / 4.7 (from 1.0 / 2.4, code unchanged), the
CLOCK line's system calls 1.33x slower: about 250 MHz of real speed while
scePower read 333. The game set 333 over it (scePowerSetClockFrequency even
when the clock was already 333). Now set_cpu_clock leaves the clock alone
when settings.txt asks nothing and it already reads 333 or more, and the
log's first line ends with "measured about N MHz": 20 M taken branches
with an add in the delay slot, timed, 2 cycles an iteration (PPSSPP: 322).
Not yet run on the console.

## The display list sent whole, vertices cached (2026-10-23)

The console's draw (every drawn frame, before): 6.9 M cycles, 4.3 M of
them memory stalls, 54 K uncached stores; "list" 3.3-9.0 ms. Two causes in
PSPSDK's libpspgu (disassembled): in GU_DIRECT, sceGuDrawArray and
sceGuGetMemory each call sceGeListUpdateStallAddr (a system call: two a
polygon), and sceGuStart makes the list address uncached (| 0x40000000),
so every vertex store went to memory. Now: the frame's list is built with
sceGuStart(GU_SEND) and handed over by sceGuSendList(GU_TAIL) after
sceGuFinish (the GE starts it then, not as it is written; GE wait was ~0),
and the vertices go to `vertices_` (cached, 24,576 of them) written back
once before the send. kListBytes 1.5 MB -> 512 KB (commands only).
**GU_SEND's sceGuStart does not set the draw buffer** (GU_DIRECT's does,
from sceGuSwapBuffers' state): without sceGuDrawBufferList at the list's
start the GE drew into one buffer (a dumped frame came out black).
PPSSPP: race_basic TRACE 6000 unchanged; frame dumps 3000, 4500, 6000
byte-identical to before, 1500 differs in 11 pixels of the bottom row
(slight shades: the layer's bilinear edge, not looked into). Not yet
measured on a console.

## Overlay; the clock refused (2026-10-12, afternoon)

**ARK-5 on the PSP-3000 refused 440 MHz**: sctrlHENSetSpeed(440, 220) left
333 (the log now says "the custom firmware refused it"). Going past 333
would mean writing the PLL ourselves (kernel mode): not done.

**Overlay**: `overlay.bin` next to EBOOT.PBP (480 x 272 RGB565, the frame
buffer's order, rows of 480) is copied into both frame buffers once, after
the Renderer starts (Renderer::set_overlay, a GE block transfer: PPSSPP
shows it, a CPU write to VRAM it would not). Every frame clears and draws
inside the scissor only (64..415), so the sides keep it at no cost; checked
in PPSSPP over race_basic's 6000 frames (frame dumps 600 and 3000 show it;
TRACE 6000 unchanged). `scripts/make_overlay.py` (standard library only:
its own PNG decoder) makes it from a PNG whose see-through (or white) area
is the game's window, scaled to the window's height; `build_psp.py game
--overlay PNG`. The player's bezel was a WebP with alpha: Windows' WIC
decoder drops the alpha (Bgr32), WinRT's BitmapDecoder keeps it.

## The clock past 333 MHz (2026-10-13)

Console after the helpers (race, untimed): core 13.5 ms (from 15.2-16.4),
total 22.5 (from 23.7-26.1), 45 K instruction misses a frame (from 51-57 K).
The player: "much better". **Audio: the mixing is most of it** (1.6-2.0 of
2.1-2.6 ms a frame): next to look at.

**set_cpu_clock** (main.cpp): `settings.txt` next to EBOOT.PBP ("cpu N",
222-444; build_psp.py writes a template with it commented out, once);
without it the clock the custom firmware's overclock already set when past
333 (the game used to set 333 over it), else 333. Past 333:
`sctrlHENSetSpeed(N, N/2)` (pspsystemctrl_user; its header also has
sctrlHENGetSpeed, but the stub library does not: scePower reads the clock),
and 333 when the clock still reads below it (PPSSPP: the call does nothing
and 222 stayed). The log's first line gives the clock, the bus and why.

## Loads, stores, calls through helpers; n derived (2026-10-12, late night)

Console with fewer cars, race (untimed reports): core 15.2-16.4, total
23.7-26.1 ms a frame (from 22 and 33): still about 40% over the arcade's
17.4. HW: 51-57 K instruction misses a frame, memory stall half the cycles.

The generated i960 code averaged 69 bytes (17 MIPS instructions) an i960
instruction. A plain load or store was about 16 MIPS instructions, 11 of
them the same bookkeeping at all 13,000 places. scripts/psp_gen.py now:
- **memory_helpers**: a plain load or store (11,879 places: one
  i960_read_dword/word, bus read_byte, or the matching write) and a call to a
  constant address or a ret (3,276) call game/gen_psp.cpp's psp_ld32/16/8,
  psp_st32/16/8, psp_call, psp_ret, which set m_IP and ls.pending, read the
  epoch around the same Cpu/Bus call and return "the event moved" (in bit
  32 of a load's result). The call site keeps the countdown (its exit out of
  line, as the others) and sets left = 1 on the flag. A store: 16 -> 13
  MIPS instructions on the straight path.
- **derive_n**: fast_gen's `++n` at every instruction's end is gone; n is
  `n_c - left` within an instruction (after its countdown) and
  `n_c + 1 - left` between instructions; n_c is set where n was reset
  (dispatch 0; recheck ~0 for its jump to dispatch with left 0, then left - 1
  for the left it resumes with), and `left = 1` within an instruction (an
  event moved; the divides) sets n_c = n_c - left + 1 first. The pass checks
  that no use of n is left.

The generated i960 code 2.64 -> 2.36 MB. PPSSPP: race_basic, race_expert
and attract_long end on the same TRACE lines as before (all three counts
and buffer hashes). PPSSPP's own core time went up (6.5 -> 7.3 ms in the
attract mode: calls cost its JIT more); the console is what counts.

PROFILE audio now splits the mixing (FastSoundEngine::mix) from the rest of
the audio time (the sequencer): PPSSPP 0.5 of 0.7 ms.

## Fewer cars in a race (2026-10-12, night)

Console, race, ms a frame (untimed reports): core 21.8-22.6, the TGP's part
about a third of the game's cycles (HW TGP 5.4-5.8 M cycles, 36-40 K
instruction misses a frame). The TGP at -O2 gained little (core 22.8 ->
22.4). The player asked for fewer cars.

**The count**: the arcade has 40 (beginner), 20 (advanced) and **30**
(expert; not 20) cars. The recorded scripts course_advanced and
course_expert do **not** select those courses (both race the beginner
oval: the select follows the wheel's position, and they let it back to the
centre before the pedal); the new `scripts/inputs/race_advanced.txt` /
`race_expert.txt` (the wheel held at 0xc0 / 0xff through the pedal at 1660)
do. The shipped ROM page list now has their pages too: 3,276 -> 3,763
(14.7 MB; the preload's limit in a 16 MB cache is 3,840 pages). In PPSSPP
both end on m2run's TGP counts (148,692,363 and 171,312,860). m2run's new
`--ram-dump DIR` (with `--every N`: RAM and work RAM, 1.1 MB a dump) and a
search over the three courses gave work RAM 0x501080 (the count) and
0x501084 (count - 1); the code that stores them (i960 0x4600-0x4620) loads
it from main data 0x02805278 + 4 * course (beginner 40, expert 30, advanced
20, then 8; the laps table beside it reads 8, 2, 4). 0x12e0 stores 44 at
start-up and 0x3b60 a linked game's count.

m2run from a copy of the images with 20 / 10 / 10 there: POSITION /20,
10/10, 10/10 on the three courses; TGP instructions in 6,000 frames 195 ->
152 M (beginner), 149 -> 124 M (advanced), 171 -> 125 M (expert).
race_to_end (20,000 frames) from it: RESULTS 20th, GAME OVER, and the next
game's GENTLEMEN START YOUR ENGINES.

**The PSP**: `kCars` (main.cpp), StickRom `patches` (a word changed as its
page is read, only where it holds the arcade's value), played games only (a
recorded race keeps the arcade's 40 / 20 / 30 and still matches the
desktop). The log: "GAME cars in a race" and the values read back.

## Exits out of line on the console; the TGP at -O2 (2026-10-12, evening)

Console, the first report (frame 1800, the start of play), before and
after psp_gen.py's exits (the scenes are not the same run):

| | core ms | cycles | memory stall | i_miss | d_miss |
| --- | --- | --- | --- | --- | --- |
| before | 12.8 | 7,836 K | 3,362 K | 39.3 K | 10.2 K |
| after | 10.4 | 6,557 K | 2,589 K | 29.9 K | 7.5 K |

Instruction misses down about a quarter. The player: steering fine now.

Next in the same direction:
- **TGP code at -O2** (Makefile): 166 -> 142 KB, 369 -> 246 calls (-Os: 91
  KB but 1,627 calls, its template helpers out of line). race_basic in
  PPSSPP on the desktop's counts and hash.
- **HW counters for the TGP apart**: in the reports with the TGP's timing on,
  m2_tgp_board's `g_tgp_profile_enter/leave` hooks move the counters into an
  "HW TGP" line and the game's line leaves the TGP out ("HW game without
  the TGP"); in the others "HW game with the TGP". The hooks cost two
  kubridge calls a TGP run (in the timed reports only).

- **The Makefile's `.SECONDARY:`** (no prerequisites: every target an
  intermediate) does not rebuild a deleted object, library or game.elf while
  EBOOT.PBP is newer than the sources ("Nothing to be done"), and a changed
  recipe is not a change: touch the source instead (a flag change for
  tgp_gen: `touch build-daytona/psp/gen_psp/daytona_tgp/tgp_gen.cpp`).
- WSL's /tmp is emptied between `wsl.exe` calls here: build logs go to ~.

## The console's core is instruction-cache bound; exits out of line; stick (2026-10-12, later)

**HW counters, PSP-3000, 333 MHz, race** (thousands; the counters seem to
count the game thread's time only: 142,000 cycles in a 10 ms sleep; their
cycle rate against the frame times is about 450 MHz, unexplained):

| | cycles | memory stall | internal stall | i_miss | d_miss | cached load/store |
| --- | --- | --- | --- | --- | --- | --- |
| game (run_frame), a frame | 12,281 | 6,529 (53%) | 1,060 | **84.5** | 11.1 | 849 / 526 |
| draw, a drawn frame | 6,702 | 4,048 (60%) | 423 | 2.4 | **32.9** | 333 / 156 |

The game's frame waits on memory half the time, and on instruction fetches
eight times as often as on data. PPSSPP, which models no cache, ran the core
2.2 times faster. The drawing is data-cache bound (the GeoPolys read in
sorted order; the list's stores are uncached already: 55,000 a drawn frame).

**scripts/psp_gen.py** (build_psp.py runs it after fast_gen.py; objects
from `gen_psp/`): each register-only i960 instruction's inline exit
(`if (!--left) { c.m_IP = ...; goto recheck; }`, 4-5 MIPS instructions next
to the branch) becomes `{ x_off = N; goto x_exit_W; }`, a shared exit per
128 KB of IPs at the function's end; GCC then moves each `li` + branch
there. Tried first and no good: __builtin_expect alone,
-freorder-blocks-algorithm=stc, -fno-reorder-blocks, a block per exit at the
end (GCC put them all back after their branches). chunk_008 121.4 -> 110.1
KB. The TGP code's budget and stall exits get __builtin_expect. race_basic
in PPSSPP ends on the desktop's counts and hash. Console figures awaited.

**Stick: left and right only** (reported: the wheel now and then stayed
turned). A stick pushed more up or down than sideways steers straight
(`controls.h`, `Pad::ly`); test_controls passes.

## First console figures; the TGP clock's own cost (2026-10-12)

**PSP-3000, 333 MHz, played race (buttons), PROFILE_TGP=1**, ms a frame:

| core (TGP) | geometry | video | draw (each drawn) | audio | total |
| --- | --- | --- | --- | --- | --- |
| 16.4-18.9 (5.4-6.6) | 3.0-3.8 | 0.4-1.2 | 2.4-3.1 (9.5-12.4) | 2.3-2.9 | 24.6-29.9 |

The draw rate sat at every 4th (its limit): the core alone is near the
arcade's 17.39 ms. Against PPSSPP (core 7.5, TGP 3, geometry 4.8) the core
is 2.2 times slower and the geometrizer faster. Drawing: sort+textures
4.4-5.6, list 5.0-6.7 ms a drawn frame (PPSSPP 1.8, 2.2). 24-48 audio
underruns in five minutes.

**The core figure includes the profiling itself**: the TGP's timing read
`sceRtcGetCurrentTick` (a system call) twice a TGP run. Now the TGP clock is
`sceKernelGetSystemTimeLow`, `CLOCK` at the start logs each clock's cost a
call, and the TGP's timing is on in every other report only (`PROFILE TGP
timing on/off ... N TGP runs a frame`): the "off" reports give the core
without the clock reads. PPSSPP: 0.93 and 0.51 us a call, 290-340 TGP runs a
frame in the attract mode. **Console: 0.74 and 0.42 us a call**, 538 TGP
runs a frame: the timing costs under 0.5 ms a frame. The core's 15-17 ms is
the game's own. The player: the time attack (one car) holds 30 pictures a
second throughout; the race with the other cars is the slow one.

## Stick writes off the game thread; ROM pages preloaded (2026-10-12)

Reported on the console: a short freeze whenever the Memory Stick is read.
Three stick accesses stopped the game thread:
- **The log**: `say` flushed every line (the PROFILE lines every 1,800
  frames, five of them).
- **The backup RAM save** every 3,600 frames: it changes all the time (the
  operating time at 0x108), so it was written every minute in play.
- **ROM cache misses**: a page (or a run of 8) read synchronously.

Now `StickWriter` (main.cpp) writes the log and the saves from its own thread
(priority 0x18, above the game's 0x20: it sleeps in the stick's driver; a
lower one would not run while the game never waits). The game thread
queues; saves are written before the log lines queued with them (GAME DONE
comes after the last save).

**ROM pages**: StickRom records each page the game uses; the list (a
32-bit word a page, region << 24 | page) is saved as `save/rom_pages.bin`
with the settings and at exit. At the start the pages listed in
`rom/rom_pages.bin` (shipped: `build-daytona/psp/rom_pages.bin`, which
build_psp.py copies when present) and `save/rom_pages.bin` are read in runs of up to 64 pages into
the cache, leaving an eighth (at most 1 MB) free. race_basic in PPSSPP: the
first run listed 1,295 pages (5.1 MB); the second preloaded them and had 0
misses; TRACE at 6,000 the desktop's both times.

The shipped list: race_basic, attract_long, course_advanced, course_expert,
time_attack and race_to_end run in turn in PPSSPP (a folder
`PSP/GAME/DAYTONA_TEST`, each run adding to its save/ list): 3,276 pages
(12.8 MB), kept as `build-daytona/psp/rom_pages.bin`. The cache (16 MB)
preloads all of it (limit 3,840 pages). The play build in PPSSPP: 0 misses in
the first 3,600 frames. To make it again: delete DAYTONA_TEST/save, then for
each script `build_psp.py game --inputs ...`, copy EBOOT.PBP there and run it
to GAME DONE.

## The CPU's hardware counters (PROFILE_TGP=1 builds)

`HwCounters` (main.cpp) reads pspdebug.h's profiler registers (0xBC400000:
cpuck, internal and memory stall cycles, i_miss, d_miss, d_writeback,
cached/uncached loads and stores, ...) through kubridge's kuKernelMemcpy
(kernel memory; enabled with kuKernelPokew), around `run_frame` ("HW game",
thousands a frame) and `renderer.draw` ("HW draw", thousands a drawn frame),
one line each per report. PPSSPP: "HW profiler not available". Meant to
settle where the console's core goes (2.2 times PPSSPP's while the
geometrizer is faster: memory, by the look of it; instruction or data
cache?). The register meanings are pspdebug.h's names; what each counts
exactly is to be read from the figures.

## Credits, Start, the D-pad views; a race frame from 22.3 to 17.7 ms (2026-10-11, night)

**Credits at power-on: none.** The bookkeeping block, found with m2run
(coins, a start, saves compared): backup RAM 0x110/0x118 coins, 0x128
credits, 0x138/0x13c plays, 0x108 operating time, 0x104 a checksum whose
formula was not found (every candidate tried matched only through the
copy at 0x180); 0x180-0x1ff a copy. With credits left the frontend zeroes
0x128 and 0x1a8; the game finds the checksum wrong and zeroes the whole
block (coins, plays, time), records and settings untouched. Checked on the
desktop: m2run from the player's backup RAM so changed shows the attract
mode with CREDIT 0/1 and saves a zeroed block; the PSP likewise (log:
"7 credits left: cleared"). Revision A starts a game by itself on a credit
(m2run: one coin, no start, Circuit Select): so **Start** puts a coin in
when there is no credit (game/controls.h; not with Cross held, the time
attack's Start + accelerator). **Views on the D-pad** (up, right, down,
left: VR1-VR4); steering on the analog stick only, shifting on L/R only.
`tools/test_controls.cpp` passes.

**Speed** (race_basic, every 2nd frame drawn, PPSSPP, ms a race frame):

| Change | core | geometry | tile layers | drawing | audio | total |
| --- | --- | --- | --- | --- | --- | --- |
| Before | 7.5 | 7.0 | 2.4 | 3.0 | 2.4 | 22.3 |
| The scrolled scenery layer on the GE (`Video::back_scrolled`, as the Dreamcast) | 7.5 | 7.0 | 0.6 | 3.1 | 2.2 | 20.3 |
| `M2_PSP_FAST_GEO`: the geometrizer's per-polygon doubles in float | 7.5 | 4.8 | 0.6 | 3.1 | 2.1 | 18.1 |
| Clip coordinates without a divide per vertex; `sceGuDrawArray` | 7.5 | 4.9 | 0.6 | 2.6 | 2.1 | 17.7 |

race_basic's TRACE line at 6,000 the same after each; the picture
compared with m2run's at frames 2,700-5,700 (a first version of the
divide-free vertices had y's sign wrong: the 3D upside down, caught by the
comparison). The adaptive draw rate now steps up only past 108% of the
arcade's frame (the game up to 8% slow rather than every 3rd), down from
every 3rd under 85%: in a race it stays at every 2nd mostly.

- **Measured, not kept**: the generated i960 code at -O2 (3.23 -> 3.72 MB):
  core unchanged to the tenth of a millisecond. The core is the board's
  memory access and the TGP (about 3 ms of 7.5, `PROFILE_TGP=1`), not the
  i960's own instructions.
- `geo.o` called `__divdf3`, `__muldf3`, `__extendsfdf2`, `__fixdfsi` and
  `sqrt` per polygon or point (`psp-nm -u`); with `M2_PSP_FAST_GEO` none.
  fabs and sqrt in float give the same bits; the z-clip reciprocal and the
  LOD distance can differ in their last bit (picture only; the PSP's
  renderer does not use the LOD).
- `platform/dreamcast/tools`' dcfastcheck with -pg did not link: with
  M2_DC_SPEED the TGP's status helpers are always_inline and the host
  build's tgp_gen objects (made without it) refer to them out of line.

## Adaptive draw rate, the GE in parallel; "the attract mode jumps to select" (2026-10-11, later)

**Reported "the attract mode starts and switches to the select screen at
once": the game's own behaviour with credits left.** The player's
`save/backup_ram.bin` differs from the factory one in 18 bytes, the same
nine twice (0x100 and 0x180 on): bookkeeping, by the look of it 11 coins,
7 plays, 4 credits. m2run on the desktop from that backup RAM, no input:
Circuit Select at frame 300, Mission Select, GENTLEMEN START YOUR ENGINES
by 1,800, as on the PSP. The PSP keeps the backup RAM as the cabinet does;
each lone Select press is a coin.

**Speed: the draw rate adapts** (buttons only; a recorded script keeps
`--draw-every`). Every 60 frames, on a frame that is a multiple of 12 (so
every rate draws it: the runtime's frame skip decides object parsing a
frame ahead), the frame's own time (not the pacing wait) is compared with
the arcade's 17.39 ms: up a step past 102%, down under 80% (from every
3rd) or 72% (from every 4th); between `--draw-every` (2) and every 4th.
PPSSPP, from that backup RAM with no input: 17.7 ms a frame with 9.6 idle
in the selects at every 2nd; the race went to every 3rd (18.4 ms) and 4th
(17.4 ms, 2.5 idle); back to every 2nd after it. The game runs at the
arcade's speed; the race shows 14-19 pictures a second.

**The GE draws while the CPU runs on**: `draw` hands it the list,
`finish()` (next draw, or before a frame that composes the layers) waits
and swaps. In PPSSPP no gain (its GE work is not charged to the PSP's
clock: "GE wait" 0.0 ms); on a console the GE's time per picture should
come off the frame. race_basic's TRACE line at 6,000 unchanged.

Where a race frame goes now (every 2nd drawn, PPSSPP): core 7.5, geometry
7.0, tile layers 2.4, drawing 3.0 (building the display list 4.3 and
textures/palettes 1.7 per picture), audio 2.4 ms.

## Textures loaded during play; frame dumps (2026-10-11, late)

**The texture cache now follows texture RAM writes.** Reported: the GAME
OVER letters mixed with the background, and the player's car on the
transmission select (Mission Select) in the wrong colours. Both are
textures the game writes into texture RAM as it goes; the cache (as the
Dreamcast's) kept the texels it built from whatever was there before.
`M2_PSP_TEX_DIRTY` (only the PSP's Makefile sets it): `M2Board::tex_write`
records `tex_generation` per 1 KB of texture RAM (`VideoMem::tex_chunks`);
each cached texture keeps the chunks it was read from and is built again
when one was written after it (checked once per texture RAM generation,
within the 32 builds a frame). race_to_end: 99 textures rebuilt by frame
9,600, 205 by 19,800. race_basic's TRACE line at frame 6,000 is unchanged.

**Frame dumps** to compare with the desktop: `build_psp.py game --inputs
SCRIPT --dump-every N` writes a drawn frame every N to `shots/` next to
EBOOT.PBP (`scripts/shot2png.py` makes PNGs), against `m2run --dump DIR
--every N` (with `--frame-skip 1` for `--draw-every 2`, the PSP's file then
one frame later). The frame buffer goes through a GE block transfer to main
RAM: PPSSPP draws on the PC's GPU and leaves VRAM empty (the first dumps,
read straight from VRAM, were black). A recorded script now runs its own
`frames` line (race_to_end: 20,000), not 6,000.

Compared, desktop and PSP side by side, every 100 frames:
- race_to_end, every frame drawn and every 2nd drawn: the GAME OVER
  sequence (frames 7,700-8,200) and Mission Select (9,000-9,700) the same.
- attract_long, every 2nd drawn: the whole loop (rankings, V.R. screens,
  title, the three courses' demos) the same; at frame 1,200 the demo's
  camera cuts between the two frames compared.
The reported "attract mode does not play on" was not reproduced (both runs
start from `rom/`'s single-cabinet settings; a played PSP starts from its
own `save/`).

- **WSL's interop can stop**: after the WSL restarts, `build_psp.py
  --ppsspp` failed with "Exec format error" starting PPSSPPWindows64.exe
  from WSL; the builds and `--deploy` still work, PPSSPP was started from
  Windows.

## Sound mixer and texture colour (2026-10-11, night)

**Sound: `game/fast_mixer.h`**, the native sequencer with a mixer of the
PSP's own (integer arithmetic, samples decoded to 16 bits once into a 3 MB
cache, phases in two 32-bit words with a carry, the envelope and volumes
every 16 frames, 32 kHz). `tools/test_fast_mixer.cpp` plays four music and
four effect sequences through it and through `snd::NativeSoundEngine` at
48 kHz on the PC: 40-68 dB signal to error on all of them. (A first
version with 17.15 phases measured 3-34 dB: the pitch was up to 0.008% off,
inaudible, but whole sequences drifted out of phase in 20 s.) Race in
PPSSPP, ms a frame: audio 10.5 -> 2.4, total 30.0 -> 22.2 (about 45 game
frames/s in a race; the attract mode now waits for the arcade's 57.52).
race_basic still ends at the same TRACE line (`26338af44761a3d9`).

**Texture colour as the CPU rasterizer's.** A palette per colour, light,
luma base and transparency (32-bit, 16 entries: `draw_tex_span`'s luma
RAM -> light -> translation table -> gamma for each texel), white vertices.
The grey-luma-times-vertex-colour approximation (the Dreamcast's) turned
textures whose hue changes with the luma grey: the windows. At most 643
palettes a frame seen (limit 4,096).

**Checkered polygons** (texheader bit 15; the CPU draws every other pixel:
the fences, the condition panel, the ROLLING START banner) were drawn
solid; now at half alpha. 62-86 of them a frame in a race. Screenshots:
the trees show through the fence.

## Current state (2026-10-11, evening)

**A whole recorded race runs in PPSSPP and ends where the desktop's does**:
`build_psp.py game --inputs scripts/inputs/race_basic.txt --deploy ...
--ppsspp ...` (coin-up, course and transmission select, rolling start, the
Beginner race, 6,000 frames) ends at 909,312,001 i960 and 195,261,176 TGP
instructions, the desktop's figures for race_basic
(`platform/dreamcast/HANDOFF.md`). Two runs gave the same TRACE line at
every one of the 100 checkpoints (frame 6000: buffer hash
`26338af44761a3d9`). The checkpoints have not been compared one by one with
a desktop `tracecheck` run yet: only the end counts.

Picture (PPSSPP window captures): the attract mode's ranking screen, the
race's road, trees, sky, mountains, cars and the whole HUD (lap, speed,
tachometer, course map) in the right colours; the busiest frame 1,839
polygons, 1,641 textured, none dropped or drawn flat; 293 textures built.

**Speed, PPSSPP only** (its clock is not a console's: these are relative
figures). `PROFILE` lines, race, every 2nd frame drawn, ms a frame:

| core | geometry | tile layers | drawing (per drawn frame) | audio | total |
| --- | --- | --- | --- | --- | --- |
| 7.5 | 5-7 | 2.4-3.9 | 1.7-2.6 (3.4-5.2) | 10-12 | 27-31 |

So about 33 game frames/s in a race in PPSSPP (the arcade runs 57.52),
40-90 in the attract mode. **The native audio's software mixer is the
largest single cost** (a third of the race's frame); then the core and
the geometrizer.

**Memory, PPSSPP**: the heap is 49 MB (54 MB for the self-test, which has
no code to speak of). After texture RAM, the renderer and the sound
(13.9 MB used) and a 16 MB ROM cache, the constructed game uses 38.4 MB,
leaving a 10 MB block; steady at 38.8 MB through a race. ROM: 454 misses,
1,425 pages read from the stick in the whole race (the cache never
evicts in it).

Settings: **Revision A starts as a linked twin cabinet** and waits at
NETWORK CHECKING for the other cabinet. `build_psp.py game` now makes
single-cabinet settings with m2run and `scripts/single_cabinet.txt` (test
mode > GAME SYSTEM: LINK ID MASTER -> SLAVE -> SINGLE, CABINET TWIN ->
UPLIGHT; service moves the cursor, test changes the value; the view buttons
do nothing there) and puts them in `rom/`. The race above starts from them.

Done and checked before that:

- **Self-test passes in PPSSPP** (1.19.3, Windows; `build_psp.py selftest
  --deploy ... --ppsspp ...`): `double` divide, multiply, add and square root
  and `float` divide, multiply and square root give the PC's bits; denormal
  results are kept (IEEE, not flushed, in PPSSPP: not yet checked on a
  console); newlib's `printf` of a double matches; C++ exceptions work;
  the largest block malloc gives is 54 MB in PPSSPP.
- **The runtime compiles for the PSP** (psp-gcc 15.2, the Makefile's
  `runtime-objects`): no errors, the same 9 warnings in `geo.cpp` as on the
  Dreamcast.
- **The frontend compiles** (`game/main.cpp` with the renderer, audio and
  controls headers): no warnings at `-Wall -Wextra`.
- **Controls** (`game/controls.h`): `tools/test_controls.cpp` passes on the
  PC (steering, pedals, gears on presses, coin on a lone Select, test and
  service, view buttons).
- **Host side in WSL**: the importer and the three recompilers build
  without SDL (`build_psp.py host` configures that way).

Not done yet: the checkpoint-by-checkpoint comparison with the desktop
(`platform/dreamcast/tools` tracecheck), listening to the sound, playing
with the buttons (only recorded races and the attract mode have run), a
console.

## Found on the way (and what was wrong)

- **GCC removes a malloc whose result is only tested and freed**, taking
  it as successful. The free-memory probe (`largest_free`) reported 64 MB,
  then 128 MB (its first try each time), the ROM cache was sized from that
  and the game ran out of memory (`std::bad_alloc`, then "no RAM for the
  ROM cache"). The pointer now goes through a volatile. The self-test's
  "64 MB" was the same mistake.
- **A PSP-3000 with ARK-5 (Use Extra Memory: Force) had 17 MB** in the
  largest free block at start ("Not enough RAM"), the PSP-1000's 24 MB less
  the program. Not explained yet: ARK-5's source (SystemControl
  settings.c, syspatch.c; Compat/PSP high_mem.c) unlocks the extra memory
  for MEMSIZE=1 or Force at sceImpose_Driver's load, before the program,
  and the extra memory cannot be unlocked later (sctrlHENApplyMemory
  returns -3 once the system has booted). The start now counts every free
  block (the heap is the largest; ROM cache or texture RAM go in another
  block through sceKernelAllocPartitionMemory if it is larger), puts
  texture RAM in the 4 MB volatile memory (sceKernelVolatileMemTryLock),
  and logs and shows each figure. A recorded race in PPSSPP still ends on
  the desktop's counts. The console still had no other block (CFW
  reported apitype 0x141, an ordinary XMB start), so below kNeeded the
  game now runs in a **low-memory mode**: PCM ROMs read through
  FastSoundEngine's PcmReader (StickPcm; tools/test_fast_mixer.cpp checks
  it gives the in-RAM engine's samples exactly), a 1.5 MB decoded-sample
  cache, kReserveLow 12 MB and the ROM cache what is left (2 MB). Tested in
  PPSSPP with a PARAM.SFO of MEMSIZE=0 (24 MB, 17 MB free as on the
  console): race_basic ends on the desktop's counts with 1.9 MB left; 139
  sample reads in the race (36 ms in PPSSPP). Not yet run on the console.
- **The PSP-3000 froze just before a race started** (both memory
  modes), where PPSSPP plays on, and the watchdog (main.cpp: freeze.log
  after 3 s without a frame) wrote nothing: the whole console had stopped.
  Likely an FPU exception: a console traps a float divide by zero or
  invalid operation by default (PPSSPP does not model it), with no handler.
  pspSdkDisableFPUExceptions() is now called by the game thread, the audio
  thread and the self-test. Not yet confirmed on the console.
- **The ROM cache at 26 MB left 0 MB** after the game was constructed;
  capped at 16 MB (every run's reads together are 14.5 MB), and kReserve
  (8 MB) is held while the cache is sized rather than estimated.
- **A second recorded race diverged**: the first had saved the backup RAM
  (the race's records) to `save/`, the second started from it and went to
  a time attack. A recorded race now starts from `rom/` and saves nothing.
- **PPSSPP's fopen "w" did not truncate**: a run's log sat on top of the
  last run's longer one. The log is removed before it is opened (and a
  save's `.new` file before it is written).
- **WSL (6 GB) stopped altogether** while the host built the generated code
  with four compiles at once; `build_psp.py` builds the host side with at
  most two. Long WSL jobs run in a `wsl.exe` that stays open: WSL stopped
  its VM between calls and a `nohup` build was killed with it (SIGHUP).

## Design

- **ROM** (`game/main.cpp`, `StickRom`): the Dreamcast's `DiscRom` with
  `sceIo` reads from the memory stick. The cache takes what is left of the
  RAM after the program, texture RAM and the sound, less 8 MB for the
  runtime's growth, at most 16 MB (the union of every run's ROM reads is
  14.5 MB, `platform/dreamcast/HANDOFF.md`).
- **Texture RAM** (4 MB) in main RAM; as on the Dreamcast, the upper 1 MB
  of each sheet (which the game never writes) is the renderer's texture
  cache.
- **Picture** (`game/renderer.h`): the Dreamcast's renderer on the GE
  (colour since changed: see "Sound mixer and texture colour" above).
  The 16-bit tile layers are textured straight from the runtime's buffers
  in main RAM (written back from the data cache when their generation
  moves), in 64-texel strips. Polygons go through the GE's transform with
  a projection that puts the depth in w (perspective-correct textures;
  the 2D mode is affine). Textures are 4-bit, swizzled, with a 16-entry
  palette each (the Dreamcast's grey luma palettes, but no 64-bank limit);
  a mirrored axis is built at twice the size (the GE has no mirrored
  repeat). Alpha test and blending for colour-keyed texels. The frame is
  drawn synchronously (`sceGuSync`): the next `run_frame` rewrites what the
  GE reads. The race's scrolled scenery layer (`Video::back_scrolled`) is
  off (`set_scroll_layer(false)`): the runtime composes it into the back
  layer.
- **Sound** (`game/audio.h`): the native sequencer and `game/fast_mixer.h`
  (32 kHz) run by the game thread each frame into a ring that keeps
  about 96 ms queued; a higher-priority thread outputs it through
  `sceAudioSRCOutputBlocking`. The PCM ROMs (8 MB) are in RAM. No atomics:
  the Allegrex has no LL/SC; one core, compiler barriers.
- **Pacing**: never faster than 57.52 frames/s; behind, it does not catch
  up. Every 2nd frame drawn by default (`--draw-every`).
- **Saves**: `save/` next to `EBOOT.PBP`, written (through a `.new` file and
  a rename) when the EEPROM or backup RAM differ from the last save, checked
  every 3,600 frames and at exit (HOME).

## Plan

1. Done: built, linked at the first try, a whole recorded race in PPSSPP
   ending on the desktop's counts.
2. **Lockstep checkpoint by checkpoint**: the Dreamcast's
   `tools/tracecheck` with race_basic and these settings (`build-daytona/
   psp/nvram`), against the PSP's 100 TRACE lines.
3. **Speed**, in this order (the audio is done: 2.4 ms): the core and
   geometrizer (the Dreamcast's list: the
   scrolled layer on the GE, the transforms on the VFPU as the Dreamcast's
   `ftrv`, picture only), the layers' upload asynchronous so the GE draws
   while the next frame runs.
4. **Console**: a PSP-2000/3000 with custom firmware; memory and speed
   figures there.

## What not to re-propose

- `std::atomic` between the game and audio threads: the Allegrex has no
  LL/SC, GCC calls libatomic for it.
- The PSP-1000: 24 MB for the program does not hold the code, texture RAM,
  the PCM ROMs and a useful ROM cache.
