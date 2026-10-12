# How to compile the PSP version

The PSP port runs the recompiled game (Daytona USA, Revision A, 1994) on a
PSP-2000, PSP-3000 or PSP Go with custom firmware. It is still in progress:
on a PSP-3000 (ARK-5, 333 MHz) a race runs at about 23 ms a frame, against
the arcade's 17.4 (57.52 frames/s), with 20 cars in the beginner race and 10
in the others (the arcade has 40, 20 and 30). Details:
[platform/psp/README.md](platform/psp/README.md) and
[platform/psp/HANDOFF.md](platform/psp/HANDOFF.md).

No ROM data is in this repository, and the build does not download any. You
supply your own ROM set. The folder the build makes holds images taken from
your ROM set, so do not share it.

## You need

* **Linux**, or **WSL on Windows** (Ubuntu 24.04 tested), with Git, Python 3,
  CMake, Make and a C++20 compiler (GCC 13 or later):

  ```sh
  sudo apt update
  sudo apt install -y git python3 cmake make g++ ninja-build
  ```

  Clone the repository and build it **inside the WSL file system** (for
  example `~/daytona-arcade-recomp`), not under `/mnt/c`. Building there is
  much slower. The host recompile compiles large generated files (over 1 GB
  of memory each, two at once), so give WSL at least 6 GB of memory.

