#!/usr/bin/env python3
"""A soak: an installed IRIX under steady load for hours of guest time (ROADMAP "Usable VM").

From a copy of an installed system (--from: a directory with disk.img, nvram.bin, and
flash.bin, such as rc-ip27-demo.py's --work), it boots to a root shell and starts a loop in
the guest. Each pass computes with awk, writes, sorts, copies, archives, and compares files in
/usr/tmp, reads /usr/bin through the disk, syncs, and prints one line:

  soak PASS AWK SORTED ARCHIVE BIN

The values after the pass number must be the same on every pass. The run is cut into
segments of at most --segment-minutes of host time (each saved with --save-state and resumed
with --load-state, so no host process runs long); with --tap, each segment also pings the
guest and logs in over telnet. After the last segment the loop is interrupted, IRIX is shut
down cleanly and cold-booted again; the second boot must reach a shell, and no program but
ESP's configmon may have dumped core (USING.adoc "Known limits"). Everything is
written under --work: the copies, a console log per segment, and results.txt.

  scripts/soak.py --from local/vm --work local/soak --guest-hours 4 [--cpus 2] [--tap uv0]
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("demo", os.path.join(HERE, "rc-ip27-demo.py"))
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)

# The guest's loop, typed into `cat`. Every value it prints depends only on the files it makes
# and the installed /usr/bin, so every pass prints the same line.
LOOP = r"""i=0
while true; do
i=`expr $i + 1`
a=`awk 'BEGIN { for (n = 0; n < 200000; n++) s = (s + n * n) % 1000003; print s }'`
rm -rf /usr/tmp/soak; mkdir -p /usr/tmp/soak/x; cd /usr/tmp/soak
awk 'BEGIN { for (n = 0; n < 20000; n++) print n, (n * 7919) % 9973 }' > f1
sort -n +1 -2 f1 > f2; cp f2 f3; b=`sum f3 | awk '{print $1}'`
tar cf t.tar f1 f2 f3; cd x; tar xf ../t.tar
c=`cmp f1 ../f1 && cmp f3 ../f2 && sum f2 | awk '{print $1}'`
d=`find /usr/bin -type f -print | sort | xargs sum | sum | awk '{print $1}'`
cd /; sync
echo soak $i $a $b $c $d
done
"""
LINE = re.compile(r"^soak (\d+) (\S+) (\S+) (\S+) (\S+)$", re.M)
TROUBLE = re.compile(r"PANIC|panic|Exception|core dumped|No space left|I/O error|"
                     r"Segmentation fault|Bus error")


def command(args, extra):
    cmd = [args.binary, "--machine", "ip27", "--prom", args.prom,
           "--flash", os.path.join(args.work, "flash.bin"),
           "--nvram", os.path.join(args.work, "nvram.bin"),
           "--disk", os.path.join(args.work, "disk.img"),
           "--interactive", "--cycles", "100000000000000"]
    if args.io6prom:
        cmd += ["--io6prom", args.io6prom]
    if args.cpus != 1:
        cmd += ["--cpus", str(args.cpus), "--parallel"]
    if args.tap:
        cmd += ["--ethernet", "tap:" + args.tap]
    return cmd + extra


def network_check(args, results, segment):
    # A resumed run answers nothing until its state has loaded; wait for that first.
    for _ in range(60):
        if subprocess.run(["ping", "-c", "1", "-W", "1", args.guest_ip],
                          capture_output=True).returncode == 0:
            break
    ping = subprocess.run(["ping", "-c", "20", "-i", "0.2", "-W", "2", args.guest_ip],
                          capture_output=True, text=True)
    received = re.search(r"(\d+) received", ping.stdout)
    session = demo.telnet_session(args.guest_ip, ["uname -a", "uptime"])
    ok = received is not None and int(received.group(1)) > 0 and "IRIX" in session
    results.write(f"segment {segment}: ping {received.group(1) if received else 0}/20, "
                  f"telnet {'ok' if 'IRIX' in session else 'failed'}\n")
    results.flush()
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="source", required=True,
                        help="directory with an installed disk.img, nvram.bin, flash.bin")
    parser.add_argument("--work", required=True)
    parser.add_argument("--guest-hours", type=float, default=4)
    parser.add_argument("--segment-minutes", type=float, default=45)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--binary", default="build/pgo/ultraviolent")
    parser.add_argument("--prom", default=os.environ.get("ULTRAVIOLENT_IP27_PROM",
                                                         "assets/firmware/ip27prom.img"))
    parser.add_argument("--io6prom", default=os.path.join(
        os.path.dirname(os.environ.get("ULTRAVIOLENT_IP27_PROM", "assets/firmware/ip27prom.img")),
        "io6prom.img"))
    parser.add_argument("--tap", help="TAP interface: ping and telnet each segment")
    parser.add_argument("--guest-ip", default="192.168.77.2")
    args = parser.parse_args()

    os.makedirs(args.work, exist_ok=True)
    for name in ("disk.img", "nvram.bin", "flash.bin"):
        subprocess.run(["cp", "--sparse=always", os.path.join(args.source, name),
                        os.path.join(args.work, name)], check=True)
    demo.LOG = open(os.path.join(args.work, "driver.log"), "a")
    results = open(os.path.join(args.work, "results.txt"), "a")
    state = os.path.join(args.work, "soak.uvstate")
    stopped = re.compile(r"stopped after (\d+) cycles at virtual (\d+) ns")
    failures = []
    baseline = None
    passes = 0
    virtual_ns = 0
    segment = 0
    start = time.time()

    while virtual_ns < args.guest_hours * 3600e9:
        segment += 1
        log = os.path.join(args.work, f"segment-{segment}.log")
        resume = ["--load-state", state] if segment > 1 else []
        guest = demo.Guest(command(args, resume + ["--save-state", state]), log)
        if segment == 1:
            demo.boot_to_shell(guest)
            if args.tap:
                guest.send(f"ifconfig ef0 inet {args.guest_ip} netmask 255.255.255.0 up")
                guest.wait_for(r"# $", 600)
            guest.send("cat > /usr/tmp/soak.sh")
            for line in LOOP.splitlines():
                guest.proc.stdin.write(line.encode() + b"\r")
            guest.proc.stdin.write(b"\x04")
            guest.proc.stdin.flush()
            guest.wait_for(r"# $", 600)
            guest.send("sh /usr/tmp/soak.sh")
        if args.tap and not network_check(args, results, segment):
            failures.append(f"segment {segment}: network check failed")
        began = time.time()
        while time.time() - began < args.segment_minutes * 60 and guest.alive():
            time.sleep(10)
            text = guest.since_mark()
            if TROUBLE.search(text):
                failures.append(f"segment {segment}: {TROUBLE.search(text).group(0)!r} on the console")
                break
        guest.finish()
        with open(log + ".err", errors="replace") as f:
            match = stopped.search(f.read())
        if not match:
            failures.append(f"segment {segment}: the emulator did not stop cleanly")
            break
        virtual_ns = int(match.group(2))
        for line in LINE.finditer(guest.since_mark()):
            passes = max(passes, int(line.group(1)))
            values = line.group(2, 3, 4, 5)
            baseline = baseline or values
            if values != baseline:
                failures.append(f"pass {line.group(1)}: {values} differs from {baseline}")
        note = (f"segment {segment}: {(time.time() - began) / 60:.1f} min host, guest at "
                f"{virtual_ns / 3600e9:.2f} h, {passes} passes")
        demo.note(note)
        results.write(note + "\n")
        results.flush()
        if failures:
            break

    if passes == 0 and not failures:
        failures.append("the loop printed no passes")
    if not failures:
        # Interrupt the loop, shut down cleanly, and cold-boot once more.
        guest = demo.Guest(command(args, ["--load-state", state]),
                           os.path.join(args.work, "shutdown.log"))
        guest.send("\x03")
        guest.wait_for(r"# $", 600)
        guest.send("uptime; df -k /")
        guest.wait_for(r"# $", 600)
        results.write(guest.since_mark() + "\n")
        guest.send("shutdown -y -g0 -i0")
        guest.wait_for(r"IP27 PROM", 3600, quiet=0)
        guest.finish()
        guest = demo.Guest(command(args, []), os.path.join(args.work, "reboot.log"))
        demo.boot_to_shell(guest)
        # Core dumps since the first boot: ESP's configmon leaves one on every boot after the
        # first (USING.adoc "Known limits"); any other program's fails the soak.
        guest.send("sleep 120; file /core /usr/tmp/core.postinst_detected")
        guest.wait_for(r"# $", 600)
        for name in sorted(set(re.findall(r"core dump of '([^']+)'", guest.since_mark()))):
            results.write(f"core dump of {name}\n")
            if name != "configmon":
                failures.append(f"{name} dumped core")
        guest.send("shutdown -y -g0 -i0")
        guest.wait_for(r"IP27 PROM", 3600, quiet=0)
        guest.finish()

    summary = (f"soak: {passes} passes, {virtual_ns / 3600e9:.2f} guest hours, "
               f"{(time.time() - start) / 3600:.2f} host hours, {args.cpus} CPU(s): "
               + ("passed" if not failures else "FAILED: " + "; ".join(failures)))
    demo.note(summary)
    results.write(summary + "\n")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
