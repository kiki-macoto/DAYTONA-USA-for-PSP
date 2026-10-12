#!/usr/bin/env python3
"""The PSP's pass over the generated code, after platform/dreamcast/scripts/
fast_gen.py: the code of the checks that almost never fire moved out of the
straight path.

On a console the game's frame is instruction-cache bound (the CPU's
profiler: 84,500 instruction misses a race frame against 11,100 data
misses, half the cycles waiting on memory), and each register-only i960
instruction carried its own "left ran out" exit inline:

    if (!--left) { c.m_IP = 0x00011688u; goto recheck; }

GCC (-Os; -freorder-blocks-algorithm=stc, -fno-reorder-blocks and a block
of its own per exit did no better) leaves that block, four or five
instructions, right after the branch: a different constant each, so it is
neither shared nor moved. Here it becomes

    if (__builtin_expect(!--left, 0)) { x_off = 1650; goto x_exit_0; }
    ...
    x_exit_0: c.m_IP = 0x0000fc8cu + uint32_t(x_off) * 4u; goto recheck;

one shared exit per 128 KB of IPs (an offset in words fits one MIPS li),
after the function's last statement (a goto: nothing falls into them). GCC
then puts each `li` and branch at the function's end and the straight path
keeps the decrement and a branch-likely (chunk_008: 121.4 -> 110.1 KB). The
other rare checks get __builtin_expect. What runs, and in which order, is
unchanged.

    psp_gen.py SRC_DIR DST_DIR

Writes DST_DIR/NAME for every SRC_DIR/*.cpp, only when the result differs
from what is there (make then rebuilds only those).
"""
import re
import sys
from pathlib import Path

EXIT = re.compile(r"if \(!--left\) \{ c\.m_IP = (0x[0-9a-f]+u); goto recheck; \}")

# (text, replacement): exact strings of fast_gen.py's and m2recomp's output.
REWRITES = [
    # i960: an instruction's lockstep countdown, and a runtime call that
    # moved the lockstep's next event.
    ("if (!--left)", "if (__builtin_expect(!--left, 0))"),
    ("if (ls.epoch != epoch) left = 1;", "if (__builtin_expect(ls.epoch != epoch, 0)) left = 1;"),
    # TGP: the run's budget used up, and a read of the empty input FIFO.
    ("if (count >= budget)", "if (__builtin_expect(count >= budget, 0))"),
    ("if (t.stall)", "if (__builtin_expect(t.stall, 0))"),
]


WINDOW_BITS = 17  # an exit's offset in words fits a MIPS li (0..32767): 128 KB of IPs per shared exit


def move_exits(text, name):
    """The inline exits to shared exits after the function's last statement
    (an i960 chunk is one function, closed by the last line "}" before the
    namespace's): one per 128 KB of IPs from the chunk's lowest exit, each
    exit passing its IP as a word offset into its window."""
    ips = [int(m.group(1)[:-1], 16) for m in EXIT.finditer(text)]
    if not ips:
        return text
    lo = min(ips)
    windows = sorted({(ip - lo) >> WINDOW_BITS for ip in ips})

    def shared(m):
        d = int(m.group(1)[:-1], 16) - lo
        w = d >> WINDOW_BITS
        return (f"if (__builtin_expect(!--left, 0)) {{ x_off = {(d & ((1 << WINDOW_BITS) - 1)) >> 2}; "
                f"goto x_exit_{w}; }}")

    text = EXIT.sub(shared, text)
    lines = text.splitlines(keepends=True)
    end = max(i for i, l in enumerate(lines) if l == "}\n")
    if not lines[end - 1].lstrip().startswith(("{ c.m_IP", "goto ", "return")) or text.count("\nvoid ") != 1:
        sys.exit(f"psp_gen: {name}: not the layout expected (one function ending in a jump)")
    head = next(i for i, l in enumerate(lines) if l.startswith("    uint32_t n = 0;"))
    lines.insert(head + 1, "    int32_t x_off = 0; // (psp_gen.py) a countdown exit's IP, in words into its x_exit_ window\n")
    end += 1
    exits = [f"x_exit_{w}: c.m_IP = 0x{lo + (w << WINDOW_BITS):08x}u + uint32_t(x_off) * 4u; goto recheck;\n"
             for w in windows]
    return "".join(lines[:end] + [
        "    // (psp_gen.py) the countdown's exits: shared blocks, out of the straight path\n"] + exits + lines[end:])


