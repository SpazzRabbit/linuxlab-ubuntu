#!/usr/bin/python3
"""The PRoot side of the chroot mode: asks llroot to run a desktop (see overlay/root/llroot.c).

  llchroot.py run gnome|lxqt   the desktop runs in a chroot of this tree, as the user of Linux Lab, and
                               this program waits for it and ends with its exit code; 90 when llroot
                               does not answer (startlxqt then starts the desktop under PRoot)
  llchroot.py status           "running <uid> <SELinux level>" or "absent"

llroot is a service of the rooted tablet, started through su by the helper app of Android; this tree
holds no part of it. TERM, INT or HUP tell the desktop to end; a second one, or the end of this
program, makes llroot end all of it.
"""
import os
import signal
import socket
import sys
import time

PORT = 54396
NOT_THERE = 90


def connect(wait):
    """A connection to llroot, trying for @wait seconds: the helper app may still be starting it."""
    deadline = time.monotonic() + wait
    while True:
        try:
            return socket.create_connection(("127.0.0.1", PORT), 2)
        except OSError:
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.5)


def status():
    link = connect(0)
    if not link:
        print("absent")
        return 1
    with link:
        link.sendall(b"hello\n")
        words = link.makefile().readline().split()
    if len(words) >= 4 and words[0] == "LLROOT":
        print("running", words[2], words[3])
        return 0
    print("absent")
    return 1


def run(desktop):
    link = connect(8)
    if not link:
        print("[llchroot] llroot does not answer", file=sys.stderr)
        return NOT_THERE
    link.settimeout(None)
    lines = ["env %s=%s" % (name, value) for name, value in os.environ.items() if "\n" not in name + value]
    link.sendall(("\n".join(lines) + "\nrun %s\n" % desktop).encode())
    reader = link.makefile()
    told = []

    def stop(*_):
        if told:
            os._exit(1)  # the connection goes, and llroot ends everything
        told.append(1)
        try:
            link.sendall(b"stop\n")
        except OSError:
            pass

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, stop)
    code = 1
    while True:
        try:
            line = reader.readline()
        except InterruptedError:
            continue
        if not line:
            break
        words = line.split()
        if words[:1] == ["started"]:
            print("[llchroot] %s runs in the chroot, process %s" % (desktop, words[1]), file=sys.stderr)
        elif words[:1] == ["exit"]:
            code = int(words[1])
            break
        elif words[:1] == ["error"]:
            print("[llchroot] llroot:", line.strip(), file=sys.stderr)
            return NOT_THERE
    return code


def main(arguments):
    if arguments == ["status"]:
        return status()
    if len(arguments) == 2 and arguments[0] == "run" and arguments[1] in ("gnome", "lxqt"):
        return run(arguments[1])
    print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
