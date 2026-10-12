# PSP port (in progress)

The recompiled game on a PSP-2000 or later (Allegrex at 333 MHz, 64 MB of
RAM, about 52 MB of it for the program in the large-memory mode; 2 MB of
video RAM), built with [pspdev](https://github.com/pspdev/pspdev). Like
`platform/dreamcast`, which it follows, this folder is a separate build: it
compiles the main tree's runtime and the host-generated game code, with the
Dreamcast's runtime paths (`M2_DC_MEMORY`, `M2_DC_SPIN_SKIP`,
`M2_DC_SPEED`) and its own (`M2_PSP_BGR`, the GE's 16-bit channel order;
`M2_PSP_TEX_DIRTY`, texture RAM writes recorded for the renderer's texture
cache; `M2_PSP_FAST_GEO`, the geometrizer's doubles in float;
`M2_PSP_PROFILE`, optional, the TGP's time). The Windows, macOS and Linux builds are unchanged. Built
from the `daytona` ROM set only (Revision A, 1994), as the Dreamcast port.

The PSP-1000 (32 MB) is not a target. When the custom firmware does not
give the program the extra memory (seen with ARK-5 on a PSP-3000: 17 MB
free at start), the game starts in a low-memory mode: the sound's PCM ROMs
are read from the Memory Stick a sample at a time as it is first played
(the same samples), the decoded-sample cache is 1.5 MB and the ROM cache
2 MB (more reads from the stick). Texture RAM is in the PSP's 4 MB
volatile memory in both modes.

Status: **the game runs in PPSSPP** with its picture, and the sound output
running (12 underruns, all at start; not yet judged by ear): a whole
recorded race ends on the desktop's instruction counts, and the attract
mode and the race are drawn right. About 45 game frames/s in a race in
PPSSPP with every 2nd frame drawn, the arcade's 57.52 in the attract mode
(PPSSPP's speed is not a console's). `tools/test_controls.cpp` and
`tools/test_fast_mixer.cpp` (the PSP's sound mixer against the desktop's,
with your ROM images) run on the PC; each says how to build it. On a
PSP-3000 (ARK-5, 333 MHz) a race runs at about 23 ms a frame (the arcade's
17.4), with 20 / 10 / 10 cars. Step-by-step build and install:
[HOW_TO_COMPILE_PSP.md](../../HOW_TO_COMPILE_PSP.md). Details and the plan:
[HANDOFF.md](HANDOFF.md).

## Requirements

- Linux, or WSL on Windows (Ubuntu 24.04 tested), with a C++20 compiler,
  CMake and Python 3.
- pspdev at `~/pspdev` (or `PSPDEV`): GCC 15.2 tested.
- Your own Revision A ROM set at `roms/daytona.zip` (or `.7z`).
- PPSSPP to run it on the PC (1.19.3 tested on Windows), or a PSP-2000/3000/Go
  with custom firmware.

## Commands

All through `platform/psp/build_psp.py`; output goes under the host build
directory (`build-daytona/psp`, git-ignored).

| Command | What it does |
| --- | --- |
| `host` | The host build without the windowed app (no SDL): fetches SoftFloat, ymfm and the LZMA SDK, configures `build-daytona` and runs `scripts/recompile.py --set daytona` on your ROM set. |
| `selftest` | The floating-point self-test as an `EBOOT.PBP`; with `--deploy DIR --ppsspp EXE`, run in PPSSPP: exit 0 only if every result matches the PC's bits. |
| `compile` | Compiles the runtime and the generated game code for the PSP and reports their size. |
| `game` | The game as a folder for the memory stick, `build-daytona/psp/DAYTONA` (`EBOOT.PBP` and `rom/`, your ROM set's images: never share it). `--draw-every N` frames per picture (default 2); `--inputs FILE` a recorded input script instead of the buttons (then `GAME DONE`; it starts from `rom/`'s settings and saves nothing); `--nvram DIR` settings to start from (default: a single cabinet, made once by the host's `m2run` with `scripts/single_cabinet.txt`); `--deploy DIR` copies it to `DIR/DAYTONA`; `--ppsspp EXE` then runs it and shows `daytona.log`; `--dump-every N` (with `--inputs`) writes a drawn frame every N to `shots/` for comparing with `m2run --dump` (`scripts/shot2png.py` converts them). |

On Windows with WSL, from the repository in the WSL file system:

    python3 platform/psp/build_psp.py host
    python3 platform/psp/build_psp.py game --deploy /mnt/c/Users/$USER/Documents/PPSSPP/PSP/GAME \
        --ppsspp "/mnt/c/Program Files/PPSSPP/PPSSPPWindows64.exe"

(`$USER` there is your Windows user name if it differs from the WSL one.)
For a PSP, copy `build-daytona/psp/DAYTONA` to the memory stick's
`PSP/GAME/`.

## Controls

| PSP | Cabinet |
| --- | --- |
| Analog stick | Steering |
| Cross / Square | Accelerator / brake |
| R / L | Shift up / down (gears 1-4) |
| D-pad up, right, down, left | View buttons VR1, VR2, VR3, VR4 |
| Start | Start; with no credit, a coin too (the game starts on a credit: one press plays). Not with Cross held (Start + accelerator is the time attack) |
| Select (pressed and let go alone) | Coin |
| Select + L, Select + R | Test, service |

Revision A's factory settings are a linked twin cabinet, which waits for a
second cabinet at start (NETWORK CHECKING); `build_psp.py game` puts a
single cabinet's settings in `rom/` instead (LINK ID SINGLE, CABINET
UPLIGHT). In the test menu (Select + L), Select + R moves the cursor and
Select + L changes the value. Credits left from the last session are
cleared at power-on (the cabinet would keep them, and the game would start
from the attract mode on its own); the game then clears its bookkeeping
too (test mode's coin, play and time counters; records and settings are
kept). The draw rate adapts during play: every 2nd frame, down to every
3rd or 4th when the frames take more than 8% longer than the arcade's.
The settings EEPROM and backup RAM are saved to
`save/` next to `EBOOT.PBP` when they change (checked every minute) and at
exit.

## Build notes

- **`int32_t` is `long` in newlib for this target**, as on the Dreamcast:
  the Makefile redefines `__INT32_TYPE__` as `int`.
- **`double` is software**: the Allegrex's FPU is single precision. GCC's
  library does `double` bit-exactly (the self-test), but slowly. The
  geometrizer's per-polygon doubles are float here (`M2_PSP_FAST_GEO`,
  picture only).
- **`-G0`**: no small-data section (the generated code's globals do not fit
  the 64 KB `$gp` window).
- The program is a PRX in `EBOOT.PBP` with `MEMSIZE=1` in its `PARAM.SFO`
  (the large-memory mode).