# A plain load or store as fast_gen.py writes it (the IP, the countdown, the
# count the runtime sees, one access between the epoch reads).
MEMORY = re.compile(
    r"(?P<ind>[ \t]*)c\.m_IP = (?P<ip>0x[0-9a-f]+u);\n"
    r"[ \t]*if \(!--left\) goto recheck;\n"
    r"[ \t]*ls\.pending = n;[^\n]*\n"
    r"[ \t]*\{ const uint32_t epoch = ls\.epoch;\n"
    r"[ \t]*(?P<stmt>[^\n]*)\n"
    r"[ \t]*if \(ls\.epoch != epoch\) left = 1; \}\n")
LOADS = {"c.i960_read_dword_unaligned(": ("psp_ld32", "uint32_t"),
         "c.i960_read_word_unaligned(": ("psp_ld16", "uint16_t"),
         "c.bus->read_byte(": ("psp_ld8", "uint8_t")}
STORES = {"c.i960_write_dword_unaligned(": "psp_st32", "c.i960_write_word_unaligned(": "psp_st16",
          "c.bus->write_byte(": "psp_st8"}
ASSIGN = re.compile(r"(R\[\d+\]) = (.*);")
# A call to a constant address: the return IP, the target, the stack pointer.
CALL = re.compile(r"c\.m_IP = (0x[0-9a-f]+u); c\.do_call\((0x[0-9a-f]+u), 0, (R\[\d+\])\);")


def call_args(text, open_at):
    """The arguments of the call whose "(" is at open_at, split at the
    top-level commas, and the index just past its ")"."""
    depth, args, start = 0, [], open_at + 1
    for i in range(open_at, len(text)):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(text[start:i].strip())
                return args, i + 1
        elif ch == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
    raise ValueError("unbalanced call: " + text)


def helper_call(stmt, ip):
    """stmt as a call to gen_psp.h's helper, or None when it is not one plain
    load or store, a call to a constant address or a return."""
    m = CALL.fullmatch(stmt)
    if m:
        ret_ip, target, sp = m.groups()
        return f"if (__builtin_expect(gen::psp_call(e, {target}, {sp}, {ret_ip}, N_MID), 0)) left = 1;"
    if stmt == "c.do_ret();":
        return f"if (__builtin_expect(gen::psp_ret(e, {ip}, N_MID), 0)) left = 1;"
    if stmt.count("c.") != 1:
        return None
    for prefix, fn in STORES.items():
        if stmt.startswith(prefix):
            args, end = call_args(stmt, len(prefix) - 1)
            if len(args) != 2 or stmt[end:] != ";":
                return None
            return f"if (__builtin_expect(gen::{fn}(e, {args[0]}, {args[1]}, {ip}, N_MID), 0)) left = 1;"
    m = ASSIGN.fullmatch(stmt)
    if not m:
        return None
    dst, rhs = m.groups()
    for prefix, (fn, type_) in LOADS.items():
        at = rhs.find(prefix)
        if at < 0:
            continue
        args, end = call_args(rhs, at + len(prefix) - 1)
        if len(args) != 1:
            return None
        rhs = rhs[:at] + f"{type_}(r_)" + rhs[end:]
        return (f"{{ const uint64_t r_ = gen::{fn}(e, {args[0]}, {ip}, N_MID); {dst} = {rhs}; "
                f"if (__builtin_expect(r_ >> 32, 0)) left = 1; }}")
    return None


def memory_helpers(text):
    """Plain loads and stores through gen_psp.h's helpers (the countdown's
    exit stays at the call site, in the form move_exits takes out of line)."""
    count = 0

    def sub(m):
        nonlocal count
        call = helper_call(m.group("stmt"), m.group("ip"))
        if call is None:
            return m.group(0)
        count += 1
        ind = m.group("ind")
        return f"{ind}if (!--left) {{ c.m_IP = {m.group('ip')}; goto recheck; }}\n{ind}{call}\n"

    text = MEMORY.sub(sub, text)
    if count:
        text = text.replace('#include "runtime/gen_support.h"\n',
                            '#include "runtime/gen_support.h"\n#include "../platform/psp/game/gen_psp.h" // (psp_gen.py)\n', 1)
    return text


