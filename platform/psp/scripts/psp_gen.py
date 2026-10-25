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


# m2recomp's dispatch: by 128-byte block, then by IP.
OUTER_SWITCH = re.compile(r"^    switch \(\(c\.m_IP - (0x[0-9a-f]+)u\) >> 7\) \{[^\n]*\n", re.M)
PLAIN_SWITCH = re.compile(r"^    switch \(c\.m_IP\) \{\n", re.M)  # (a chunk whose IPs are far apart)
DISPATCH_CASE = re.compile(r"case (0x[0-9a-f]+)u: goto (L_[0-9a-f]+);")


def flat_dispatch(text, name):
    """The dispatch as one switch on the IP's word from the chunk's base.
    m2recomp's switch by block, then by IP, became compare trees (the inner
    switches' cases are too sparse for -Os's jump tables): li, beq and sltiu
    per case, a quarter of chunk_000's instructions, and GCC laid them out
    among the instructions' own code, so the lines the instruction cache
    held for a hot loop held some of them too. One table instead (in
    .rodata), and the instructions' code in a run of their own. Where it
    goes for an IP is unchanged: the same label, or return for an IP the
    chunk has no code for."""
    m = OUTER_SWITCH.search(text) or PLAIN_SWITCH.search(text)
    if not m:
        return text  # (the TGP's and the sound's code: not this dispatch)
    end = text.index("\n    }\n", m.end()) + len("\n    }\n")
    cases = []
    for line in text[m.end():end].splitlines():
        s = line.strip()
        c = DISPATCH_CASE.fullmatch(s)
        if c:
            ip = int(c.group(1), 16)
            if ip & 3 or c.group(2) != f"L_{ip:08x}":
                sys.exit(f"psp_gen: {name}: dispatch case {s} not as expected")
            cases.append(ip)
        elif not (s in ("", "default: return;", "}") or s == "switch (c.m_IP) {" or re.fullmatch(r"case \d+u:", s)):
            sys.exit(f"psp_gen: {name}: dispatch line not as expected: {s}")
    if not cases or len(set(cases)) != len(cases):
        sys.exit(f"psp_gen: {name}: no dispatch cases, or one twice")
    # A table of the labels' addresses (GCC's labels as values), not a
    # switch: GCC made the switch a compare tree again where most cases go
    # to the same place (dual_modes' fast version), and laid it out among
    # the instructions. One table per run of IPs (chunk_017's are in three
    # places, megabytes apart).
    cases.sort()
    runs, start = [], 0
    for i in range(1, len(cases) + 1):
        if i == len(cases) or cases[i] - cases[i - 1] > 4096:
            runs.append(cases[start:i])
            start = i
    flat = ["    // (psp_gen.py) a jump table on the IP's word for each run of IPs (flat_dispatch)\n"]
    for r, run in enumerate(runs):
        base, span = run[0], (run[-1] - run[0]) // 4 + 1
        at = {(ip - base) >> 2: ip for ip in run}
        table = "".join(f"            &&L_{at[i]:08x},\n" if i in at else "            &&no_code,\n" for i in range(span))
        flat.append(f"    {{\n"
                    f"        static const void *const kTable{r}[] = {{\n{table}        }};\n"
                    f"        const uint32_t i_ = c.m_IP - 0x{base:08x}u;\n"
                    f"        if (!(i_ & 3u) && i_ < {span * 4}u) goto *kTable{r}[i_ >> 2];\n"
                    f"    }}\n")
    flat.append("no_code: // (an IP this chunk has no code for)\n    return;\n")
    return text[:m.start()] + "".join(flat) + text[end:]


LABEL = re.compile(r"^L_([0-9a-f]{8}):")
HEAD_A = re.compile(r"^    if \(__builtin_expect\(!--left, 0\)\) \{ x_off = \d+; goto x_exit_\d+; \}$")
HEAD_B = "    if (__builtin_expect(!--left, 0)) goto recheck;"
GOTO_LABEL = re.compile(r"goto (L_[0-9a-f]{8});")
CASE_GOTO = re.compile(r"^(\s*case [^:]+: )goto (L_[0-9a-f]{8});$")
TABLE_ENTRY = re.compile(r"^(\s*)&&(L_[0-9a-f]{8}),$")
EVENT_MOVED = "{ n_c = n_c - left + 1; left = 1; }"  # (derive_n's: the next instruction checks in full)
FAST_AFTER = 64  # the precise code hands back once this many instructions are left before the next event


