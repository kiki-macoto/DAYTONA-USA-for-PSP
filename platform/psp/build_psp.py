#!/usr/bin/env python3
"""The PSP port's build driver (Linux, or WSL on Windows; pspdev in
$PSPDEV, default ~/pspdev). Run it after the host build: the importer and
recompilers on this machine, then scripts/recompile.py --set daytona
--build-dir build-daytona (Revision A, 1994, as the Dreamcast port). It only
reads the host build's generated code and ROM images; everything it makes
goes under the host build directory's psp* folders (git-ignored), and nothing
here changes the main tree.

  build_psp.py host     [--host-build-dir build-daytona] [--jobs N]
  build_psp.py selftest [--deploy DIR] [--ppsspp EXE]
  build_psp.py compile  [--host-build-dir build-daytona] [--jobs N]
  build_psp.py game     [--host-build-dir build-daytona] [--jobs N]
                        [--draw-every N] [--inputs SCRIPT] [--nvram DIR]
                        [--overlay PNG] [--deploy DIR] [--ppsspp EXE] [--timeout S]

host:     the host build without the windowed app (no SDL, so nothing to
          install beyond a C++ compiler, CMake and Python): fetches SoftFloat,
          ymfm and the LZMA SDK at setup.py's pinned commits, configures
          build-daytona for Revision A, and runs scripts/recompile.py on your
          roms/daytona.zip (or .7z): the ROM images and generated code the
          other commands use.
selftest: the floating-point self-test (no game code) as an EBOOT.PBP in
          psp/selftest/; with --deploy it is copied to DIR/DAYTONA_SELFTEST,
          and with --ppsspp run there: exit 0 only when every check matches
          the PC's bits.
compile:  compiles the runtime and the generated game code for the PSP and
          reports their size.
game:     the game, as a folder ready for a memory stick: psp/DAYTONA/ with
          EBOOT.PBP and rom/ (your own ROM set's images: build output, never
          share it). --deploy DIR copies it to DIR/DAYTONA (DIR is a memory
          stick's PSP/GAME, or PPSSPP's: on WSL
          /mnt/c/Users/<you>/Documents/PPSSPP/PSP/GAME); --ppsspp then starts
          it there and shows daytona.log until the game ends (a recorded
          race: GAME DONE) or --timeout.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
# The Dreamcast port's rewrite of the generated code (platform/dreamcast/
# scripts/fast_gen.py), copied here so this port builds without that folder.
FAST_GEN = HERE / "scripts" / "fast_gen.py"
# The importer's images the game reads (StickRom's regions, the TGP tables,
# the sound program and PCM ROMs for the native audio).
ROM_FILES = ["program", "main_data", "polygons", "textures", "copro_data", "copro_tables",
             "sound_program", "pcm1", "pcm2"]


def run(command, **kw):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kw)


def make(args, out, targets, extra=()):
    pspdev = Path(os.environ.get("PSPDEV", Path.home() / "pspdev"))
    if not (pspdev / "bin" / "psp-g++").is_file():
        sys.exit(f"build_psp: no pspdev toolchain at {pspdev} (set PSPDEV)")
    out.mkdir(parents=True, exist_ok=True)
    run(["make", "-C", HERE, f"-j{args.jobs}", f"OUT={out}", f"PSPDEV={pspdev}", *extra, *targets])


def host_dir(args):
    return (ROOT / args.host_build_dir).resolve()


def gen_dir(args):
    gen = host_dir(args) / "gen"
    if not (gen / "daytona" / "gen_table.cpp").is_file():
        sys.exit(f"build_psp: no generated code in {gen / 'daytona'}; run "
                 "scripts/recompile.py --set daytona --build-dir build-daytona first")
    return gen


def windows_path(path):
    """A WSL path as Windows names it (for PPSSPP, a Windows program)."""
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()


def deploy(folder, dest_parent, name):
    dest = Path(dest_parent) / name
    dest.mkdir(parents=True, exist_ok=True)
    for src in folder.rglob("*"):
        if src.is_file():
            target = dest / src.relative_to(folder)
            target.parent.mkdir(parents=True, exist_ok=True)
            # Large ROM images only when they changed (a memory stick is slow).
            if target.is_file() and target.stat().st_size == src.stat().st_size and \
                    target.stat().st_mtime >= src.stat().st_mtime and src.name != "EBOOT.PBP":
                continue
            shutil.copy2(src, target)
    print(f"build_psp: copied to {dest}")
    return dest


def run_ppsspp(exe, eboot, log, markers, timeout):
    """Start PPSSPP on eboot and print log as it grows, until a marker line
    appears (returned) or timeout; then close PPSSPP."""
    if log.exists():
        log.unlink()
    exe = Path(exe)
    target = windows_path(eboot) if str(exe).endswith(".exe") and "microsoft" in os.uname().release.lower() else str(eboot)
    proc = subprocess.Popen([str(exe), target])
    seen, shown, start = None, 0, time.time()
    try:
        while time.time() - start < timeout and proc.poll() is None and not seen:
            time.sleep(1)
            if not log.exists():
                continue
            text = log.read_text(errors="replace")
            print(text[shown:], end="", flush=True)
            shown = len(text)
            seen = next((m for m in markers if m in text), None)
    finally:
        if proc.poll() is None:
            # From WSL, proc is the interop stub (a Linux pid): killing it
            # leaves the Windows program running. Close it by image name
            # (any other PPSSPP window of the same program goes too).
            if str(exe).endswith(".exe"):
                subprocess.run(["taskkill.exe", "/IM", exe.name, "/F"], capture_output=True)
            proc.kill()
    return seen


def host(args):
    if not any((ROOT / "roms" / f"daytona.{e}").is_file() for e in ("zip", "7z")):
        sys.exit("build_psp: put your own Revision A ROM set at roms/daytona.zip (or .7z) first")
    sys.path.insert(0, str(ROOT / "scripts"))
    import setup  # scripts/setup.py: the pinned third-party commits and its fetch
    os.makedirs(setup.EXTERN, exist_ok=True)
    setup.fetch(*setup.SOFTFLOAT, "softfloat")
    setup.fetch(*setup.YMFM, "ymfm")
    setup.fetch(*setup.LZMA_SDK, "lzma")
    build = host_dir(args)
    if not (build / "CMakeCache.txt").is_file():
        run(["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DM2_ROMSET=daytona",
             *(["-G", "Ninja"] if shutil.which("ninja") else [])])
    # At most two compiles at once: the generated chunks take over a GB each
    # in the host compiler, and four at once in WSL's default 6 GB stopped the
    # whole WSL VM.
    env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL=str(host_jobs(args)))
    run([sys.executable, "scripts/recompile.py", "--set", "daytona", "--build-dir", args.host_build_dir], env=env)


def host_jobs(args):
    return max(1, min(args.jobs, 2))


def single_cabinet_nvram(args):
    """The PSP's starting settings: Revision A's factory settings are a linked
    twin cabinet, which waits for the other cabinet. m2run plays
    scripts/single_cabinet.txt through the test menu (LINK ID SINGLE, CABINET
    UPLIGHT) and saves the EEPROM and backup RAM."""
    host = host_dir(args)
    out = host / "psp" / "nvram"
    if all((out / n).is_file() for n in ("ioboard_eeprom.bin", "backup_ram.bin")):
        return out
    m2run = host / "m2run"
    if not m2run.is_file():
        run(["cmake", "--build", host, "--parallel", host_jobs(args), "--target", "m2run"])
    out.mkdir(parents=True, exist_ok=True)
    script = HERE / "scripts" / "single_cabinet.txt"
    run([m2run, host / "rom_cache" / "daytona", 7000, "--inputs", script, "--save-nvram", out],
        stdout=subprocess.DEVNULL)
    return out


def selftest(args):
    out = host_dir(args) / "psp"
    make(args, out, ["selftest"])
    print(f"build_psp: {out / 'selftest' / 'EBOOT.PBP'}")
    if not args.deploy:
        return
    dest = deploy(out / "selftest", args.deploy, "DAYTONA_SELFTEST")
    if args.ppsspp:
        seen = run_ppsspp(args.ppsspp, dest / "EBOOT.PBP", dest / "selftest.log",
                          ("SELFTEST PASS", "SELFTEST FAIL"), 60)
        if seen != "SELFTEST PASS":
            sys.exit("build_psp: self-test " + ("failed" if seen else "did not finish"))


def psp_gen(out, src):
    dst = out / "gen_psp" / src.name
    run([sys.executable, HERE / "scripts" / "psp_gen.py", src, dst])
    return dst


def game(args):
    host = host_dir(args)
    gen = gen_dir(args)
    out = host / "psp"
    # The i960 code with fewer lockstep checks and the TGP code with its
    # count in a local: the Dreamcast's rewrite (scripts/fast_gen.py, from
    # platform/dreamcast), the same on any target; it rewrites only the files whose
    # output changes.
    fast = out / "gen_fast" / "daytona"
    fast_tgp = out / "gen_fast" / "daytona_tgp"
    run([sys.executable, FAST_GEN, gen / "daytona", fast])
    run([sys.executable, FAST_GEN, gen / "daytona_tgp", fast_tgp])
    # Then the PSP's own pass (scripts/psp_gen.py): the rare checks' code
    # out of the straight path, for the instruction cache. Same behaviour.
    fast, fast_tgp = psp_gen(out, fast), psp_gen(out, fast_tgp)
    # The recorded input script, compiled in (game/inputs.h; empty: the pad),
    # and the draw rate. Written only when it changes, so make rebuilds only then.
    text = (ROOT / args.inputs).read_text() if args.inputs else ""
    header = out / "game" / "inputs.h"
    content = ("// Written by build_psp.py: the recorded input script (--inputs), or none,\n"
               "// the frames per picture (--draw-every) and the frame dumps (--dump-every).\n"
               f"static const char kInputs[] = R\"INPUTS({text})INPUTS\";\n"
               f"static const int kDrawEvery = {args.draw_every};\n"
               f"static const int kDumpEvery = {args.dump_every if args.inputs else 0};\n")
    header.parent.mkdir(parents=True, exist_ok=True)
    if not header.is_file() or header.read_text() != content:
        header.write_text(content, newline="\n")
    make(args, out, ["game"], [f"GEN={gen}", f"GEN_I960={fast}", f"GEN_TGP={fast_tgp}"])

    # The folder for the memory stick: the program and the ROM images.
    images = host / "rom_cache" / "daytona"
    package = out / "DAYTONA"
    (package / "rom").mkdir(parents=True, exist_ok=True)
    shutil.copy2(out / "game" / "EBOOT.PBP", package / "EBOOT.PBP")
    for name in ROM_FILES:
        src = images / f"{name}.bin"
        if not src.is_file():
            sys.exit(f"build_psp: {src} is missing (run scripts/recompile.py --set daytona first)")
        dst = package / "rom" / src.name
        if not dst.is_file() or dst.stat().st_mtime < src.stat().st_mtime:
            shutil.copy2(src, dst)
    # The settings to start from (save/ on the PSP wins once it exists; a
    # recorded race always starts from these): --nvram, or a single cabinet.
    nvram = Path(args.nvram) if args.nvram else single_cabinet_nvram(args)
    for name in ("ioboard_eeprom.bin", "backup_ram.bin"):
        src = nvram / name
        if src.is_file():
            shutil.copy2(src, package / "rom" / name)
    # The ROM pages the game loads at the start (rom/rom_pages.bin): the
    # union of recorded runs' save/rom_pages.bin, kept as psp/rom_pages.bin
    # (made with PPSSPP; see HANDOFF.md). The player's own save/ list adds to it.
    pages = out / "rom_pages.bin"
    if pages.is_file():
        shutil.copy2(pages, package / "rom" / "rom_pages.bin")
    # The player's settings (game/main.cpp's set_cpu_clock), all commented
    # out: written once, so an edited copy in the package stays.
    settings = package / "settings.txt"
    if not settings.is_file():
        settings.write_text(
            "# Daytona USA (PSP) settings. Remove the # in front of a line to use it.\n"
            "#\n"
            "# The CPU clock in MHz (222-444). Without this line the game keeps the\n"
            "# custom firmware's overclock when it is set past 333, else runs at 333.\n"
            "# Past 333 needs the custom firmware; whether a console is stable there\n"
            "# is the console's own.\n"
            "#cpu 333\n", newline="\r\n")
    # A picture at the sides of the screen (a PNG: scripts/make_overlay.py):
    # overlay.bin, which the game shows when it is there.
    if args.overlay:
        run([sys.executable, HERE / "scripts" / "make_overlay.py", Path(args.overlay).resolve(),
             package / "overlay.bin"])
    print(f"build_psp: {package} (EBOOT.PBP and rom/: copy the folder to the memory stick's PSP/GAME)")
    if not args.deploy:
        return
    dest = deploy(package, args.deploy, "DAYTONA")
    if args.ppsspp:
        markers = ("GAME DONE", "GAME FAILED")
        seen = run_ppsspp(args.ppsspp, dest / "EBOOT.PBP", dest / "daytona.log", markers, args.timeout)
        if args.inputs and seen != "GAME DONE":
            sys.exit("build_psp: game " + ("failed" if seen else "did not finish"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["host", "selftest", "compile", "game"])
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--host-build-dir", default="build-daytona")
    ap.add_argument("--draw-every", type=int, default=2, choices=[0, 1, 2, 3, 4],
                    help="game: frames per picture (with the buttons, the least: it adapts up to every 4th; "
                         "0: adaptive from 2, a recorded script too)")
    ap.add_argument("--inputs", help="game: a recorded input script (scripts/inputs) played instead of the pad")
    ap.add_argument("--dump-every", type=int, default=0,
                    help="game with --inputs: write the drawn frames whose number is a multiple of N to shots/ next "
                         "to EBOOT.PBP (scripts/shot2png.py converts them; compare with m2run --dump --every N; "
                         "use --draw-every 1 so every such frame is drawn)")
    ap.add_argument("--nvram", help="game: settings EEPROM and backup RAM to start from (the desktop's data folder; "
                    "default: a single cabinet, made by m2run with scripts/single_cabinet.txt)")
    ap.add_argument("--overlay", help="game: a PNG shown around the game (its see-through or white area is the "
                    "game's window): made into overlay.bin next to EBOOT.PBP")
    ap.add_argument("--deploy", help="copy the result into this PSP/GAME folder (a memory stick's, or PPSSPP's)")
    ap.add_argument("--ppsspp", default=os.environ.get("PPSSPP"),
                    help="run it in PPSSPP after --deploy (default: $PPSSPP)")
    ap.add_argument("--timeout", type=int, default=3600, help="game: seconds to watch PPSSPP")
    args = ap.parse_args(argv)
    if args.ppsspp and not args.deploy:
        sys.exit("build_psp: --ppsspp needs --deploy (PPSSPP runs it from its memory stick folder)")
    if args.command == "host":
        host(args)
    elif args.command == "selftest":
        selftest(args)
    elif args.command == "compile":
        make(args, host_dir(args) / "psp", ["game-sizes"], [f"GEN={gen_dir(args)}"])
    else:
        game(args)


if __name__ == "__main__":
    main()
