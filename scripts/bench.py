#!/usr/bin/env python3
"""Performance benchmarks: fixed guest workloads, timed on the host.

Every workload starts from the same state and runs to the same guest-defined end (a console
"@stop" or a cycle count), so the guest's output and final cycle count are identical on
every run and every build. Only host time varies. The digest of the guest's console output
and final state is the transparency check: an optimization that changes it changed guest
behavior.

    scripts/bench.py fixture --from local/rc   # once: copy an installed system, log in, snapshot
    scripts/bench.py run                       # all workloads, 3 runs each
    scripts/bench.py run --save base.json      # record results
    scripts/bench.py run --compare base.json   # digests must match; speedups reported
    scripts/bench.py run --statistics DIR --compare a.json --compare b.json
    scripts/bench.py run --counters --repeat 1 cpu  # host counters per guest cycle (perf)
    scripts/bench.py run --engine reference     # the reference interpreter

The fixture holds copies of the user's private disk and firmware state; keep it out of Git
(the default is on local disk, off the network file system that holds the repository). Each
run works on copies in memory (--scratch, default /dev/shm), so `boot` and `fs` time the
emulator rather than the host disk.

Workloads:
  prom   power-on to the PROM menu, no disk: memory tests, discovery, diagnostics
  boot   the installed disk from power-on to "login:"
  cpu    from a root shell: an awk arithmetic loop (user-mode integer code)
  fs     from a root shell: find and sum over /usr/lib, /usr/bin, /usr/sbin (kernel, XFS,
         SCSI)
  boot2  boot with two CPUs (--cpus 2, SMP.adoc)
  cpu2   from a two-CPU root shell: two cpu loops at once
  mix2   from a two-CPU root shell: the fs pipeline and the cpu loop at once
  reboot2  from a two-CPU root shell: init 6, through both PROMs, to a root shell again

The default set is the one-CPU workloads; name the two-CPU ones to run them. They need the
two-CPU shell snapshot: scripts/bench.py fixture --cpus 2 (after the one-CPU fixture).

Rates count only each workload's own cycles: a snapshot workload starts where the fixture's
login stopped (fixture/shell.json). With two CPUs the rate counts both CPUs' cycles (twice
CPU A's; they differ by at most a slice), and "x real" is virtual time per host second.

--counters runs each workload under `perf stat` (user mode) and reports host instructions,
branches, branch misses, and L1 misses per guest cycle. The counts include process start and,
for the shell workloads, loading the snapshot. Counting needs kernel.perf_event_paranoid <= 2;
with the NMI watchdog off (kernel.nmi_watchdog = 0) the six events are counted without
multiplexing.
"""

import argparse
import hashlib
import json
import os
import re
import resource
import shutil
import statistics
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.dirname(os.environ.get("ULTRAVIOLENT_IP27_PROM", "assets/firmware/ip27prom.img"))
# Host events per run for --counters: six, so a Zen 2 or later core counts them all at once.
COUNTERS = ["instructions:u", "cycles:u", "branches:u", "branch-misses:u",
            "L1-icache-load-misses:u", "L1-dcache-load-misses:u"]
STOPPED = re.compile(rb"stopped after (\d+) cycles at virtual (\d+) ns, pc (0x[0-9a-f]+)")

# A generous cap: every workload ends on its own well before it.
CAP = "200000000000"
LOGIN = ["@expect Option?", "1", "@expect login:"]
SHELL = ["@expect # "]

AWK = "awk 'BEGIN { for (i = 0; i < 1500000; i++) s = (s + i * i) % 1000003; print s }'"
FIND = "find /usr/lib /usr/bin /usr/sbin -type f -print | xargs sum | sum"

WORKLOADS = {
    "prom": {"disk": False, "console": ["@expect Option?", "@stop"]},
    "boot": {"disk": True, "console": LOGIN + ["@stop"]},
    "cpu": {"snapshot": True, "console": [AWK] + SHELL + ["@stop"]},
    "fs": {"snapshot": True, "console": [FIND] + SHELL + ["@stop"]},
    "boot2": {"cpus": 2, "disk": True, "console": LOGIN + ["@stop"]},
    # Both jobs in one sh, so the prompt returns when both are done.
    "cpu2": {"cpus": 2, "snapshot": True,
             "console": [f'sh -c "{AWK} & {AWK} & wait"'] + SHELL + ["@stop"]},
    "mix2": {"cpus": 2, "snapshot": True,
             "console": [f"sh -c '({FIND}) & {AWK.replace(chr(39), chr(34))} & wait'"]
             + SHELL + ["@stop"]},
    # A kernel-requested reboot skips the PROM's diagnostics and its menu (no "Option?").
    "reboot2": {"cpus": 2, "snapshot": True,
                "console": ["init 6", "@expect login:", "root", "@expect TERM", "vt100"]
                + SHELL + ["mpadmin -s"] + SHELL + ["@stop"]},
}
DEFAULT_WORKLOADS = [name for name, w in WORKLOADS.items() if w.get("cpus", 1) == 1]


