#!/usr/bin/env python3
"""Two machines on one virtual switch, driven through uvctl (doc/ORCHESTRATION.adoc "O4").

From an installed machine's directory (disk.img, nvram.bin, flash.bin; for example the one
scripts/rc-ip27-demo.py leaves), it:

  1. creates alpha on a copy-on-write overlay of that disk and clones beta from alpha;
  2. starts both on switch "lan" (uvctl up), logs in on each console, and gives them
     addresses 10.77.0.1 and 10.77.0.2;
  3. pings alpha from beta, and logs in to alpha from beta with telnet, reading a file that
     only alpha has;
  4. suspends beta, snapshots it as "net", resumes it, and pings alpha again from the same
     shell (the resumed machine carries on);
  5. turns both off (IRIX shuts down), restores beta's "net" snapshot, resumes it, and pings
     alpha's address again: alpha is off, so nothing answers, but the resumed shell is the
     one from the snapshot;
  6. stops everything.

Everything goes under --work (removed first if --fresh). The installed disk is only read.

  scripts/fleet-demo.py --from local/release-check-013 --work local/fleet-demo --fresh
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
UVCTL = [sys.executable, os.path.join(HERE, "uvctl.py")]

FLEET = """\
[fleet]
dir = "fleet"
{binary}
[switch.lan]

[machine.alpha]
ethernet = "lan"

