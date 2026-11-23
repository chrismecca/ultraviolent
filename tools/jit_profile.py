#!/usr/bin/env python3
"""Profiles one benchmark workload's generated code (doc/TIER2.adoc "Why").

    tools/jit_profile.py where boot           # host time: generated code, C++, kernel
    tools/jit_profile.py roles cpu            # generated code by role and operation
    tools/jit_profile.py roles cpu --binary build/release/ultraviolent

`where` runs the workload under `perf record` with ULTRAVIOLENT_PERF_MAP and places each sample
in a translation (generated code lives in a memfd mapping perf cannot name) or a C++ symbol.
`roles` runs it with ULTRAVIOLENT_JIT_DUMP as well and places each sample in generated code by
the dump's marks: a block's entry check, one of its operations (by guest mnemonic), its end,
or its out-of-line code. The dump slows translation, so `roles` gives shares within generated
code, not host time. The workload runs as scripts/bench.py runs it, from its fixture.
"""

import argparse
import contextlib
import io
import bisect
import collections
import glob
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import bench  # noqa: E402


def workload_command(name, scratch, binary, engine_args):
    """The command bench.py would run for `name`, with its inputs copied to `scratch`."""
    argv = ["bench.py", "run", "--scratch", scratch, "--binary", binary] + engine_args + [name]
    captured = {}
    real_run = subprocess.run

    def capture(command, **kw):
        if isinstance(command, list) and command[0] == binary:
            captured["command"] = command
            raise SystemExit(0)
        return real_run(command, **kw)

    subprocess.run, old_argv = capture, sys.argv
    sys.argv = argv
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            bench.main()
    except SystemExit:
        pass
    finally:
        subprocess.run, sys.argv = real_run, old_argv
    return captured["command"]


def read_perf_map(path):
    entries = []
    for line in open(path):
        start, size, name = line.split(maxsplit=2)
        entries.append((int(start, 16), int(size, 16), name.strip()))
    entries.sort()
    return entries


def read_dump(path):
    """Each dumped block: (host start, size, guest pc, guest words, [(offset, mark)])."""
    blocks, lines, i = [], open(path).read().split("\n"), 0
    while i < len(lines):
        if lines[i].startswith("block "):
            _, pc, host, size = lines[i].split()
            words = [int(w, 16) for w in lines[i + 1].split()[1:]]
            marks = sorted((int(m.split(":")[0], 16), int(m.split(":")[1]))
                           for m in lines[i + 2].split()[1:])
            blocks.append((int(host, 16), int(size, 16), int(pc, 16), words, marks))
            i += 4
        else:
            i += 1
    blocks.sort()
    return blocks


SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr",
           10: "movz", 11: "movn", 12: "syscall", 15: "sync", 16: "mfhi", 17: "mthi",
           18: "mflo", 19: "mtlo", 20: "dsllv", 22: "dsrlv", 23: "dsrav", 24: "mult",
           25: "multu", 26: "div", 27: "divu", 28: "dmult", 29: "dmultu", 30: "ddiv",
           31: "ddivu", 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or",
           38: "xor", 39: "nor", 42: "slt", 43: "sltu", 44: "dadd", 45: "daddu", 46: "dsub",
           47: "dsubu", 56: "dsll", 58: "dsrl", 59: "dsra", 60: "dsll32", 62: "dsrl32",
           63: "dsra32"}
OPCODE = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu",
          10: "slti", 11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui", 19: "cop1x",
          20: "beql", 21: "bnel", 22: "blezl", 23: "bgtzl", 24: "daddi", 25: "daddiu",
          26: "ldl", 27: "ldr", 32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu",
          38: "lwr", 39: "lwu", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 44: "sdl", 45: "sdr",
          46: "swr", 47: "cache", 48: "ll", 49: "lwc1", 51: "pref", 52: "lld", 53: "ldc1",
          55: "ld", 56: "sc", 57: "swc1", 60: "scd", 61: "sdc1", 63: "sd"}
LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lwc1", "ldc1", "ldl", "ldr", "lwl",
         "lwr", "ll", "lld"}
STORES = {"sb", "sh", "sw", "sd", "swc1", "sdc1", "sdl", "sdr", "swl", "swr", "sc", "scd"}
FLOW = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "beql", "bnel", "blezl", "bgtzl",
        "bltzl", "bgezl", "bltzal", "bgezal", "j", "jal", "jr", "jalr", "bc1"}


def mnemonic(word):
    op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    if op == 0:
        return SPECIAL.get(word & 63, f"special{word & 63}")
    if op == 1:
        return {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 16: "bltzal",
                17: "bgezal"}.get(rt, f"regimm{rt}")
    if op == 16:
        return {0: "mfc0", 1: "dmfc0", 4: "mtc0", 5: "dmtc0"}.get(rs, "cop0")
    if op == 17:
        return "bc1" if rs == 8 else "cop1"
    return OPCODE.get(op, f"op{op}")


def kind(m):
    if m in LOADS:
        return "load"
    if m in STORES:
        return "store"
    if m in FLOW:
        return "branch/jump"
    if m in ("cop1", "cop1x"):
        return "fpu"
    return "alu/other"


def percent(n, of):
    return f"{100 * n / of:5.1f}%" if of else "    -"