def shell_prefix(cpus):
    # The root-shell snapshot's files: shell-disk.img, shell.uvstate, ... for one CPU, and
    # shell2-disk.img, shell2.uvstate, ... for two (a snapshot loads only into a machine
    # with the same number of CPUs).
    return "shell" if cpus == 1 else f"shell{cpus}"


def fixture_paths(work, cpus=1):
    fixture = os.path.join(work, "fixture")
    shell = shell_prefix(cpus)
    paths = {name: os.path.join(fixture, name) for name in ("disk.img", "nvram.bin", "flash.bin")}
    for name in ("disk.img", "nvram.bin", "flash.bin"):
        paths["shell-" + name] = os.path.join(fixture, f"{shell}-{name}")
    paths["shell.uvstate"] = os.path.join(fixture, f"{shell}.uvstate")
    paths["shell.json"] = os.path.join(fixture, f"{shell}.json")
    return paths


def copy(source, destination):
    subprocess.run(["cp", "--sparse=always", "--reflink=auto", source, destination], check=True)


def default_scratch():
    # In memory: on a reflink-capable file system (XFS) a copy shares the fixture's blocks
    # but not its page cache, so every run would read the guest disk cold from the host disk
    # (DEVELOPMENT "Benchmarks").
    if os.path.isdir("/dev/shm"):
        return "/dev/shm/ultraviolent-bench-scratch"
    return None


def base_command(args, scratch, cpus=1):
    return [args.binary, "--machine", "ip27", "--prom", args.prom, "--io6prom", args.io6prom,
            "--flash", os.path.join(scratch, "flash.bin"),
            "--nvram", os.path.join(scratch, "nvram.bin"), "--engine", args.engine] + (
                ["--cpus", str(cpus)] if cpus != 1 else []) + (
                ["--cpu-slice", str(args.cpu_slice)] if cpus != 1 and args.cpu_slice else []) + (
                ["--tier0-cache-entries", str(args.cache_entries)] if args.cache_entries else []) + (
                ["--tier1-threshold", str(args.tier1_threshold)]
                if args.tier1_threshold is not None else [])


def make_fixture(args):
    paths = fixture_paths(args.work, args.cpus)
    if args.cpus == 1:
        os.makedirs(os.path.dirname(paths["disk.img"]), exist_ok=True)
        for name in ("disk.img", "nvram.bin", "flash.bin"):
            copy(os.path.join(args.source, name), paths[name])
    elif not os.path.exists(paths["disk.img"]):
        sys.exit("fixture --cpus 2: make the one-CPU fixture first")
    # The shell snapshot: boot a copy of the disk, log in as root, stop at the prompt.
    scratch = os.path.join(args.work, "scratch")
    os.makedirs(scratch, exist_ok=True)
    for name in ("disk.img", "nvram.bin", "flash.bin"):
        copy(paths[name], os.path.join(scratch, name))
    console = LOGIN + ["root", "@expect TERM", "vt100", "@expect # ", "@stop"]
    command = base_command(args, scratch, args.cpus) + ["--disk", os.path.join(scratch, "disk.img"),
                                             "--save-state", paths["shell.uvstate"],
                                             "--cycles", CAP]
    for line in console:
        command += ["--console", line]
    result = subprocess.run(command, capture_output=True)
    if not result.stdout.rstrip().endswith(b"#"):
        sys.stdout.buffer.write(result.stdout[-2000:] + result.stderr[-2000:])
        sys.exit("fixture: no root prompt")
    stopped = STOPPED.search(result.stderr)
    with open(paths["shell.json"], "w") as f:
        json.dump({"cycles": int(stopped.group(1)), "virtual_ns": int(stopped.group(2))}, f)
    # The snapshot carries the flash and timekeeper; the files must agree with it.
    for name in ("disk.img", "nvram.bin", "flash.bin"):
        copy(os.path.join(scratch, name), paths["shell-" + name])
    print(f"fixture in {os.path.dirname(paths['disk.img'])}")