[machine.beta]
ethernet = "lan"
"""


def note(text):
    print(time.strftime("%H:%M:%S"), text, flush=True)


class Console:
    """A uvctl machine's console socket, read and typed to like a terminal."""

    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(path)
        self.socket.settimeout(1)
        self.seen = ""

    def wait_for(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while True:
            match = re.search(pattern, self.seen)
            if match:
                self.seen = self.seen[match.end():]
                return match
            if time.monotonic() > deadline:
                raise TimeoutError(f"waiting for {pattern!r}; last output: {self.seen[-400:]!r}")
            try:
                data = self.socket.recv(65536)
            except socket.timeout:
                continue
            if not data:
                raise EOFError("console closed")
            text = data.decode("latin-1")
            sys.stdout.write(text)
            sys.stdout.flush()
            self.seen += text

    def send(self, line):
        time.sleep(0.3)
        self.socket.sendall(line.encode() + b"\r")

    def command(self, line, timeout=120):
        """Types a shell command and returns its output up to the next prompt."""
        self.send(line)
        match = self.wait_for(r"([\s\S]*?)\n[^\n]*# $", timeout)
        return match.group(1)

    def close(self):
        self.socket.close()


def uvctl(args, *words):
    command = UVCTL + ["-f", os.path.join(args.work, "fleet.toml")] + list(words)
    note("$ uvctl " + " ".join(words))
    result = subprocess.run(command, capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    return result.stdout


def console(args, name):
    return Console(os.path.join(args.work, "fleet", name, "console.sock"))


def log_in(guest):
    """From power-on to a root shell."""
    while True:
        match = guest.wait_for(r"(Option\? $|login: $)", 1800)
        if match.group(0).startswith("Option"):
            guest.send("1")
            continue
        break
    guest.send("root")
    match = guest.wait_for(r"(TERM = \([^)]*\) $|# $)", 600)
    if match.group(0).startswith("TERM"):
        guest.send("vt100")
        guest.wait_for(r"# $", 600)


def check(results, name, condition):
    results.append((name, bool(condition)))
    note(("PASS " if condition else "FAIL ") + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="source", required=True)
    parser.add_argument("--work", default="local/fleet-demo")
    parser.add_argument("--binary")
    parser.add_argument("--fresh", action="store_true")
    args = parser.parse_args()
    args.work = os.path.abspath(args.work)
    if args.fresh and os.path.exists(args.work):
        shutil.rmtree(args.work)
    os.makedirs(args.work)
    binary = f'binary = "{os.path.abspath(args.binary)}"\n' if args.binary else ""
    with open(os.path.join(args.work, "fleet.toml"), "w") as f:
        f.write(FLEET.format(binary=binary))
    results = []
    start = time.monotonic()
    try:
        note("1. create and clone")
        uvctl(args, "create", "alpha", "--from", os.path.abspath(args.source))
        uvctl(args, "clone", "alpha", "beta")

        note("2. both up, addresses")
        uvctl(args, "up")
        alpha = console(args, "alpha")
        beta = console(args, "beta")
        log_in(alpha)
        log_in(beta)
        alpha.command("ifconfig ef0 inet 10.77.0.1 netmask 255.255.255.0 up")
        alpha.command("echo only-on-alpha > /usr/tmp/fleet-demo-alpha")
        beta.command("ifconfig ef0 inet 10.77.0.2 netmask 255.255.255.0 up")
        check(results, "two different Ethernet addresses",
              len(set(re.findall(r"08:00:69:[0-9a-f:]{8}", uvctl(args, "status")))) == 2)

        note("3. ping and telnet")
        output = beta.command("ping -c 3 10.77.0.1")
        check(results, "beta pings alpha", "3 packets received" in output)
        beta.send("telnet 10.77.0.1")
        beta.wait_for(r"login: $", 120)
        beta.send("root")
        match = beta.wait_for(r"(TERM = \([^)]*\) $|# $)", 120)
        if match.group(0).startswith("TERM"):
            beta.send("vt100")
            beta.wait_for(r"# $", 120)
        output = beta.command("cat /usr/tmp/fleet-demo-alpha")
        check(results, "beta logs in to alpha with telnet", "only-on-alpha" in output)
        beta.command("exit")
        output = beta.command("ls /usr/tmp/fleet-demo-alpha")
        check(results, "the file is alpha's alone", "No such file" in output or "not found" in output)

        note("4. suspend, snapshot, resume")
        beta.command("echo before-suspend > /usr/tmp/fleet-demo-beta; sync")
        beta.close()
        uvctl(args, "suspend", "beta")
        check(results, "beta suspended", "suspended" in uvctl(args, "status"))
        uvctl(args, "snapshot", "beta", "net")
        uvctl(args, "resume", "beta")
        beta = console(args, "beta")
        # The resumed shell is still at its prompt: a new command runs at once.
        output = beta.command("ping -c 3 10.77.0.1")
        check(results, "the resumed beta pings alpha", "3 packets received" in output)
        beta.command("echo after-resume > /usr/tmp/fleet-demo-beta; sync")

        note("5. off, restore, resume the snapshot")
        alpha.close()
        beta.close()
        uvctl(args, "down", "alpha", "beta")
        check(results, "both off", uvctl(args, "status").count(" off ") >= 2)
        uvctl(args, "restore", "beta", "net")
        uvctl(args, "up", "beta")
        beta = console(args, "beta")
        output = beta.command("cat /usr/tmp/fleet-demo-beta")
        check(results, "the restored beta is the snapshot's", "before-suspend" in output)
        output = beta.command("ping -c 2 10.77.0.1", 120)
        check(results, "alpha is off: no answer", "0 packets received" in output)
        beta.close()
    except Exception as error:  # noqa: BLE001 - the demonstration reports any failure
        note(f"error: {error}")
        results.append(("ran to the end", False))
    finally:
        note("6. stop everything")
        try:
            uvctl(args, "down", "--force")
        except RuntimeError as error:
            note(f"error: {error}")
    elapsed = (time.monotonic() - start) / 60
    with open(os.path.join(args.work, "results.txt"), "w") as f:
        for name, passed in results:
            f.write(f"{'PASS' if passed else 'FAIL'} {name}\n")
        f.write(f"{elapsed:.1f} min\n")
    failed = [name for name, passed in results if not passed]
    note(f"{len(results) - len(failed)} of {len(results)} checks passed in {elapsed:.1f} min")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