def where(samples, perf_map, top):
    starts = [e[0] for e in perf_map]
    kinds, symbols, translations = collections.Counter(), collections.Counter(), collections.Counter()
    for ip, symbol, dso in samples:
        if "memfd" in dso:
            k = bisect.bisect_right(starts, ip) - 1
            if k >= 0 and ip < perf_map[k][0] + perf_map[k][1]:
                name = perf_map[k][2]
                if name.startswith("uv:block_"):
                    kinds["generated code: tier 2" if name.endswith("_t2")
                          else "generated code: tier 1"] += 1
                    translations[name] += 1
                else:
                    kinds["generated code: shared"] += 1
                    symbols[name] += 1
            else:
                kinds["generated code: unnamed"] += 1
        else:
            kinds["host kernel" if "kernel" in dso else "C++"] += 1
            symbols[symbol] += 1
    total = len(samples)
    print(f"{total} samples")
    for name, n in kinds.most_common():
        print(f"  {percent(n, total)}  {name}")
    print("symbols:")
    for name, n in symbols.most_common(top):
        print(f"  {percent(n, total)}  {name[:100]}")
    in_translations = sum(translations.values())
    if in_translations:
        print(f"{len(translations)} translations sampled; the hottest hold:")
        running = 0
        for rank, (name, n) in enumerate(translations.most_common(), 1):
            running += n
            if rank in (1, 10, 100, 1000, 10000):
                print(f"  top {rank:5}: {percent(running, in_translations)}")


def roles(samples, dump, top):
    starts = [b[0] for b in dump]
    role, operations, kinds = collections.Counter(), collections.Counter(), collections.Counter()
    placed = 0
    names = {-1: "block entry check", -2: "block end", -3: "out of line"}
    for ip, _, _ in samples:
        k = bisect.bisect_right(starts, ip) - 1
        if k < 0 or ip >= dump[k][0] + dump[k][1]:
            continue
        placed += 1
        host, _, _, words, marks = dump[k]
        offsets = [m[0] for m in marks]
        j = bisect.bisect_right(offsets, ip - host) - 1
        mark = marks[j][1] if j >= 0 else None
        if mark is None:
            role["dispatch entry (TLB test)"] += 1
        elif mark >= 0:
            m = mnemonic(words[mark]) if mark < len(words) else "?"
            role["operation"] += 1
            operations[m] += 1
            kinds[kind(m)] += 1
        else:
            role[names[mark]] += 1
    print(f"{len(samples)} samples, {placed} in dumped generated code "
          f"({percent(placed, len(samples)).strip()}); shares of those:")
    for name, n in role.most_common():
        print(f"  {percent(n, placed)}  {name}")
    print("operations by kind:")
    for name, n in kinds.most_common():
        print(f"  {percent(n, placed)}  {name}")
    print("operations:")
    for name, n in operations.most_common(top):
        print(f"  {percent(n, placed)}  {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["where", "roles"])
    parser.add_argument("workload")
    parser.add_argument("--binary", default="build/pgo/ultraviolent")
    parser.add_argument("--frequency", type=int, default=2000, help="perf samples per second")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--keep", help="keep perf.data, the perf map, and the dump here")
    args, engine_args = parser.parse_known_args()

    work = tempfile.mkdtemp(prefix="jit-profile-")
    scratch = os.path.join(bench.default_scratch() or work, "jit-profile")
    try:
        command = workload_command(args.workload, scratch, args.binary, engine_args)
        env = dict(os.environ, ULTRAVIOLENT_PERF_MAP="1")
        dump = os.path.join(work, "code.dump")
        if args.mode == "roles":
            env["ULTRAVIOLENT_JIT_DUMP"] = dump
        data = os.path.join(work, "perf.data")
        before = set(glob.glob("/tmp/perf-*.map"))
        process = subprocess.Popen(["perf", "record", "-q", "-F", str(args.frequency), "-o", data,
                                    "--"] + command,
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        _, err = process.communicate()
        if process.returncode != 0:
            sys.exit(f"jit_profile: the run failed:\n{err.decode()[-2000:]}")
        maps = sorted(set(glob.glob("/tmp/perf-*.map")) - before, key=os.path.getmtime)
        if not maps:
            sys.exit("jit_profile: no perf map was written")
        perf_map_path = maps[-1]
        script = subprocess.run(["perf", "script", "-F", "ip,sym,dso", "-i", data],
                                capture_output=True, text=True).stdout
        samples = []
        for line in script.splitlines():
            parts = line.split(None, 1)
            if not parts:
                continue
            rest = parts[1] if len(parts) > 1 else ""
            # The dso is the last parenthesized field; generated code's ("/memfd:... (deleted)")
            # has parentheses of its own.
            cut = rest.find(" (/memfd:") if "/memfd:" in rest else rest.rfind(" (")
            symbol, dso = (rest[:cut], rest[cut:]) if cut >= 0 else (rest, "")
            samples.append((int(parts[0], 16), symbol.strip() or "?", dso))
        if args.mode == "where":
            where(samples, read_perf_map(perf_map_path), args.top)
        else:
            roles(samples, read_dump(dump), args.top)
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            for path in [data, perf_map_path] + ([dump] if args.mode == "roles" else []):
                shutil.copy(path, args.keep)
        os.remove(perf_map_path)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