def run_once(args, name, workload):
    cpus = workload.get("cpus", 1)
    paths = fixture_paths(args.work, cpus)
    scratch = args.scratch or os.path.join(args.work, "scratch")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    prefix = "shell-" if workload.get("snapshot") else ""
    for state in ("nvram.bin", "flash.bin"):
        copy(paths[prefix + state], os.path.join(scratch, state))
    command = base_command(args, scratch, cpus)
    if workload.get("snapshot"):
        copy(paths["shell-disk.img"], os.path.join(scratch, "disk.img"))
        command += ["--disk", os.path.join(scratch, "disk.img"),
                    "--load-state", paths["shell.uvstate"]]
    elif workload.get("disk"):
        copy(paths["disk.img"], os.path.join(scratch, "disk.img"))
        command += ["--disk", os.path.join(scratch, "disk.img")]
    command += ["--cycles", workload.get("cycles", CAP)]
    for line in workload["console"]:
        command += ["--console", line]
    if args.statistics:
        command.append("--engine-statistics")
    counters_file = os.path.join(scratch, "counters.csv")
    if args.counters:
        command = ["perf", "stat", "-x,", "-o", counters_file, "-e", ",".join(COUNTERS),
                   "--"] + command
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    result = subprocess.run(command, capture_output=True)
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    stopped = STOPPED.search(result.stderr)
    if result.returncode != 0 or not stopped:
        sys.stderr.buffer.write(result.stderr[-2000:])
        sys.exit(f"{name}: the run failed")
    digest = hashlib.sha256(result.stdout + stopped.group(0)).hexdigest()[:16]
    if args.output:
        os.makedirs(args.output, exist_ok=True)
        with open(os.path.join(args.output, name + ".txt"), "wb") as f:
            f.write(result.stdout)
    if args.statistics:
        os.makedirs(args.statistics, exist_ok=True)
        with open(os.path.join(args.statistics, name + ".txt"), "wb") as f:
            f.write(b"".join(line + b"\n" for line in result.stderr.splitlines()
                             if line.startswith(b"tier")))
    origin = {"cycles": 0, "virtual_ns": 0}
    if workload.get("snapshot"):
        with open(paths["shell.json"]) as f:
            origin = json.load(f)
    result = {"cycles": (int(stopped.group(1)) - origin["cycles"]) * cpus,
              "virtual_ns": int(stopped.group(2)) - origin["virtual_ns"],
              "wall": wall,
              "cpu": after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime,
              "digest": digest}
    if args.counters:
        result["counters"] = read_counters(counters_file)
    return result


def read_counters(path):
    # perf stat -x, lines: value,unit,event,running time,percent running,...
    counters = {}
    with open(path) as f:
        for line in f:
            fields = line.strip().split(",")
            if len(fields) < 5 or not fields[0].isdigit():
                continue
            if float(fields[4]) < 100.0:
                sys.exit(f"perf multiplexed {fields[2]} ({fields[4]}%); see --counters")
            counters[fields[2].removesuffix(":u")] = int(fields[0])
    return counters


def counter_summary(entry):
    # Host events per guest cycle, and misprediction rate.
    counters = entry["counters"]
    cycles = entry["cycles"]
    per = {name: counters[name] / cycles for name in counters}
    return (f"  host per guest cycle: {per['instructions']:.1f} instructions, "
            f"{per['cycles']:.1f} cycles (IPC {counters['instructions'] / counters['cycles']:.2f}), "
            f"{per['branches']:.2f} branches, {per['branch-misses']:.3f} mispredicts "
            f"({100 * counters['branch-misses'] / counters['branches']:.2f}%), "
            f"{per['L1-icache-load-misses']:.3f} L1I and {per['L1-dcache-load-misses']:.3f} "
            f"L1D misses")


