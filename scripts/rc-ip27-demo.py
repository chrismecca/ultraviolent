#!/usr/bin/env python3
"""The RC-IP27 canonical demonstration (doc/RC-IP27.adoc), driven end to end.

From new, blank persistent state (flash, NVRAM, and disk files created here), it runs:

  A. first power-on: the PROM log is initialized from POD (go cac, initlog), as on a machine
     with a fresh log;
  B. power-on with the Installation Tools CD: the Command Monitor boots fx.64 from the CD to
     label the disk, "Install System Software" loads the miniroot, IRIX makes the root file
     system, inst reads the five CDs, declines the overlay conflicts whose base products are
     not on them, installs, and restarts;
  C. cold boot of the installed disk: multiuser, a console login, ef0 configured on the TAP
     link, a telnet login from this host that runs the contract's commands, and a clean
     shutdown;
  D. cold start again, checking that a file written in C survived.

The guest is driven as an operator would drive it: each answer waits for the guest's prompt.
Console input goes through --interactive (standard input), disc changes through the monitor
command "cdrom FILE" (Ctrl-]). Nothing is patched or bypassed.

  scripts/rc-ip27-demo.py --work local/rc --media assets/media --tap uv0

The TAP interface must exist and belong to the user (see doc/STATUS.adoc); its host address
is the telnet client's peer. Everything is written under --work: the persistent state, a
console log per phase, and results.txt.
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import threading
import time

ESCAPE = b"\x1d"

MEDIA = {
    "tools": "IRIX 6.5.30 Installation Tools and Overlays (1 of 3).iso",
    "overlays2": "IRIX 6.5.30 Overlays (2 of 3).iso",
    "overlays3": "IRIX 6.5.30 Overlays (3 of 3).iso",
    "foundation1": "IRIX-6.5-Foundation1.iso",
    "foundation2": "IRIX-6.5-Foundation2.iso",
}


class Guest:
    """One emulator run: console output collected from standard output, input written to
    standard input."""

    def __init__(self, command, log_path):
        self.log = open(log_path, "wb")
        self.err = open(log_path + ".err", "wb")
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err)
        self.text = ""
        self.mark = 0
        self.last_output = time.time()
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            data = self.proc.stdout.read1(4096)
            if not data:
                return
            self.log.write(data)
            self.log.flush()
            with self.lock:
                self.text += data.decode("latin-1").replace("\r", "")
                self.last_output = time.time()

    def since_mark(self):
        with self.lock:
            return self.text[self.mark:]

    def tail(self, n=3000):
        with self.lock:
            return self.text[-n:]

    def alive(self):
        return self.proc.poll() is None

    def send(self, line):
        """Types a line (and Enter)."""
        with self.lock:
            self.mark = len(self.text)
        self.proc.stdin.write(line.encode() + b"\r")
        self.proc.stdin.flush()
        note(f"    > {line!r}")

    def monitor(self, command):
        """An Ultraviolent monitor command (Ctrl-], the command, Enter)."""
        self.proc.stdin.write(ESCAPE + command.encode() + b"\r")
        self.proc.stdin.flush()
        note(f"    [monitor] {command}")

    def wait_for(self, pattern, timeout, quiet=2.0):
        """Waits until `pattern` matches the output since the last input and the guest has
        printed nothing for `quiet` seconds; returns the match."""
        deadline = time.time() + timeout
        regex = re.compile(pattern)
        while time.time() < deadline:
            if not self.alive():
                raise RuntimeError(f"emulator exited while waiting for {pattern!r}")
            match = regex.search(self.since_mark())
            if match and time.time() - self.last_output >= quiet:
                return match
            time.sleep(0.5)
        raise RuntimeError(f"timed out waiting for {pattern!r}; tail:\n{self.tail(800)}")

    def finish(self, timeout=600):
        """Ends the run as an operator would (monitor q) and waits for the files to be written."""
        if self.alive():
            self.monitor("q")
        self.proc.stdin.close()
        self.proc.wait(timeout=timeout)
        self.log.close()
        self.err.close()


LOG = None


def note(text):
    line = time.strftime("%H:%M:%S ") + text
    print(line, flush=True)
    if LOG:
        LOG.write(line + "\n")
        LOG.flush()


def emulator(args, extra, cdrom=None, network=False):
    command = [args.binary, "--machine", "ip27", "--prom", args.prom,
               "--flash", os.path.join(args.work, "flash.bin"),
               "--nvram", os.path.join(args.work, "nvram.bin"),
               "--disk", os.path.join(args.work, "disk.img"),
               "--interactive", "--cycles", "100000000000000"]
    if args.io6prom:
        command += ["--io6prom", args.io6prom]
    if cdrom:
        command += ["--cdrom", cdrom]
    if network and args.tap:
        command += ["--ethernet", "tap:" + args.tap]
    return command + extra


def phase_a(args):
    note("A. first power-on: initialize the PROM log")
    guest = Guest(emulator(args, []), os.path.join(args.work, "a-initlog.log"))
    guest.wait_for(r"Dex> $", 3600)
    guest.send("go cac")
    guest.wait_for(r"> $", 600)
    guest.send("initlog")
    match = guest.wait_for(r"(\? .*$|> $)", 600)
    if match.group(0).startswith("?"):
        guest.send("y")
        guest.wait_for(r"> $", 600)
    guest.finish()


def disc(args, name):
    return os.path.join(args.media, MEDIA[name])


def disc_for_request(args, request):
    if "FOUNDATION-1" in request or "Foundation 1" in request:
        return disc(args, "foundation1")
    if "FOUNDATION-2" in request or "Foundation 2" in request:
        return disc(args, "foundation2")
    match = re.search(r"(\d)[- ]of[- ]3", request)
    if match:
        return disc(args, {"1": "tools", "2": "overlays2", "3": "overlays3"}[match.group(1)])
    if "Installation Tools" in request:
        return disc(args, "tools")
    return None


def phase_b(args):
    note("B. install from the CDs")
    guest = Guest(emulator(args, [], cdrom=disc(args, "tools")),
                  os.path.join(args.work, "b-install.log"))
    state = {"labeled": False, "repartitioned": False, "fx_started": False,
             "install_started": False, "went": False, "installing": False,
             "discs": ["overlays2", "overlays3", "foundation1", "foundation2"],
             "swapped_at": -1, "restarted": False, "inst_mark": 0, "last": ""}
    # Operator answers, keyed on the prompt the guest is waiting at.
    deadline = time.time() + args.install_hours * 3600
    while time.time() < deadline:
        if state["restarted"]:
            break
        if not guest.alive():
            raise RuntimeError("emulator exited during the installation")
        tail = guest.tail()
        idle = time.time() - guest.last_output
        # Disc requests during the copy: the drive is reloaded, no input.
        # (inst wraps long requests, so the period may start the next line.)
        request = re.search(r'Please insert the "([^"]+)" CD\s*\.\s*\nType control-C to interrupt\.\s*$',
                            tail)
        if request and idle > 2 and len(guest.text) != state["swapped_at"]:
            path = disc_for_request(args, request.group(1))
            if not path:
                raise RuntimeError("unknown disc request: " + request.group(1))
            state["swapped_at"] = len(guest.text)
            guest.monitor("cdrom " + path)
            continue
        if idle < 2.0:
            time.sleep(0.5)
            continue
        since = guest.since_mark()
        if re.search(r"Option\? $", tail):
            guest.send("5" if not state["labeled"] else "2")
        elif re.search(r">> $", tail):
            if not state["labeled"]:
                state["fx_started"] = True
                guest.send("boot -f dksc(0,6,8)sash64 dksc(0,6,7)stand/fx.64 --x")
            else:
                guest.send("exit")
        elif state["fx_started"] and not state["labeled"] and re.search(r"= \([^)]*\) $", tail):
            guest.send("")
        elif re.search(r"fx> $", tail):
            if not state["repartitioned"]:
                guest.send("r")
            else:
                state["labeled"] = True
                guest.send("exit")
        elif re.search(r"fx/repartition> $", tail):
            guest.send("ro" if not state["repartitioned"] else "..")
            state["repartitioned"] = True
        elif re.search(r"Continue\? $", tail):
            guest.send("y")
        elif re.search(r"or <enter> to start: $", tail) or re.search(r"press <enter>: $", tail):
            state["install_started"] = True
            guest.send("")
        elif re.search(r"Make new file system on /dev/dsk/realroot.*: $", tail):
            guest.send("yes")
        elif re.search(r"Are you sure\? \[y/n\] \(n\): $", tail):
            guest.send("y")
        elif re.search(r"512 or 4096 bytes\? $", tail):
            guest.send("4096")
        elif re.search(r"more\? \(h=help\) $", tail):
            guest.send("n" if "Do not install" in tail else "q")
        elif re.search(r"Please enter a choice \[1\]: $", tail):
            guest.send("")
        elif re.search(r"Install software from: \[[^]]*\] $", tail):
            if state["discs"]:
                guest.monitor("cdrom " + disc(args, state["discs"].pop(0)))
                guest.send("")
            else:
                guest.send("done")
        elif re.search(r"Interrupt> $", tail):
            # A failed file (the user's Overlays 2 image has a bad libmp.so entry): go on.
            note("    inst reported an error; continuing: " +
                 " | ".join(l for l in since.splitlines() if "ERROR" in l or "checksum" in l)[:300])
            guest.send("continue")
        elif re.search(r"Inst> $", tail):
            # Everything inst printed since the last command given at this prompt (pager
            # answers in between do not count).
            segment = guest.text[state["inst_mark"]:]
            choices = sorted({int(n) for n in re.findall(r"(\d+)a\. Do not install", segment)})
            if state["installing"] and ("successful" in segment or "Errors occurred" in segment):
                command = "quit"
            elif not state["went"]:
                state["went"] = True
                command = "go"
            elif choices:
                # Overlay products whose base products are on CDs the user does not have.
                state["installing"] = False
                command = "conflicts " + " ".join(f"{n}a" for n in choices[:10])
                state["last"] = "resolve"
            elif "No conflicts" in segment or state["last"] == "list":
                state["installing"] = True
                command = "go"
                state["last"] = "go"
            else:
                command = "conflicts"
                state["last"] = "list"
            guest.send(command)
            state["inst_mark"] = len(guest.text)
        elif re.search(r"Restart\? \{ \(y\)es, \(n\)o, \(sh\)ell, \(h\)elp \}: $", tail):
            guest.send("y")
            guest.wait_for(r"IP27 PROM", 3600, quiet=0)
            state["restarted"] = True
        else:
            # A prompt with no answer here: say so once, after a while, for diagnosis.
            if idle > 600 and state.get("reported") != len(guest.text) and \
                    re.search(r"[:?>)\]] $", tail):
                state["reported"] = len(guest.text)
                note("    waiting at an unrecognized prompt: " + tail[-160:].replace("\n", " | "))
            time.sleep(1.0)
            continue
        time.sleep(1.0)
    guest.finish()
    if not state["restarted"]:
        raise RuntimeError("the installation did not finish in time")


def boot_to_shell(guest, timeout=7200):
    while True:
        match = guest.wait_for(r"(Option\? $|login: $)", timeout)
        if match.group(0).startswith("Option"):
            guest.send("1")
            continue
        break
    guest.send("root")
    match = guest.wait_for(r"(TERM = \([^)]*\) $|# $)", 600)
    if match.group(0).startswith("TERM"):
        guest.send("vt100")
        guest.wait_for(r"# $", 600)


def telnet_session(host, commands, timeout=60):
    """A minimal telnet client (refuses every option): logs in as root and runs `commands`."""
    iac, dont, do, wont, will, sb, se = 255, 254, 253, 252, 251, 250, 240
    sock = socket.create_connection((host, 23), timeout=timeout)
    out = bytearray()

    def pump(wait):
        end = time.time() + wait
        while time.time() < end:
            sock.settimeout(max(0.1, end - time.time()))
            try:
                data = sock.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            i = 0
            while i < len(data):
                b = data[i]
                if b == iac and i + 1 < len(data):
                    c = data[i + 1]
                    if c in (do, dont, will, wont) and i + 2 < len(data):
                        sock.send(bytes([iac, wont if c in (do, dont) else dont, data[i + 2]]))
                        i += 3
                        continue
                    if c == sb:
                        j = data.find(bytes([iac, se]), i)
                        i = j + 2 if j >= 0 else len(data)
                        continue
                    i += 2
                    continue
                out.append(b)
                i += 1

    pump(30)
    sock.send(b"root\r\n")
    pump(30)
    if b"TERM" in out[-80:]:  # root's .login asks only when the session has no terminal type
        sock.send(b"vt100\r\n")
        pump(20)
    for line, wait in [(c, 30) for c in commands] + [("exit", 10)]:
        sock.send(line.encode() + b"\r\n")
        pump(wait)
    sock.close()
    return out.decode("latin-1").replace("\r", "")


def phase_c(args, results):
    note("C. cold boot of the installed disk; network login")
    guest = Guest(emulator(args, [], network=True), os.path.join(args.work, "c-coldboot.log"))
    boot_to_shell(guest)
    guest.send("echo rc-ip27-persist > /usr/tmp/rc-ip27-demo; sync")
    guest.wait_for(r"# $", 600)
    if args.tap:
        guest.send(f"ifconfig ef0 inet {args.guest_ip} netmask 255.255.255.0 up")
        guest.wait_for(r"# $", 600)
        time.sleep(5)
        note(f"    telnet {args.guest_ip}")
        session = telnet_session(args.guest_ip, ["uname -a", "hinv", "id", "pwd", "date", "df -k"])
        results.write("=== telnet session from the host\n" + session + "\n")
        results.flush()
        if "IRIX" not in session or "login" not in session:
            raise RuntimeError("no IRIX login over telnet")
    guest.send("shutdown -y -g0 -i0")
    guest.wait_for(r"IP27 PROM", 3600, quiet=0)
    guest.finish()


def phase_d(args, results):
    note("D. cold start after the clean shutdown")
    guest = Guest(emulator(args, []), os.path.join(args.work, "d-restart.log"))
    boot_to_shell(guest)
    guest.send("cat /usr/tmp/rc-ip27-demo")
    guest.wait_for(r"# $", 600)
    persisted = "rc-ip27-persist" in guest.since_mark()
    results.write(f"=== persistence across clean shutdown and cold start: {persisted}\n")
    guest.send("shutdown -y -g0 -i0")
    guest.wait_for(r"IP27 PROM", 3600, quiet=0)
    guest.finish()
    if not persisted:
        raise RuntimeError("the file written before shutdown is missing")


def main():
    global LOG
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--work", default="local/rc")
    parser.add_argument("--media", default=os.environ.get("ULTRAVIOLENT_IRIX_MEDIA", "assets/media"))
    parser.add_argument("--prom", default=os.environ.get("ULTRAVIOLENT_IP27_PROM",
                                                         "assets/firmware/ip27prom.img"))
    parser.add_argument("--binary", default="build/pgo/ultraviolent")
    parser.add_argument("--io6prom", default=os.path.join(
        os.path.dirname(os.environ.get("ULTRAVIOLENT_IP27_PROM", "assets/firmware/ip27prom.img")),
        "io6prom.img"), help="the BaseIO flash PROM image (empty for none)")
    parser.add_argument("--tap", help="TAP interface for the network login")
    parser.add_argument("--guest-ip", default="192.168.77.2")
    parser.add_argument("--disk-size", default="9G")
    parser.add_argument("--install-hours", type=float, default=16)
    parser.add_argument("--from-phase", default="A", choices="ABCD")
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    LOG = open(os.path.join(args.work, "driver.log"), "a")
    results = open(os.path.join(args.work, "results.txt"), "a")
    if args.from_phase == "A":
        for name in ("flash.bin", "nvram.bin", "disk.img"):
            path = os.path.join(args.work, name)
            if os.path.exists(path):
                sys.exit(f"{path} exists: the demonstration starts from blank state")
        subprocess.run(["truncate", "-s", args.disk_size, os.path.join(args.work, "disk.img")],
                       check=True)
    start = time.time()
    phases = {"A": phase_a, "B": phase_b, "C": lambda a: phase_c(a, results),
              "D": lambda a: phase_d(a, results)}
    for name in "ABCD"[ "ABCD".index(args.from_phase):]:
        began = time.time()
        phases[name](args)
        note(f"{name} done in {(time.time() - began) / 60:.1f} min")
    note(f"demonstration complete in {(time.time() - start) / 3600:.2f} h")
    results.write("=== demonstration complete\n")


if __name__ == "__main__":
    main()
