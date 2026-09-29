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

The fixture holds copies of the user's private disk and firmware state; keep it out of Git
(the default is on local disk, off the network file system that holds the repository).

Workloads:
  prom   power-on to the PROM menu, no disk: memory tests, discovery, diagnostics
  boot   the installed disk from power-on to "login:"
  cpu    from a root shell: an awk arithmetic loop (user-mode integer code)
  fs     from a root shell: find and sum over /usr/lib, /usr/bin, /usr/sbin (kernel, XFS,
         SCSI)

Rates count only each workload's own cycles: a snapshot workload starts where the fixture's
login stopped (fixture/shell.json).
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
STOPPED = re.compile(rb"stopped after (\d+) cycles at virtual (\d+) ns, pc (0x[0-9a-f]+)")

# A generous cap: every workload ends on its own well before it.
CAP = "200000000000"
LOGIN = ["@expect Option?", "1", "@expect login:"]
SHELL = ["@expect # "]

WORKLOADS = {
    "prom": {"disk": False, "console": ["@expect Option?", "@stop"]},
    "boot": {"disk": True, "console": LOGIN + ["@stop"]},
    "cpu": {"snapshot": True, "console": [
        "awk 'BEGIN { for (i = 0; i < 1500000; i++) s = (s + i * i) % 1000003; print s }'"]
        + SHELL + ["@stop"]},
    "fs": {"snapshot": True, "console": [
        "find /usr/lib /usr/bin /usr/sbin -type f -print | xargs sum | sum"] + SHELL + ["@stop"]},
}


def fixture_paths(work):
    fixture = os.path.join(work, "fixture")
    return {name: os.path.join(fixture, name) for name in
            ("disk.img", "nvram.bin", "flash.bin", "shell.uvstate", "shell-disk.img",
             "shell.json")}


def copy(source, destination):
    subprocess.run(["cp", "--sparse=always", "--reflink=auto", source, destination], check=True)


def base_command(args, scratch):
    return [args.binary, "--machine", "ip27", "--prom", args.prom, "--io6prom", args.io6prom,
            "--flash", os.path.join(scratch, "flash.bin"),
            "--nvram", os.path.join(scratch, "nvram.bin")]


def make_fixture(args):
    paths = fixture_paths(args.work)
    os.makedirs(os.path.dirname(paths["disk.img"]), exist_ok=True)
    for name in ("disk.img", "nvram.bin", "flash.bin"):
        copy(os.path.join(args.source, name), paths[name])
    # The shell snapshot: boot a copy of the disk, log in as root, stop at the prompt.
    scratch = os.path.join(args.work, "scratch")
    os.makedirs(scratch, exist_ok=True)
    for name in ("disk.img", "nvram.bin", "flash.bin"):
        copy(paths[name], os.path.join(scratch, name))
    console = LOGIN + ["root", "@expect TERM", "vt100", "@expect # ", "@stop"]
    command = base_command(args, scratch) + ["--disk", os.path.join(scratch, "disk.img"),
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
    copy(os.path.join(scratch, "disk.img"), paths["shell-disk.img"])
    # The snapshot carries the flash and timekeeper; the files must agree with it.
    for name in ("nvram.bin", "flash.bin"):
        copy(os.path.join(scratch, name), os.path.join(os.path.dirname(paths["disk.img"]),
                                                       "shell-" + name))
    print(f"fixture in {os.path.dirname(paths['disk.img'])}")


def run_once(args, name, workload):
    paths = fixture_paths(args.work)
    fixture = os.path.dirname(paths["disk.img"])
    scratch = os.path.join(args.work, "scratch")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    prefix = "shell-" if workload.get("snapshot") else ""
    for state in ("nvram.bin", "flash.bin"):
        copy(os.path.join(fixture, prefix + state), os.path.join(scratch, state))
    command = base_command(args, scratch)
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
    origin = {"cycles": 0, "virtual_ns": 0}
    if workload.get("snapshot"):
        with open(paths["shell.json"]) as f:
            origin = json.load(f)
    return {"cycles": int(stopped.group(1)) - origin["cycles"],
            "virtual_ns": int(stopped.group(2)) - origin["virtual_ns"],
            "wall": wall, "cpu": after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime,
            "digest": digest}


def run(args):
    names = args.workloads or list(WORKLOADS)
    baseline = {}
    if args.compare:
        with open(args.compare) as f:
            baseline = json.load(f)["workloads"]
    results = {}
    failed = False
    print(f"{'workload':8} {'guest cycles':>14} {'virtual s':>9} {'host s':>8} "
          f"{'M cycles/s':>10} {'x real':>6} {'digest':>16}  vs baseline")
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
        results[name] = entry
        note = ""
        if len(digests) > 1:
            note = "NONDETERMINISTIC " + ",".join(sorted(digests))
            failed = True
        if name in baseline:
            old = baseline[name]
            if old["digest"] != best["digest"]:
                note += f" DIGEST CHANGED (was {old['digest']})"
                failed = True
            else:
                note += f" {old['best_wall'] / best['wall']:.2f}x"
        real = best["virtual_ns"] / 1e9 / best["wall"]
        print(f"{name:8} {best['cycles']:>14} {best['virtual_ns'] / 1e9:>9.1f} "
              f"{best['wall']:>8.2f} {rate:>10.1f} {real:>6.3f} {best['digest']:>16} {note}",
              flush=True)
    if args.save:
        host = {"binary": args.binary, "cpu": cpu_model(), "kernel": os.uname().release,
                "repeat": args.repeat, "date": time.strftime("%Y-%m-%d %H:%M")}
        with open(args.save, "w") as f:
            json.dump({"host": host, "workloads": results}, f, indent=2)
            f.write("\n")
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
                        help="fixture and scratch directory (local disk)")
    common.add_argument("--binary", default="build/pgo/ultraviolent")
    common.add_argument("--prom", default=os.path.join(FIRMWARE, "ip27prom.img"))
    common.add_argument("--io6prom", default=os.path.join(FIRMWARE, "io6prom.img"))
    commands = parser.add_subparsers(dest="command", required=True)
    fixture = commands.add_parser("fixture", parents=[common],
                                  help="build the fixture from an installed system")
    fixture.add_argument("--from", dest="source", default="local/rc",
                         help="directory with disk.img, nvram.bin, flash.bin after a clean shutdown")
    bench = commands.add_parser("run", parents=[common], help="run workloads")
    bench.add_argument("workloads", nargs="*", help=", ".join(WORKLOADS))
    bench.add_argument("--repeat", type=int, default=3)
    bench.add_argument("--save")
    bench.add_argument("--compare")
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
