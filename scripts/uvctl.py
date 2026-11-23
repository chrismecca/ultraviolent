#!/usr/bin/env python3
"""uvctl: many Ultraviolent machines on one host (doc/ORCHESTRATION.adoc "O4").

A fleet file (TOML) describes machines and the virtual switches they share:

    [fleet]
    dir = "fleet"                     # state, relative to this file (default: its name)

    [switch.lan]
    tap = "uv0"                       # optional: join the host through a TAP you own

    [machine.alpha]
    ethernet = "lan"                  # a switch above
    cpus = 2
    parallel = true

Each machine's files live in DIR/NAME: its disks (disk.cow, a copy-on-write overlay, or
disk.img), nvram.bin, flash.bin (flash1.bin, ... for more nodes), and while it runs
console.sock, console.log (the console), machine.log (the emulator's messages), and pid. A
suspended machine also has suspend.state. Frozen disk layers that overlays rest on are kept
in DIR/layers and never change.

    uvctl create NAME --from DIR       new machine on an installed machine's disk (overlay)
    uvctl clone SOURCE NAME            new machine sharing SOURCE's disk as it is now
    uvctl up [NAME...]                 start (or resume) machines, and the switches they use
    uvctl down [--force] [NAME...]     keyswitch off: IRIX shuts down, power goes off
    uvctl suspend [NAME...]            stop now, keeping the running machine (suspend.state)
    uvctl snapshot NAME TAG            keep a stopped or suspended machine's state as TAG
    uvctl restore NAME TAG             go back to TAG (what was written since is lost)
    uvctl snapshots NAME               list TAGs
    uvctl console NAME                 attach to the console; Ctrl-] then d detaches
    uvctl status

Without NAMEs, up, down, and suspend act on every machine in the fleet file; down without
NAMEs also stops the switches. Machines run with --realtime. The fleet file is -f FILE, or
$UVCTL_FLEET, or ./fleet.toml.
"""

import argparse
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESCAPE = b"\x1d"
FOREVER_CYCLES = "100000000000000000"


def fail(message):
    print(f"uvctl: {message}", file=sys.stderr)
    sys.exit(1)