def dual_modes(text, name):
    """Two versions of an i960 chunk: the precise one (chunk_NNN_p: the code
    as it is, counting down `left` before every instruction) and a fast one
    (chunk_NNN) that counts a basic block at once: at the block's first
    instruction, if more instructions are left before the next lockstep
    event than the block holds, all of them run and `left` drops by their
    number; else the precise code takes over there, with that count, and
    meets the event at its instruction, as before. The precise code hands
    back once an event is behind it and more than FAST_AFTER are left. Each
    instruction that calls the runtime, branches or touches `left` ends its
    block, so within a block `left` means what it means in the precise code
    wherever it is read. Entered at an IP that does not start a block (an
    interrupt's return), the fast code hands over too. The same instructions
    run, the events at the same counts: only the countdown's code moves, from
    every instruction (three MIPS instructions and an exit) to every block.
    gen_table.cpp's run (dual_table) calls the version the last one asked
    for, again while one hands over."""
    lines = text.split("\n")
    try:
        f0 = next(i for i, l in enumerate(lines) if re.match(r"^void chunk_\d+\(Env &e\) \{$", l))
        f1 = max(i for i, l in enumerate(lines) if l == "}")
        exits = next(i for i, l in enumerate(lines) if l.startswith("    // (psp_gen.py) the countdown's exits"))
        disp = next(i for i in range(f0, f1) if lines[i] == "dispatch:")
        resume = next(i for i in range(f0, f1) if lines[i] == "resume:")
        first = next(i for i in range(f0, f1) if LABEL.match(lines[i]))
    except StopIteration:
        sys.exit(f"psp_gen: {name}: not the chunk layout dual_modes expects")
    if lines[resume - 1] != "    n_c = left - 1;" or "    goto resume;" not in lines[disp:resume]:
        sys.exit(f"psp_gen: {name}: recheck / dispatch not as dual_modes expects")
    fn = re.match(r"^void (chunk_\d+)\(", lines[f0]).group(1)
    handoff_in = ("    if (g_left) { left = g_left; g_left = 0; n_c = left - 1; goto resume; } "
                  "// (psp_gen.py dual_modes) handed over with the count until the next event")

    # The precise version: the code as it is, taking and handing over the count.
    precise = lines[f0:f1 + 1]
    precise[0] = precise[0].replace(f"void {fn}(", f"void {fn}_p(")
    precise.insert(disp - f0, handoff_in)
    r = resume - f0 + 1
    precise.insert(r, f"    if (left > {FAST_AFTER}u) {{ g_left = left; g_precise = false; g_handoff = true; return; }} "
                      "// (dual_modes) back to the fast code")

    # The fast version: instructions, their blocks.
    insns = []  # (ip, line indices)
    for i in range(first, exits):
        m = LABEL.match(lines[i])
        if m:
            insns.append((int(m.group(1), 16), [i]))
        elif insns:
            insns[-1][1].append(i)
    targets = set(GOTO_LABEL.findall("\n".join(lines[first:exits])))
    lo = min(ip for ip, _ in insns)
    out = lines[f0:first]
    heads, block_starts = set(), []
    ends_block = True  # (the chunk's first instruction starts one)
    bodies = []  # (ip, label line, body lines, a load or store through a helper only)
    for ip, idx in insns:
        body = []
        for j in idx[1:]:
            l = lines[j]
            if HEAD_A.match(l) or l == HEAD_B:
                continue
            body.append(l)
        head = ends_block or f"L_{ip:08x}" in targets
        if head:
            heads.add(ip)
            block_starts.append(len(bodies))
        joined = "\n".join(body)
        # A load or store through a psp_ helper: it reads `left` only as
        # n_c - left and, when the next event moved, sets left = 1. It stays
        # inside its block: the count it reads is corrected by the block's
        # instructions after it, and a moved event hands over to the precise
        # code at the next instruction (below).
        plain = joined.replace(EVENT_MOVED, "")
        helper = ("left" in joined and plain.count("left") == plain.count("n_c - left") and
                  not any(k in joined for k in ("goto ", "return", "c.m_IP", "dispatch")))
        bodies.append((ip, idx[0], body, helper))
        ends_block = not helper and any(k in joined for k in ("goto ", "return", "left", "c.m_IP", "dispatch"))
    windows = set()
    for b, start in enumerate(block_starts):
        stop = block_starts[b + 1] if b + 1 < len(block_starts) else len(bodies)
        k = stop - start
        for n in range(start, stop):
            ip, label, body, helper = bodies[n]
            out.append(lines[label])
            if n == start:
                d = ip - lo
                windows.add(d >> WINDOW_BITS)
                out.append(f"    if (__builtin_expect(left <= {k}u, 0)) {{ x_off = {(d & ((1 << WINDOW_BITS) - 1)) >> 2}; "
                           f"goto h_exit_{d >> WINDOW_BITS}; }} left -= {k}u;")
            if helper:
                # `left` as the precise code has it here: after this block's
                # later instructions are added back.
                after = stop - n - 1
                true_left = f"(left + {after}u)" if after else "left"
                if n + 1 >= len(bodies):
                    sys.exit(f"psp_gen: {name}: a helper call ends the chunk")
                moved = (f"{{ n_c = n_c - {true_left} + 1; left = 1; c.m_IP = 0x{bodies[n + 1][0]:08x}u; "
                         f"goto to_precise; }}")
                body = [l.replace(EVENT_MOVED, "\0").replace("n_c - left", f"n_c - {true_left}").replace("\0", moved)
                        for l in body]
            out.extend(body)
    out.extend(lines[exits:f1])
    out.extend(f"h_exit_{w}: c.m_IP = 0x{lo + (w << WINDOW_BITS):08x}u + uint32_t(x_off) * 4u; goto to_precise;"
               for w in sorted(windows))
    out += ["to_precise: // (dual_modes) the precise code from c.m_IP, with the count until the next event",
            "    ls.count += n_c + 1 - left;",
            "    ls.pending = 0;",
            "    g_left = left;",
            "    g_precise = true;",
            "    g_handoff = true;",
            "    return;",
            "}"]
    # The fast version's dispatch: the count taken at once (as the precise
    # code's first instruction would take it), and the IPs that start no
    # block to the precise code.
    head_lines = out[:first - f0]
    d = disp - f0
    head_lines.insert(d, handoff_in)
    k = next(i for i in range(d, len(head_lines)) if head_lines[i] == "    goto resume;")
    head_lines[k] = "    left = 0; goto recheck; // (dual_modes) the count until the next event, taken here"
    for i, l in enumerate(head_lines):
        m = CASE_GOTO.match(l)
        if m and int(m.group(2)[2:], 16) not in heads:
            head_lines[i] = f"{m.group(1)}goto to_precise;"
        m = TABLE_ENTRY.match(l)
        if m and int(m.group(2)[2:], 16) not in heads:
            head_lines[i] = f"{m.group(1)}&&to_precise,"
    fast = head_lines + out[first - f0:]
    return "\n".join(lines[:f0] + fast + [""] + precise + lines[f1 + 1:])