def derive_n(text, name):
    """fast_gen.py's n (instructions run since the last dispatch or recheck,
    not yet in ls.count) counted by `++n` at the end of every instruction:
    here derived from the countdown, which every instruction decrements once
    at its start. With n_c set where n is reset:

        n = n_c - left        within an instruction (after its countdown)
        n = n_c + 1 - left    between instructions (dispatch)

    The places that set left without counting an instruction keep n:
    `left = 1` within an instruction (an event moved, a divide) sets
    n_c = n_c - left + 1 first; recheck sets n_c = left - 1 for the left it
    resumes with (and ~0 before, for its jump to dispatch with left 0)."""
    if "uint32_t n = 0;" not in text:
        return text.replace("N_MID", "n")
    decl = re.compile(r"uint32_t n = 0; +// [^\n]*\n")
    text, k = decl.subn("uint32_t n_c = 0; // (psp_gen.py) n = n_c - left within an instruction, "
                        "n_c + 1 - left between them (derive_n)\n", text)
    steps = [
        ("dispatch:\n    ls.count += n;\n    n = 0;\n", "dispatch:\n    ls.count += n_c + 1 - left;\n    n_c = 0;\n"),
        ("    ls.count += n;\n    n = 0;\n    ls.pending = 0;\n    left = ls.check(c.m_IP);\n",
         "    ls.count += n_c - left;\n    n_c = ~0u; // (for dispatch, with left 0)\n    ls.pending = 0;\n"
         "    left = ls.check(c.m_IP);\n"),
        ("    ++left; // the instruction decrements it again\n",
         "    ++left; // the instruction decrements it again\n    n_c = left - 1;\n"),
    ]
    for old, new in steps:
        if text.count(old) != 1:
            sys.exit(f"psp_gen: {name}: derive_n: {old.splitlines()[0]!r} found {text.count(old)} times")
        text = text.replace(old, new)
    text = text.replace("    ++n;\n", "")
    text = text.replace("ls.pending = n;", "ls.pending = n_c - left;").replace("N_MID", "n_c - left")
    # left = 1 within an instruction: after a condition, or on its own line
    # (dispatch's is followed by n_c = 0 already, above: "n_c = 0;\n    ls.pending = 0;\n    left = 1;").
    text = text.replace(") left = 1;", ") { n_c = n_c - left + 1; left = 1; }")
    text = text.replace("\n    ls.pending = n_c - left; // the runtime reads ls.count + ls.pending (Lockstep::now)\n    left = 1;\n",
                        "\n    ls.pending = n_c - left; // the runtime reads ls.count + ls.pending (Lockstep::now)\n"
                        "    n_c = n_c - left + 1; left = 1;\n")
    code = "\n".join(l.split("//")[0] for l in text.splitlines())
    left_over = re.findall(r"(?<![\w.>])n(?!\w)|\+\+n\b", code)
    bare = len(re.findall(r"^\s*left = 1;$", code, re.M))
    if k != 1 or left_over or bare != 1:  # (dispatch's own)
        sys.exit(f"psp_gen: {name}: derive_n left n uses {left_over[:5]} or {bare} bare left = 1 lines")
    return text


def rewrite(text, name):
    text = memory_helpers(text)
    text = move_exits(text, name)
    for old, new in REWRITES:
        text = text.replace(old, new)
    text = derive_n(text, name)
    return text


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, dst = Path(argv[1]), Path(argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    written = 0
    for path in sorted(src.glob("*.cpp")):
        out = dst / path.name
        text = rewrite(path.read_text(), path.name)
        if out.is_file() and out.read_text() == text:
            continue
        out.write_text(text, newline="\n")
        written += 1
    print(f"psp_gen: {src} -> {dst}, {written} written")


if __name__ == "__main__":
    main(sys.argv)
