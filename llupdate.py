#!/usr/bin/python3
"""Updates of the Linux side of Linux Lab Helper, from the releases of its repository on GitHub.

A while after the desktop starts, and then every few hours, the latest release is asked for its
manifest.json and manifest.json.sig. The signature is checked with gpgv against the key of the
project (release-key.gpg next to this file; the secret half never leaves the computer the releases
are made on). A release with a higher serial than the one installed (the file "release" next to this
file, written by the installer) is offered in a notification of the desktop. "Install" downloads the
installer the manifest names, checks its size and SHA-256 against the signed manifest and runs it
with the word "update", through sudo, which asks no password in Linux Lab. Linux Lab has to be
started again afterwards: the notification says so.

  llupdate.py            the service (gnome-session.sh starts it)
  llupdate.py check      look once and say what was found, without a notification

Nothing is downloaded but the two small files until the person asks for the update. The extension of
the shell shows the service in the top bar (dev.linuxlab.Update on the session bus): the version, what
was found, "Check for updates" and "Install". LINUXLAB_UPDATE=off in /etc/default/linuxlab-gnome
switches the service off.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

REPOSITORY = "SpazzRabbit/linuxlab-ubuntu"
LATEST = "https://github.com/%s/releases/latest/download/" % REPOSITORY
HERE = os.path.dirname(os.path.abspath(__file__))
KEY = os.path.join(HERE, "release-key.gpg")
RELEASE = os.path.join(HERE, "release")
CACHE = os.path.join(GLib.get_user_cache_dir(), "linuxlab-update")
FIRST_LOOK_S = 30
LOOK_EVERY_S = 6 * 3600
TIMEOUT_S = 30


def russian():
    return "ru" in (os.environ.get("LANGUAGE", "") + os.environ.get("LANG", ""))


def say(english, russian_text):
    return russian_text if russian() else english


def log(text):
    print("[llupdate] " + text, file=sys.stderr, flush=True)


def installed_serial():
    """The serial of the release the installer put here; 0 for an older installer, which wrote none."""
    try:
        with open(RELEASE) as f:
            return int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def fetch(url, limit):
    request = urllib.request.Request(url, headers={"User-Agent": "linuxlab-update"})
    with urllib.request.urlopen(request, timeout=TIMEOUT_S) as answer:
        data = answer.read(limit + 1)
    if len(data) > limit:
        raise ValueError("%s is larger than %d bytes" % (url, limit))
    return data


def verified(data, signature):
    """True when @signature is a good signature of @data by the key of the project."""
    with tempfile.TemporaryDirectory() as work:
        paths = [os.path.join(work, name) for name in ("manifest.json", "manifest.json.sig")]
        for path, content in zip(paths, (data, signature)):
            with open(path, "wb") as f:
                f.write(content)
        result = subprocess.run(["gpgv", "--keyring", KEY, paths[1], paths[0]],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode != 0:
        log("the signature of the manifest does not hold: " + result.stderr.decode(errors="replace").strip())
    return result.returncode == 0


def latest():
    """The signed manifest of the latest release, or None (no network, no release, a bad signature)."""
    # a query of its own each time: right after a release, "latest" is still the old one in a cache for a while
    fresh = "?t=%d" % time.time()
    try:
        data = fetch(LATEST + "manifest.json" + fresh, 65536)
        signature = fetch(LATEST + "manifest.json.sig" + fresh, 65536)
    except (OSError, ValueError) as e:
        log("no answer from GitHub: %s" % e)
        return None
    if not verified(data, signature):
        return None
    try:
        manifest = json.loads(data)
        int(manifest["serial"])
        int(manifest["installer"]["size"])
        if "/" in manifest["installer"]["name"] or len(manifest["installer"]["sha256"]) != 64:
            raise ValueError("a strange installer")
    except (ValueError, KeyError, TypeError) as e:
        log("the manifest makes no sense: %s" % e)
        return None
    return manifest


def notes(manifest):
    text = manifest.get("notes", {})
    return text.get("ru" if russian() else "en", "") if isinstance(text, dict) else ""


INTERFACE = """
<node>
  <interface name="dev.linuxlab.Update">
    <method name="Check"/>
    <method name="Install"/>
    <method name="GetState"><arg type="a{ss}" direction="out" name="state"/></method>
    <signal name="Changed"><arg type="a{ss}" name="state"/></signal>
  </interface>