* **[pspdev](https://github.com/pspdev/pspdev)** (psp-gcc and PSPSDK), at
  `~/pspdev` or wherever `PSPDEV` points. GCC 15.2 was tested. The prebuilt
  release already has everything this build links against, including the
  `pspsystemctrl_user` and `pspkubridge` libraries:

  ```sh
  cd ~
  wget https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
  tar xf pspdev-ubuntu-latest-x86_64.tar.gz      # makes ~/pspdev
  echo 'export PSPDEV=~/pspdev' >> ~/.bashrc
  echo 'export PATH=$PATH:$PSPDEV/bin' >> ~/.bashrc
  source ~/.bashrc
  psp-gcc --version
  ```

* **Your own Revision A ROM set** (MAME's `daytona`) at
  `roms/daytona.zip` (or `.7z`) in the repository. Other revisions
  (`daytona93`, ...) are not used by the PSP build.

* To run it: a **PSP-2000/3000/Go with custom firmware** (ARK-4/ARK-5 tested
  on a PSP-3000), or **[PPSSPP](https://www.ppsspp.org)** (1.19.3 tested) on
  the PC. A PSP-1000 does not have enough memory.

## Build

From the repository's root, in Linux or WSL:

```sh
git clone https://github.com/<you>/daytona-arcade-recomp.git
cd daytona-arcade-recomp
cp /path/to/daytona.zip roms/

# 1. The host side, once and again after you update the repository: fetches
#    the pinned third-party sources (SoftFloat, ymfm, LZMA SDK) into extern/,
#    configures build-daytona/, and recompiles your ROM set's programs to C++.
python3 platform/psp/build_psp.py host

# 2. The PSP program and the folder for the memory stick.
python3 platform/psp/build_psp.py game
```

The result is the folder **`build-daytona/psp/DAYTONA/`**:

| File | What it is |
| --- | --- |
| `EBOOT.PBP` | The game |
| `rom/` | Images taken from your ROM set (never share them), and the single-cabinet settings the game starts with |
| `settings.txt` | Optional settings (the CPU clock); every line is commented out at first |

Options for `game`:

| Option | What it does |
| --- | --- |
| `--jobs N` | Compiles at once for the PSP side (the default is the number of CPUs; the host side uses at most 2) |
| `--draw-every N` | Draws every Nth frame at first (1-4, default 2); the game draws less often by itself when frames run late |
| `--overlay PNG` | A picture shown at the sides of the screen, such as a cabinet's bezel (see below) |
| `--deploy DIR` | Copies the folder to `DIR/DAYTONA`, for example a memory stick's `PSP/GAME`, or PPSSPP's (on WSL, `/mnt/c/Users/<you>/Documents/PPSSPP/PSP/GAME`) |
| `--ppsspp EXE` | With `--deploy`, starts PPSSPP and shows the game's log (on WSL, `"/mnt/c/Program Files/PPSSPP/PPSSPPWindows64.exe"`) |

`PROFILE_TGP=1 python3 platform/psp/build_psp.py game` makes a build that
adds the CPU's hardware counters and per-part timings to the log. It is
about 1 ms a frame slower. Use it to measure, not to play.

The PSP side is a Makefile (`platform/psp/Makefile`) that `build_psp.py`
runs. After you change only the frontend (`platform/psp/game/`), running
`game` again rebuilds only what changed.

## Install

1. Copy `build-daytona/psp/DAYTONA` to the memory stick's `PSP/GAME/`, so
   that you have `PSP/GAME/DAYTONA/EBOOT.PBP`.
2. Start "Daytona USA" from the XMB.

The game writes `daytona.log` next to `EBOOT.PBP`, and its settings and
records to `save/`. To update, copy `EBOOT.PBP` and `rom/` over the old
ones. Copying them does not touch `save/`. Copy `settings.txt` only if you
want the template back, because it replaces your own.

## The CPU clock

By default the game runs at 333 MHz, the PSP's standard maximum. When the
custom firmware's overclock already set a higher clock, the game keeps it. To
choose a clock, put this line in `PSP/GAME/DAYTONA/settings.txt`:

```
cpu 333
```

Values from 222 to 444 are accepted. The bus runs at half the CPU clock.
Clocks above 333 MHz are asked of the custom firmware (`sctrlHENSetSpeed`),
but the firmware does not always set them: ARK-5 on a PSP-3000 kept 333 MHz
when 440 was asked. The first line of `daytona.log` says which clock the game
runs at and why.

## An overlay (bezel)

The game's picture takes the middle 351 x 272 of the PSP's 480 x 272
screen. A picture of your own can fill the black sides:

```sh
python3 platform/psp/build_psp.py game --overlay my_bezel.png
```

This writes `overlay.bin` next to `EBOOT.PBP`, and the game shows it when
the file is there. Delete the file to go back to black sides. Copy
`overlay.bin` to the memory stick along with the rest of the folder.

* The picture must be a PNG with 8 bits a channel. Convert other formats
  (WebP, JPEG) to PNG first.
* The game's window is the picture's see-through area, or its white area
  when the picture has no transparency. Give `--window X0,Y0,X1,Y1` to
  `platform/psp/scripts/make_overlay.py` if neither works.
* The picture is scaled so that its window is the screen's full height,
  and centred. What falls past the screen's edges is cut off. A 16:9
  picture with a 4:3 window fits well.
* The picture is put on the screen once, at the start. It costs no time
  while the game runs.

## Controls

| PSP | Cabinet |
| --- | --- |
| Analog stick (left and right only) | Steering |
| Cross / Square | Accelerator / brake |
| R / L | Shift up / down |
| D-pad | View buttons |
| Start | Start (and a coin when there is no credit) |
| Select | Coin |
| Select + L, Select + R | Test, service |

## Troubleshooting

* **`put your own Revision A ROM set at roms/daytona.zip`**: the ROM set is
  missing or has another name. It has to be MAME's `daytona` set.
* **`psp-g++: not found`**: pspdev is not on `PATH`, or `PSPDEV` is not set
  (see above).
* **WSL stops during `host`**: WSL ran out of memory. Give it more in
  `%UserProfile%\.wslconfig` (`[wsl2]`, then `memory=8GB` on the next line)
  and run `host` again; it carries on from where it stopped.
* **A file you deleted from `build-daytona/psp` is not rebuilt**: the
  Makefile keeps intermediate files without tracking them (`.SECONDARY`).
  Touch the source file, or delete `build-daytona/psp/game` to rebuild the
  whole PSP side.
* **It runs too slowly in PPSSPP, or too fast**: PPSSPP's speed is not a
  console's (it does not model the PSP's caches). Judge the speed on a PSP.