def run(args):
    names = args.workloads or DEFAULT_WORKLOADS
    baselines = []
    for path in args.compare or []:
        with open(path) as f:
            baselines.append((os.path.basename(path), json.load(f)["workloads"]))
    results = {}
    failed = False
    print(f"{'workload':8} {'guest cycles':>14} {'virtual s':>9} {'host s':>8} "
          f"{'M cycles/s':>10} {'x real':>6} {'digest':>16}  vs "
          + (", ".join(label for label, _ in baselines) or "baseline"))
    for name in names:
        runs = [run_once(args, name, WORKLOADS[name]) for _ in range(args.repeat)]
        digests = {r["digest"] for r in runs}
        best = min(runs, key=lambda r: r["wall"])
        rate = best["cycles"] / best["wall"] / 1e6
        entry = {"cycles": best["cycles"], "virtual_ns": best["virtual_ns"],
                 "wall": [round(r["wall"], 3) for r in runs],
                 "best_wall": round(best["wall"], 3),
                 "median_wall": round(statistics.median(r["wall"] for r in runs), 3),
                 "mcycles_per_s": round(rate, 2), "digest": best["digest"]}
        if args.counters:
            entry["counters"] = best["counters"]
        results[name] = entry
        note = ""
        if len(digests) > 1:
            note = "NONDETERMINISTIC " + ",".join(sorted(digests))
            failed = True
        for label, baseline in baselines:
            if name not in baseline:
                continue
            old = baseline[name]
            if old["digest"] != best["digest"]:
                note += f" DIGEST CHANGED vs {label} (was {old['digest']})"
                failed = True
            else:
                note += f" {old['best_wall'] / best['wall']:.2f}x"
        real = best["virtual_ns"] / 1e9 / best["wall"]
        print(f"{name:8} {best['cycles']:>14} {best['virtual_ns'] / 1e9:>9.1f} "
              f"{best['wall']:>8.2f} {rate:>10.1f} {real:>6.3f} {best['digest']:>16} {note}",
              flush=True)
        if args.counters:
            print(counter_summary(entry), flush=True)
    if args.save:
        host = {"binary": args.binary, "engine": args.engine, "cpu_slice": args.cpu_slice,
                "cache_entries": args.cache_entries, "tier1_threshold": args.tier1_threshold,
                "cpu": cpu_model(),
                "kernel": os.uname().release,
                "repeat": args.repeat, "date": time.strftime("%Y-%m-%d %H:%M")}
        with open(args.save, "w") as f:
            json.dump({"host": host, "workloads": results}, f, indent=2)
            f.write("\n")
    if args.scratch:
        shutil.rmtree(args.scratch, ignore_errors=True)  # gives the memory back
    return 1 if failed else 0


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--work", default="/var/tmp/ultraviolent-bench",
                        help="fixture directory (local disk)")
    common.add_argument("--scratch", default=default_scratch(),
                        help="each run's copies (default in memory, /dev/shm)")
    common.add_argument("--binary", default="build/pgo/ultraviolent")
    common.add_argument("--prom", default=os.path.join(FIRMWARE, "ip27prom.img"))
    common.add_argument("--io6prom", default=os.path.join(FIRMWARE, "io6prom.img"))
    common.add_argument("--engine", default="tier1", choices=["reference", "tier0", "tier1"])
    common.add_argument("--cache-entries", type=int, help="tier 0's block cache size")
    common.add_argument("--cpu-slice", type=int,
                        help="two-CPU workloads: cycles a CPU may run ahead (default 180)")
    common.add_argument("--tier1-threshold", type=int,
                        help="cache hits before tier 1 translates a block")
    commands = parser.add_subparsers(dest="command", required=True)
    fixture = commands.add_parser("fixture", parents=[common],
                                  help="build the fixture from an installed system")
    fixture.add_argument("--from", dest="source", default="local/rc",
                         help="directory with disk.img, nvram.bin, flash.bin after a clean shutdown")
    fixture.add_argument("--cpus", type=int, choices=[1, 2], default=1,
                         help="2: only the two-CPU shell snapshot, from the existing fixture")
    bench = commands.add_parser("run", parents=[common], help="run workloads")
    bench.add_argument("workloads", nargs="*", help=", ".join(WORKLOADS))
    bench.add_argument("--repeat", type=int, default=3)
    bench.add_argument("--save")
    bench.add_argument("--compare", action="append",
                       help="a saved result to compare with (repeatable)")
    bench.add_argument("--counters", action="store_true",
                       help="count host events per guest cycle with perf stat")
    bench.add_argument("--output", metavar="DIR",
                       help="keep each workload's console output in DIR/WORKLOAD.txt")
    bench.add_argument("--statistics", metavar="DIR",
                       help="write each workload's tier-0 statistics to DIR/WORKLOAD.txt")
    args = parser.parse_args()
    os.chdir(REPO)
    unknown = set(getattr(args, "workloads", None) or []) - set(WORKLOADS)
    if unknown:
        parser.error(f"unknown workloads: {', '.join(sorted(unknown))}")
    if args.command == "fixture":
        make_fixture(args)
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