</node>"""


def installed_version():
    try:
        with open(RELEASE) as f:
            return f.read().split()[1]
    except (OSError, IndexError):
        return ""


class Service:
    """The looking, the notifications and dev.linuxlab.Update on the session bus, which the extension of
    the shell shows in the top bar: state is one of idle, checking, current, available, installing,
    installed (Linux Lab is to be started again), failed, offline."""

    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.offer = None
        self.shown = 0
        self.offered_serial = 0
        self.process = None
        self.state = "idle"
        self.error = ""
        self.bus.signal_subscribe("org.freedesktop.Notifications", "org.freedesktop.Notifications",
                                  "ActionInvoked", "/org/freedesktop/Notifications", None,
                                  Gio.DBusSignalFlags.NONE, self.on_action)
        node = Gio.DBusNodeInfo.new_for_xml(INTERFACE)
        register = getattr(self.bus, "register_object_with_closures2", None) or self.bus.register_object
        register("/dev/linuxlab/Update", node.interfaces[0], self.on_call, None, None)
        Gio.bus_own_name_on_connection(self.bus, "dev.linuxlab.Update", Gio.BusNameOwnerFlags.NONE, None, None)

    # ---- dev.linuxlab.Update

    def snapshot(self):
        state = {"state": self.state, "installed": installed_version(), "error": self.error}
        if self.offer:
            state["latest"] = str(self.offer.get("version", ""))
            state["notes"] = notes(self.offer)
            state["page"] = "https://github.com/%s/releases/tag/%s" % (REPOSITORY, self.offer.get("tag", ""))
        return state

    def on_call(self, _bus, _sender, _path, _interface, method, _parameters, invocation):
        if method == "Check":
            self.check(by_hand=True)
        elif method == "Install":
            if self.offer and self.state == "available":
                self.install(self.offer)
        elif method == "GetState":
            invocation.return_value(GLib.Variant("(a{ss})", (self.snapshot(),)))
            return
        invocation.return_value(None)

    def set_state(self, state, error=""):
        self.state = state
        self.error = error
        try:
            self.bus.emit_signal(None, "/dev/linuxlab/Update", "dev.linuxlab.Update", "Changed",
                                 GLib.Variant("(a{ss})", (self.snapshot(),)))
        except GLib.Error as e:
            log("no signal: %s" % e.message)

    # ---- looking

    def look(self):
        self.check(by_hand=False)
        return True

    def check(self, by_hand):
        if self.state in ("checking", "installing"):
            return
        self.set_state("checking")
        # the network in a thread: the bus and the notifications go on meanwhile
        threading.Thread(target=lambda: GLib.idle_add(self.checked, latest(), by_hand), daemon=True).start()

    def checked(self, manifest, by_hand):
        if not manifest:
            self.set_state("offline" if self.offer is None else "available")
            return False
        if int(manifest["serial"]) <= installed_serial():
            self.offer = None
            self.set_state("current")
            return False
        self.offer = manifest
        self.set_state("available")
        if int(manifest["serial"]) != self.offered_serial and not by_hand:
            self.offered_serial = int(manifest["serial"])
            log("release %s (serial %s) is newer than serial %d" % (manifest.get("version"), manifest["serial"],
                                                                  installed_serial()))
            self.notify(say("Update of Linux Lab Helper %s", "Обновление Linux Lab Helper %s") % manifest.get("version", ""),
                        notes(manifest) or say("Fixes for the Ubuntu desktop of Linux Lab.",
                                               "Исправления для рабочего стола Ubuntu в Linux Lab."),
                        # "default" is a tap on the notification itself: the banner shows its buttons
                        # only when it is opened out, which a finger hardly does
                        ["default", say("Install", "Установить"), "install", say("Install", "Установить")],
                        resident=True)
        return False

    # ---- notifications

    def notify(self, summary, body, actions=(), resident=False):
        hints = {}
        if resident:
            hints["resident"] = GLib.Variant("b", True)
        try:
            reply = self.bus.call_sync(
                "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", "Notify",
                GLib.Variant("(susssasa{sv}i)", ("Linux Lab Helper", self.shown, "software-update-available",
                                                 summary, body, list(actions), hints, 0 if resident else -1)),
                GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 10000, None)
            self.shown = reply.unpack()[0]
        except GLib.Error as e:
            log("no notification: %s" % e.message)

    def on_action(self, _bus, _sender, _path, _interface, _signal, parameters):
        number, action = parameters.unpack()
        if number == self.shown and action in ("install", "default") and self.offer and self.state == "available":
            self.install(self.offer)

    # ---- installing

    def install(self, manifest):
        self.set_state("installing")
        self.notify(say("Updating Linux Lab Helper", "Обновляю Linux Lab Helper"),
                    say("A few minutes. Programs keep running.", "Несколько минут. Программы продолжают работать."),
                    resident=True)
        threading.Thread(target=lambda: GLib.idle_add(self.downloaded, manifest, download(manifest)), daemon=True).start()

    def downloaded(self, _manifest, result):
        path, error = result
        if error:
            log("download failed: %s" % error)
            self.done(False, say("Could not download it: %s", "Не удалось скачать: %s") % error)
            return False
        self.process = subprocess.Popen(["sudo", "-n", "bash", path, "update"], stdin=subprocess.DEVNULL,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, self.process.pid, self.finished)
        return False

    def finished(self, _pid, status):
        ok = os.waitstatus_to_exitcode(status) == 0
        self.done(ok, say("Close Linux Lab and open it again: the update works from then on.",
                          "Закройте Linux Lab и откройте снова: обновление заработает после этого.") if ok else
                  say("The log is /var/log/linuxlab-helper.log, a copy is in Downloads.",
                      "Журнал: /var/log/linuxlab-helper.log, копия лежит в «Загрузках»."))

    def done(self, ok, body):
        if ok:
            self.offer = None
            self.set_state("installed")
        else:
            self.offered_serial = 0
            self.set_state("failed", body)
        self.notify(say("Linux Lab Helper is updated", "Linux Lab Helper обновлён") if ok
                    else say("The update failed", "Обновление не удалось"), body, resident=ok)


def download(manifest):
    """The installer of @manifest in the cache, checked: (path, None), or (None, what went wrong)."""
    thing = manifest["installer"]
    # from the release of this very manifest, should a newer one appear in the meantime
    base = "https://github.com/%s/releases/download/%s/" % (REPOSITORY, manifest["tag"]) if manifest.get("tag") else LATEST
    try:
        data = fetch(base + thing["name"], int(thing["size"]))
        if len(data) != int(thing["size"]) or hashlib.sha256(data).hexdigest() != thing["sha256"]:
            raise ValueError("the installer is not the one the manifest names")
        os.makedirs(CACHE, exist_ok=True)
        path = os.path.join(CACHE, "linuxlab-helper.sh")
        with open(path + ".new", "wb") as f:
            f.write(data)
        os.replace(path + ".new", path)
    except (OSError, ValueError) as e:
        return None, str(e)
    return path, None


def enabled():
    try:
        with open("/etc/default/linuxlab-gnome") as f:
            return not any(line.strip() == "LINUXLAB_UPDATE=off" for line in f)
    except OSError:
        return True


def main(arguments):
    if arguments == ["check"]:
        manifest = latest()
        if not manifest:
            print("nothing found")
            return 1
        print("latest %s, serial %s; installed serial %d" % (manifest.get("version"), manifest["serial"], installed_serial()))
        return 0
    if arguments:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    if not enabled():
        return 0
    service = Service()
    GLib.timeout_add_seconds(FIRST_LOOK_S, lambda: service.look() and False)
    GLib.timeout_add_seconds(LOOK_EVERY_S, service.look)
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