def dual_table(text):
    """gen_table.cpp for dual_modes: each chunk's precise version beside it,
    and run calling the version asked for, again while one hands over."""
    text = re.sub(r"^void (chunk_\d+)\(Env &\);$", r"void \1(Env &);\nvoid \1_p(Env &);", text, flags=re.M)
    text = text.replace("struct Range { uint32_t first, last; void (*fn)(Env &); };",
                        "struct Range { uint32_t first, last; void (*fn)(Env &), (*precise)(Env &); };")
    text = re.sub(r"^(    \{0x[0-9a-f]+u, 0x[0-9a-f]+u, )(chunk_\d+)\},$", r"\1\2, \2_p},", text, flags=re.M)
    old = "    if (r != std::begin(kChunks) && a <= (r - 1)->last) (r - 1)->fn(e);\n"
    new = ("    if (r == std::begin(kChunks) || a > (r - 1)->last) return;\n"
           "    // (psp_gen.py dual_modes) the fast or the precise version, as the\n"
           "    // last asked; again while one hands over (the same IP, the same chunk).\n"
           "    for (;;) {\n"
           "        g_handoff = false;\n"
           "        (g_precise ? (r - 1)->precise : (r - 1)->fn)(e);\n"
           "        if (!g_handoff) return;\n"
           "    }\n")
    if old not in text or "(*precise)" not in text or "_p}," not in text:
        sys.exit("psp_gen: gen_table.cpp not as dual_table expects")
    text = text.replace(old, new)
    return text.replace('#include "runtime/gen_support.h"\n',
                        '#include "runtime/gen_support.h"\n#include "../platform/psp/game/gen_psp.h" // (psp_gen.py)\n', 1)


def rewrite(text, name):
    if name == "gen_table.cpp" and "struct Range" in text:
        return dual_table(text)
    text = flat_dispatch(text, name)
    text = memory_helpers(text)
    text = move_exits(text, name)
    for old, new in REWRITES:
        text = text.replace(old, new)
    text = derive_n(text, name)
    if name.startswith("chunk_") and "recheck: // left ran out" in text:
        text = dual_modes(text, name)
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
