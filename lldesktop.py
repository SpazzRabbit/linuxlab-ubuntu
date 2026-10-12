#!/usr/bin/python3
"""The switches of the desktop of Linux Lab: Ubuntu's GNOME or Honor's LXQt, under PRoot or in a chroot.

  lldesktop.py --serve <pid>     the listener; startlxqt starts it and is the process <pid>
  lldesktop.py gnome | lxqt      switch the desktop (what install.sh and a person in a terminal use)
  lldesktop.py proot | chroot    switch the way it runs
  lldesktop.py                   say which desktop is running, and how

The choices are the files /etc/linuxlab-desktop and /etc/linuxlab-engine, which startlxqt reads before
it starts a desktop. The listener waits on 127.0.0.1:54397 so that the helper app of Android can make
them too: it greets with "LLDESKTOP 2 <desktop> <engine> <payload> <serial>" (what is running, then the
mark and the release serial of the installer that set this desktop up, "-" and 0 when it wrote none),
and on the line "desktop
gnome", "desktop lxqt", "engine proot" or "engine chroot" it writes the file and signals startlxqt,
which ends the running desktop and starts the chosen one. It answers "ok" or "error <why>". A chroot
is chosen only while llroot, the root service the helper app starts on a rooted tablet, answers.

Like the other ports of this project it listens on 127.0.0.1 only and does not ask who is there: any
program on the tablet can switch the desktop. Only the Python that Ubuntu ships is needed, so it works
under either desktop.
"""
import os
import signal
import socket
import sys

PORT = 54397
CHOICE = "/etc/linuxlab-desktop"
RUNNING = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "linuxlab-desktop")
ENGINE_CHOICE = "/etc/linuxlab-engine"
ENGINE_RUNNING = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "linuxlab-engine")
SESSION = "/opt/linuxlab-gnome/gnome-session.sh"
# written by the installer of the helper app; the app offers its own one while its serial is higher
PAYLOAD = "/opt/linuxlab-gnome/payload"
RELEASE = "/opt/linuxlab-gnome/release"
DESKTOPS = ("gnome", "lxqt")
ENGINES = ("proot", "chroot")
LLROOT_PORT = 54396


def read_word(path, words):
    """The word in @path if it is one of @words, else the first of them."""
    try:
        with open(path) as f:
            name = f.read().strip()
    except OSError:
        name = ""
    return name if name in words else words[0]


def running():
    return read_word(RUNNING, ("lxqt", "gnome"))


def chosen():
    return read_word(CHOICE, ("lxqt", "gnome"))


def payload():
    try:
        with open(PAYLOAD) as f:
            mark = f.read().strip()
    except OSError:
        return "-"
    return mark if mark.isalnum() else "-"


def serial():
    try:
        with open(RELEASE) as f:
            return int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def engine_running():
    return read_word(ENGINE_RUNNING, ENGINES)


def write_word(path, name):
    with open(path + ".new", "w") as f:
        f.write(name + "\n")
    os.replace(path + ".new", path)


def choose(name):
    write_word(CHOICE, name)


def llroot_answers():
    try:
        with socket.create_connection(("127.0.0.1", LLROOT_PORT), 2) as link:
            link.sendall(b"hello\n")
            return link.makefile().readline().startswith("LLROOT ")
    except OSError:
        return False


def answer(client, starter):
    client.settimeout(5)
    client.sendall(("LLDESKTOP 2 %s %s %s %d\n" % (running(), engine_running(), payload(), serial())).encode())
    line = b""
    while not line.endswith(b"\n") and len(line) < 64:
        piece = client.recv(64)
        if not piece:
            break
        line += piece
    words = line.decode("ascii", "replace").split()
    if not words:
        return
    if len(words) == 2 and words[0] == "desktop" and words[1] in DESKTOPS:
        if words[1] == "gnome" and not os.access(SESSION, os.X_OK):
            client.sendall(b"error GNOME is not installed\n")
            return
        choose(words[1])
        if words[1] != running():
            os.kill(starter, signal.SIGUSR1)
        client.sendall(b"ok\n")
    elif len(words) == 2 and words[0] == "engine" and words[1] in ENGINES:
        if words[1] == "chroot" and not llroot_answers():
            client.sendall(b"error llroot does not answer\n")
            return
        write_word(ENGINE_CHOICE, words[1])
        if words[1] != engine_running():
            os.kill(starter, signal.SIGUSR1)
        client.sendall(b"ok\n")
    else:
        client.sendall(b"error what\n")


def serve(starter):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", PORT))
    server.listen(4)
    server.settimeout(10)
    while True:
        try:
            os.kill(starter, 0)
        except OSError:
            return 0  # Linux Lab is closing
        try:
            client, _address = server.accept()
        except socket.timeout:
            continue
        try:
            answer(client, starter)
        except OSError:
            pass
        finally:
            client.close()


def ask(command):
    """The answer of the listener to @command ("desktop gnome"), or the greeting when @command is None."""
    with socket.create_connection(("127.0.0.1", PORT), 3) as link:
        link.settimeout(10)
        reader = link.makefile()
        greeting = reader.readline().split()
        if len(greeting) < 3 or greeting[0] != "LLDESKTOP":
            raise OSError("something else listens on port %d" % PORT)
        if command is None:
            return " ".join(greeting[2:])
        link.sendall(("%s\n" % command).encode())
        return reader.readline().strip()


def main(arguments):
    if len(arguments) == 2 and arguments[0] == "--serve":
        return serve(int(arguments[1]))
    if len(arguments) > 1 or (arguments and arguments[0] not in DESKTOPS + ENGINES):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    name = arguments[0] if arguments else None
    kind = "engine" if name in ENGINES else "desktop"
    try:
        reply = ask("%s %s" % (kind, name) if name else None)
    except OSError:
        # no listener: Linux Lab was started before this was installed, or by Honor's own startlxqt
        if name is None:
            print("desktop: nobody to ask; the choice for the next start of Linux Lab is %s, %s"
                  % (chosen(), read_word(ENGINE_CHOICE, ENGINES)))
            return 0
        if name == "gnome" and not os.access(SESSION, os.X_OK):
            print("GNOME is not installed", file=sys.stderr)
            return 1
        write_word(ENGINE_CHOICE if kind == "engine" else CHOICE, name)
        print("%s: %s. Close Linux Lab and open it again." % (kind, name))
        return 0
    if name is None:
        print("desktop: %s" % reply)
        return 0
    if reply != "ok":
        print(reply, file=sys.stderr)
        return 1
    print("%s: %s" % (kind, name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