class Fleet:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        try:
            with open(self.path, "rb") as f:
                self.config = tomllib.load(f)
        except FileNotFoundError:
            fail(f"no fleet file {path} (see uvctl --help)")
        except tomllib.TOMLDecodeError as error:
            fail(f"{path}: {error}")
        base = os.path.dirname(self.path)
        fleet = self.config.get("fleet", {})
        default_dir = os.path.splitext(os.path.basename(self.path))[0]
        self.dir = os.path.join(base, fleet.get("dir", default_dir))
        self.binary = self._binary(fleet.get("binary"), base)
        prom = fleet.get("prom", os.environ.get("ULTRAVIOLENT_IP27_PROM",
                                                os.path.join(ROOT, "assets/firmware/ip27prom.img")))
        self.prom = os.path.join(base, prom)
        self.io6prom = os.path.join(base, fleet.get("io6prom", os.path.join(
            os.path.dirname(self.prom), "io6prom.img")))
        self.machines = self.config.get("machine", {})
        self.switches = self.config.get("switch", {})
        for name, machine in self.machines.items():
            network = machine.get("ethernet")
            if network and network not in self.switches and not network.startswith("tap:"):
                fail(f"machine {name}: no switch {network!r} in the fleet file")
        addresses = {}
        for name in self.machines:
            address = self.ethernet_address(name)
            if address in addresses:
                fail(f"machines {addresses[address]} and {name} have the same Ethernet address "
                     f"{address}; set ethernet_address for one")
            addresses[address] = name

    @staticmethod
    def _binary(configured, base):
        if configured:
            return os.path.join(base, configured)
        for candidate in ("build/pgo/ultraviolent", "build/release/ultraviolent"):
            path = os.path.join(ROOT, candidate)
            if os.path.exists(path):
                return path
        fail("no ultraviolent binary (build it, or set binary in [fleet])")

    def machine(self, name):
        if name not in self.machines:
            fail(f"no machine {name!r} in {self.path}")
        return self.machines[name]

    def names(self, names):
        for name in names:
            self.machine(name)
        return names or list(self.machines)

    def home(self, name):
        return os.path.join(self.dir, name)

    def file(self, name, leaf):
        return os.path.join(self.home(name), leaf)

    def layers(self):
        return os.path.join(self.dir, "layers")

    def ethernet_address(self, name):
        configured = self.machines[name].get("ethernet_address")
        if configured:
            return configured.lower()
        # SGI's prefix and three bytes from the name: stable, and different per name.
        digest = hashlib.sha256(name.encode()).digest()
        return "08:00:69:" + ":".join(f"{b:02x}" for b in digest[:3])

    def nodes(self, name):
        return int(self.machines[name].get("nodes", 1))

    def flash_files(self, name):
        return [self.file(name, "flash.bin" if n == 0 else f"flash{n}.bin")
                for n in range(self.nodes(name))]

    def disks(self, name):
        """The machine's disk files, in --disk order (placements kept)."""
        configured = self.machines[name].get("disks")
        if configured is None:
            for leaf in ("disk.cow", "disk.img"):
                if os.path.exists(self.file(name, leaf)):
                    return [self.file(name, leaf)]
            return [self.file(name, "disk.cow")]
        result = []
        for entry in configured:
            place, _, leaf = entry.rpartition("=")
            path = leaf if os.path.isabs(leaf) else self.file(name, leaf)
            result.append(f"{place}={path}" if place else path)
        return result

    # Switches.

    def switch_socket(self, network):
        return os.path.join(self.dir, "switches", network + ".sock")

    def switch_pid(self, network):
        return running(os.path.join(self.dir, "switches", network + ".pid"))

    def start_switch(self, network):
        if self.switch_pid(network):
            return
        os.makedirs(os.path.join(self.dir, "switches"), exist_ok=True)
        sock = self.switch_socket(network)
        if os.path.exists(sock):
            os.unlink(sock)  # left by a switch that did not stop cleanly
        command = [self.binary, "switch", sock]
        tap = self.switches[network].get("tap")
        if tap:
            command += ["--tap", tap]
        log = open(os.path.join(self.dir, "switches", network + ".log"), "ab")
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                   start_new_session=True)
        write_pid(os.path.join(self.dir, "switches", network + ".pid"), process.pid)
        for _ in range(50):
            if os.path.exists(sock):
                print(f"switch {network} up")
                return
            if process.poll() is not None:
                break
            time.sleep(0.1)
        fail(f"switch {network} did not start (see {log.name})")

    def stop_switch(self, network):
        pid = self.switch_pid(network)
        if pid:
            os.kill(pid, signal.SIGTERM)
            wait_exit(pid, 10)
            print(f"switch {network} down")

    # Machines.

    def pid(self, name):
        return running(self.file(name, "pid"))

    def command(self, name):
        m = self.machines[name]
        command = [self.binary, "--machine", "ip27", "--prom", self.prom,
                   "--io6prom", self.io6prom,
                   "--nvram", self.file(name, "nvram.bin"),
                   "--suspend", self.file(name, "suspend.state"),
                   "--console-socket", self.file(name, "console.sock"),
                   "--realtime", "--cycles", FOREVER_CYCLES]
        for flash in self.flash_files(name):
            command += ["--flash", flash]
        for disk in self.disks(name):
            command += ["--disk", disk]
        network = m.get("ethernet")
        if network:
            cable = network if network.startswith("tap:") else "switch:" + self.switch_socket(network)
            command += ["--ethernet", cable, "--ethernet-address", self.ethernet_address(name)]
        for key, flag in (("memory", "--memory"), ("cpus", "--cpus"), ("nodes", "--nodes"),
                          ("module", "--module"), ("cpu", "--cpu"), ("engine", "--engine")):
            if key in m:
                command += [flag, str(m[key])]
        if m.get("parallel"):
            command.append("--parallel")
        if m.get("express_links"):
            command.append("--express-links")
        command += [str(option) for option in m.get("options", [])]
        return command

    def up(self, name):
        if self.pid(name):
            print(f"{name}: already running")
            return
        if not os.path.isdir(self.home(name)):
            fail(f"{name}: no machine directory {self.home(name)} (uvctl create or clone)")
        for required in [self.file(name, "nvram.bin")] + [
                d.rpartition("=")[2] for d in self.disks(name)]:
            if not os.path.exists(required):
                fail(f"{name}: missing {required}")
        network = self.machines[name].get("ethernet")
        if network and not network.startswith("tap:"):
            self.start_switch(network)
        sock = self.file(name, "console.sock")
        if os.path.exists(sock):
            os.unlink(sock)
        resuming = os.path.exists(self.file(name, "suspend.state"))
        console = open(self.file(name, "console.log"), "ab")
        log = open(self.file(name, "machine.log"), "ab")
        process = subprocess.Popen(self.command(name), stdin=subprocess.DEVNULL, stdout=console,
                                   stderr=log, start_new_session=True, cwd=self.home(name))
        write_pid(self.file(name, "pid"), process.pid)
        for _ in range(100):
            if os.path.exists(sock):
                print(f"{name}: {'resumed' if resuming else 'powered on'}")
                return
            if process.poll() is not None:
                break
            time.sleep(0.1)
        fail(f"{name}: did not start (see {log.name})")

    def stop(self, name, how, timeout):
        pid = self.pid(name)
        if not pid:
            print(f"{name}: not running")
            return False
        os.kill(pid, how)
        if not wait_exit(pid, timeout):
            fail(f"{name}: still running after {timeout} s (pid {pid})")
        return True


def write_pid(path, pid):
    with open(path, "w") as f:
        f.write(f"{pid}\n")


def running(pid_file):
    """The pid in pid_file if that process is an ultraviolent still running, else None."""
    try:
        with open(pid_file) as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            if b"ultraviolent" not in f.read():
                return None
        with open(f"/proc/{pid}/stat") as f:
            if f.read().rsplit(")", 1)[1].split()[0] == "Z":
                return None
        return pid
    except (OSError, ValueError):
        return None


def wait_exit(pid, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            # Reap it if it is our child; otherwise just look.
            if os.waitpid(pid, os.WNOHANG)[0] == pid:
                return True
        except ChildProcessError:
            pass
        try:
            with open(f"/proc/{pid}/stat") as f:
                if f.read().rsplit(")", 1)[1].split()[0] == "Z":
                    return True
        except OSError:
            return True
        time.sleep(0.2)
    return False


# Disk layers: a machine's disk is frozen into DIR/layers and the machine continues on a
# fresh overlay above it, so that snapshots and clones share it without copying.

def overlay(fleet, path, base):
    result = subprocess.run([fleet.binary, "disk", "create", path, os.path.abspath(base)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        fail(result.stderr.strip())


def disk_leafs(fleet, name):
    return [d.rpartition("=")[2] for d in fleet.disks(name)]


def freeze(fleet, name, label):
    """Freezes NAME's disks into layers; NAME goes on on new overlays. Returns the layers."""
    os.makedirs(fleet.layers(), exist_ok=True)
    frozen = []
    for index, path in enumerate(disk_leafs(fleet, name)):
        extension = os.path.splitext(path)[1] or ".img"
        layer = os.path.join(fleet.layers(), f"{name}-{label}-{index}{extension}")
        if os.path.exists(layer):
            fail(f"layer {layer} exists")
        os.rename(path, layer)
        os.chmod(layer, 0o444)
        # The new overlay takes the disk's own name, whatever it is: the emulator knows an
        # overlay by its header, so the fleet file stays right.
        overlay(fleet, path, layer)
        frozen.append(layer)
    return frozen


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def require_stopped(fleet, name):
    if fleet.pid(name):
        fail(f"{name} is running; uvctl down or suspend it first")


def copy_machine_files(source_dir, target_dir):
    """The NVRAM and flash files (the machine's lasting state besides its disks)."""
    for leaf in os.listdir(source_dir):
        if leaf == "nvram.bin" or (leaf.startswith("flash") and leaf.endswith(".bin")):
            shutil.copy2(os.path.join(source_dir, leaf), os.path.join(target_dir, leaf))


def cmd_create(fleet, args):
    fleet.machine(args.name)
    home = fleet.home(args.name)
    if os.path.exists(home):
        fail(f"{home} exists")
    source = os.path.abspath(args.source)
    base = os.path.join(source, "disk.img")
    if not os.path.exists(base):
        base = os.path.join(source, "disk.cow")
    for required in (base, os.path.join(source, "nvram.bin")):
        if not os.path.exists(required):
            fail(f"missing {required}")
    os.makedirs(home)
    overlay(fleet, disk_leafs(fleet, args.name)[0], base)
    copy_machine_files(source, home)
    print(f"{args.name}: created on {base} (which must not change while overlays use it)")


def cmd_clone(fleet, args):
    fleet.machine(args.source)
    fleet.machine(args.name)
    require_stopped(fleet, args.source)
    if os.path.exists(fleet.file(args.source, "suspend.state")):
        fail(f"{args.source} is suspended; a clone starts from a machine that is off")
    home = fleet.home(args.name)
    if os.path.exists(home):
        fail(f"{home} exists")
    targets = disk_leafs(fleet, args.name)
    if len(targets) != len(disk_leafs(fleet, args.source)):
        fail(f"give {args.name} as many disks as {args.source} in the fleet file")
    layers = freeze(fleet, args.source, "clone-" + stamp())
    os.makedirs(home)
    for target, layer in zip(targets, layers):
        overlay(fleet, target, layer)
    copy_machine_files(fleet.home(args.source), home)
    print(f"{args.name}: cloned from {args.source}")


def cmd_up(fleet, args):
    for name in fleet.names(args.names):
        fleet.up(name)


def cmd_down(fleet, args):
    names = fleet.names(args.names)
    for name in names:
        if args.force:
            # Stopped at once, like pulling the plug: no suspend file is kept.
            if fleet.stop(name, signal.SIGTERM, 120):
                state = fleet.file(name, "suspend.state")
                if os.path.exists(state):
                    os.unlink(state)
                print(f"{name}: stopped")
        elif fleet.stop(name, signal.SIGPWR, args.timeout):
            print(f"{name}: powered off")
    if not args.names:
        for network in fleet.switches:
            fleet.stop_switch(network)


def cmd_suspend(fleet, args):
    for name in fleet.names(args.names):
        if fleet.stop(name, signal.SIGTERM, 300):
            if not os.path.exists(fleet.file(name, "suspend.state")):
                fail(f"{name}: stopped, but no suspend file (see machine.log)")
            print(f"{name}: suspended")


def snapshot_dir(fleet, name, tag):
    return os.path.join(fleet.home(name), "snapshots", tag)


def cmd_snapshot(fleet, args):
    fleet.machine(args.name)
    require_stopped(fleet, args.name)
    where = snapshot_dir(fleet, args.name, args.tag)
    if os.path.exists(where):
        fail(f"snapshot {args.tag} exists")
    layers = freeze(fleet, args.name, args.tag)
    os.makedirs(where)
    copy_machine_files(fleet.home(args.name), where)
    state = fleet.file(args.name, "suspend.state")
    if os.path.exists(state):
        shutil.copy2(state, os.path.join(where, "suspend.state"))
    with open(os.path.join(where, "snapshot.json"), "w") as f:
        json.dump({"layers": layers, "disks": disk_leafs(fleet, args.name),
                   "taken": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "suspended": os.path.exists(state)}, f, indent=1)
    print(f"{args.name}: snapshot {args.tag}" + (" (suspended)" if os.path.exists(state) else ""))


def cmd_restore(fleet, args):
    fleet.machine(args.name)
    require_stopped(fleet, args.name)
    where = snapshot_dir(fleet, args.name, args.tag)
    try:
        with open(os.path.join(where, "snapshot.json")) as f:
            snapshot = json.load(f)
    except OSError:
        fail(f"no snapshot {args.tag} of {args.name}")
    for disk, layer in zip(snapshot["disks"], snapshot["layers"]):
        if os.path.exists(disk):
            os.unlink(disk)
        overlay(fleet, disk, layer)
    copy_machine_files(where, fleet.home(args.name))
    state = fleet.file(args.name, "suspend.state")
    if os.path.exists(state):
        os.unlink(state)
    if snapshot.get("suspended"):
        shutil.copy2(os.path.join(where, "suspend.state"), state)
    print(f"{args.name}: restored {args.tag}")


def cmd_snapshots(fleet, args):
    fleet.machine(args.name)
    root = os.path.join(fleet.home(args.name), "snapshots")
    for tag in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        try:
            with open(os.path.join(root, tag, "snapshot.json")) as f:
                snapshot = json.load(f)
        except OSError:
            continue
        print(f"{tag:20} {snapshot['taken']}{'  suspended' if snapshot.get('suspended') else ''}")


def cmd_status(fleet, args):
    for network in fleet.switches:
        pid = fleet.switch_pid(network)
        print(f"switch {network:12} {'up (pid %d)' % pid if pid else 'down'}")
    for name in fleet.machines:
        pid = fleet.pid(name)
        if pid:
            state = f"running (pid {pid})"
        elif os.path.exists(fleet.file(name, "suspend.state")):
            state = "suspended"
        elif os.path.isdir(fleet.home(name)):
            state = "off"
        else:
            state = "not created"
        network = fleet.machines[name].get("ethernet", "")
        print(f"{name:20} {state:22} {network} {fleet.ethernet_address(name) if network else ''}")


def cmd_console(fleet, args):
    fleet.machine(args.name)
    if not fleet.pid(args.name):
        fail(f"{args.name} is not running")
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.connect(fleet.file(args.name, "console.sock"))
    import select
    import termios
    import tty
    interactive = os.isatty(0)
    saved = termios.tcgetattr(0) if interactive else None
    print(f"[{args.name} console; Ctrl-] then d detaches]\r", file=sys.stderr)
    try:
        if interactive:
            tty.setraw(0)
        escaped = False
        while True:
            ready, _, _ = select.select([0, connection], [], [])
            if connection in ready:
                data = connection.recv(65536)
                if not data:
                    break
                os.write(1, data)
            if 0 in ready:
                data = os.read(0, 1024)
                if not data:
                    break
                out = b""
                for byte in data:
                    b = bytes([byte])
                    if escaped:
                        escaped = False
                        if b in (b"d", b"D", b"."):
                            return
                        out += ESCAPE + b
                    elif b == ESCAPE:
                        escaped = True
                    else:
                        out += b
                if out:
                    connection.sendall(out)
    finally:
        if saved is not None:
            termios.tcsetattr(0, termios.TCSADRAIN, saved)
        connection.close()
        print(f"\r\n[detached from {args.name}]", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-f", "--fleet", default=os.environ.get("UVCTL_FLEET", "fleet.toml"))
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("create")
    p.add_argument("name")
    p.add_argument("--from", dest="source", required=True)
    p.set_defaults(run=cmd_create)
    p = commands.add_parser("clone")
    p.add_argument("source")
    p.add_argument("name")
    p.set_defaults(run=cmd_clone)
    p = commands.add_parser("up")
    p.add_argument("names", nargs="*")
    p.set_defaults(run=cmd_up)
    p = commands.add_parser("down")
    p.add_argument("--force", action="store_true")
    p.add_argument("--timeout", type=float, default=600)
    p.add_argument("names", nargs="*")
    p.set_defaults(run=cmd_down)
    p = commands.add_parser("suspend")
    p.add_argument("names", nargs="*")
    p.set_defaults(run=cmd_suspend)
    p = commands.add_parser("resume")
    p.add_argument("names", nargs="*")
    p.set_defaults(run=cmd_up)
    p = commands.add_parser("snapshot")
    p.add_argument("name")
    p.add_argument("tag")
    p.set_defaults(run=cmd_snapshot)
    p = commands.add_parser("restore")
    p.add_argument("name")
    p.add_argument("tag")
    p.set_defaults(run=cmd_restore)
    p = commands.add_parser("snapshots")
    p.add_argument("name")
    p.set_defaults(run=cmd_snapshots)
    p = commands.add_parser("console")
    p.add_argument("name")
    p.set_defaults(run=cmd_console)
    p = commands.add_parser("status")
    p.set_defaults(run=cmd_status)
    args = parser.parse_args()
    args.run(Fleet(args.fleet), args)


if __name__ == "__main__":
    main()
