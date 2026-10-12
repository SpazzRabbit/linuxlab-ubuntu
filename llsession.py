#!/usr/bin/python3
"""The part of gnome-session that GNOME Shell and the programs talk to.

On a PC gnome-session owns org.gnome.SessionManager: "Power Off", "Restart" and "Log Out" in the
system menu are requests to it, programs register with it and tell it about unsaved work. Here
gnome-session.sh does the starting, and nobody answered: the menu entries did nothing.

What the three requests mean inside Linux Lab:

  Power Off          the desktop and all its programs end. What stays is a black screen that
                     waits for a touch to start the desktop again (lloff.py says why the
                     container itself is kept). The Android app is closed the Android way.
  Restart, Log Out   the desktop starts anew. There is neither a firmware to go through nor a login
                     screen to return to.

The shell shows its usual dialog with the countdown first, and lists the programs that asked not to
be interrupted. Programs that registered get the usual "the session is ending" call.

The property Renderer names the graphics of the session, as gnome-session finds it out with a test
program on a PC. Settings shows it under System, About. Without it Settings runs its own test without
the setting of the session that picks the GPU (gnome-session.sh), gets the renderer of the CPU, and
says "Software Rendering". Here the same test runs with the setting, once the shell is up.

Started by gnome-session.sh, which is told what to do through a file and a signal (see there).
"""
import os
import signal
import subprocess
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NAME = "org.gnome.SessionManager"
ROOT = "/org/gnome/SessionManager"
PROPERTIES = "org.freedesktop.DBus.Properties"
CLIENT = NAME + ".Client"
CLIENT_PRIVATE = NAME + ".ClientPrivate"
INHIBITOR = NAME + ".Inhibitor"
PRESENCE = NAME + ".Presence"
DIALOG = NAME + ".EndSessionDialog"
DIALOG_PATH = ROOT + "/EndSessionDialog"

# the dialogs of the shell
ASK_LOGOUT, ASK_POWER_OFF, ASK_RESTART = 0, 1, 2
COUNTDOWN = 60  # seconds, as on a PC
# answers to "can you"
UNAVAILABLE, AVAILABLE = 0, 3
INHIBIT_LOGOUT = 1

u32 = dbus.UInt32


def say(*words):
    print("[llsession]", *words, flush=True)


class Properties(dbus.service.Object):
    """An object with read-only properties."""

    properties = {}  # {interface: {name: value}}

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        try:
            return self.properties[interface][name]
        except KeyError:
            raise dbus.exceptions.DBusException(
                "No property %s.%s" % (interface, name), name="org.freedesktop.DBus.Error.UnknownProperty"
            )

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.properties.get(interface, {}), signature="sv")

    @dbus.service.signal(PROPERTIES, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


class Client(dbus.service.Object):
    """A program that wants to hear when the session ends."""

    def __init__(self, bus, path, app_id, startup_id, owner, pid):
        super().__init__(bus, path)
        self.path = path
        self.app_id = app_id
        self.startup_id = startup_id
        self.owner = owner
        self.pid = pid

    @dbus.service.method(CLIENT, out_signature="s")
    def GetAppId(self):
        return self.app_id

    @dbus.service.method(CLIENT, out_signature="s")
    def GetStartupId(self):
        return self.startup_id

    @dbus.service.method(CLIENT, out_signature="u")
    def GetRestartStyleHint(self):
        return u32(0)

    @dbus.service.method(CLIENT, out_signature="u")
    def GetUnixProcessId(self):
        return u32(self.pid)

    @dbus.service.method(CLIENT, out_signature="u")
    def GetStatus(self):
        return u32(1)  # registered

    @dbus.service.method(CLIENT)
    def Stop(self):
        pass

    @dbus.service.method(CLIENT_PRIVATE, in_signature="bs")
    def EndSessionResponse(self, is_ok, reason):
        pass

    @dbus.service.signal(CLIENT_PRIVATE, signature="u")
    def QueryEndSession(self, flags):
        pass

    @dbus.service.signal(CLIENT_PRIVATE, signature="u")
    def EndSession(self, flags):
        pass

    @dbus.service.signal(CLIENT_PRIVATE, signature="")
    def CancelEndSession(self):
        pass


class Inhibitor(dbus.service.Object):
    """A reason not to end the session, or not to go idle: unsaved work, a film, a copy in progress."""

    def __init__(self, bus, path, app_id, xid, reason, flags, owner):
        super().__init__(bus, path)
        self.path = path
        self.app_id = app_id
        self.xid = xid
        self.reason = reason
        self.flags = int(flags)
        self.owner = owner

    @dbus.service.method(INHIBITOR, out_signature="s")
    def GetAppId(self):
        return self.app_id

    @dbus.service.method(INHIBITOR, out_signature="o")
    def GetClientId(self):
        return dbus.ObjectPath("/")

    @dbus.service.method(INHIBITOR, out_signature="s")
    def GetReason(self):
        return self.reason

    @dbus.service.method(INHIBITOR, out_signature="u")
    def GetFlags(self):
        return u32(self.flags)

    @dbus.service.method(INHIBITOR, out_signature="u")
    def GetToplevelXid(self):
        return u32(self.xid)


class Presence(Properties):
    def __init__(self, bus):
        super().__init__(bus, ROOT + "/Presence")
        self.properties = {PRESENCE: {"status": u32(0), "status-text": ""}}

    @dbus.service.method(PRESENCE, in_signature="u")
    def SetStatus(self, status):
        self.properties[PRESENCE]["status"] = u32(status)
        self.StatusChanged(u32(status))

    @dbus.service.method(PRESENCE, in_signature="s")
    def SetStatusText(self, text):
        self.properties[PRESENCE]["status-text"] = text
        self.StatusTextChanged(text)

    @dbus.service.signal(PRESENCE, signature="u")
    def StatusChanged(self, status):
        pass

    @dbus.service.signal(PRESENCE, signature="s")
    def StatusTextChanged(self, text):
        pass


class Manager(Properties):
    def __init__(self, bus, end):
        super().__init__(bus, ROOT)
        self.bus = bus
        self.end = end
        self.clients = {}
        self.inhibitors = {}
        self.counter = 0
        self.asking = False
        self.ending = False
        self.properties = {
            NAME: {"SessionName": "ubuntu", "Renderer": "", "SessionIsActive": True, "InhibitedActions": u32(0)}
        }
        bus.add_signal_receiver(
            self.peer_changed, "NameOwnerChanged", "org.freedesktop.DBus", "org.freedesktop.DBus", "/org/freedesktop/DBus"
        )
        for name, action in (("ConfirmedLogout", "restart"), ("ConfirmedReboot", "restart"), ("ConfirmedShutdown", "poweroff")):
            bus.add_signal_receiver(lambda *_, action=action: self.confirmed(action), name, DIALOG, None, DIALOG_PATH)
        for name in ("Canceled", "Closed"):
            bus.add_signal_receiver(self.closed, name, DIALOG, None, DIALOG_PATH)
        self.renderer_tries = 0
        GLib.timeout_add_seconds(5, self.find_renderer)

    # --- the graphics

    def find_renderer(self):
        """Asks the test program of Settings, until the display of the shell is there."""
        self.renderer_tries += 1
        environment = dict(os.environ, WAYLAND_DISPLAY=os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
        try:
            found = subprocess.run(
                ["/usr/libexec/gnome-control-center-print-renderer"], env=environment,
                capture_output=True, text=True, timeout=15,
            ).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            found = ""
        if not found:
            return self.renderer_tries < 12
        # "zink Vulkan 1.4(Adreno (TM) 829 (MESA_TURNIP))": the GPU is the part in the brackets
        if "Adreno" in found:
            found = "Qualcomm " + found[found.index("Adreno"):].split(" (MESA", 1)[0].rstrip(")")
            found = " ".join(found.replace("(TM)", "").split())  # Settings would show the sign
        say("graphics:", found)
        self.properties[NAME]["Renderer"] = found
        self.PropertiesChanged(NAME, dbus.Dictionary({"Renderer": found}, signature="sv"), dbus.Array([], signature="s"))
        return False

    # --- programs

    def next_path(self, kind):
        self.counter += 1
        return "%s/%s%d" % (ROOT, kind, self.counter)

    def peer_changed(self, name, old, new):
        if new or not name.startswith(":"):
            return
        for path in [path for path, client in self.clients.items() if client.owner == name]:
            self.forget_client(path)
        for cookie in [cookie for cookie, inhibitor in self.inhibitors.items() if inhibitor.owner == name]:
            self.forget_inhibitor(cookie)

    def forget_client(self, path):
        client = self.clients.pop(path, None)
        if client:
            client.remove_from_connection()
            self.ClientRemoved(dbus.ObjectPath(path))

    def forget_inhibitor(self, cookie):
        inhibitor = self.inhibitors.pop(cookie, None)
        if inhibitor:
            inhibitor.remove_from_connection()
            self.InhibitorRemoved(dbus.ObjectPath(inhibitor.path))
            self.inhibited_changed()

    def inhibited_changed(self):
        flags = 0
        for inhibitor in self.inhibitors.values():
            flags |= inhibitor.flags
        if self.properties[NAME]["InhibitedActions"] != flags:
            self.properties[NAME]["InhibitedActions"] = u32(flags)
            self.PropertiesChanged(
                NAME, dbus.Dictionary({"InhibitedActions": u32(flags)}, signature="sv"), dbus.Array([], signature="s")
            )

    @dbus.service.method(NAME, in_signature="ss", out_signature="o", sender_keyword="sender")
    def RegisterClient(self, app_id, startup_id, sender=None):
        try:
            pid = int(self.bus.get_unix_process_id(sender)) if hasattr(self.bus, "get_unix_process_id") else 0
        except dbus.exceptions.DBusException:
            pid = 0
        path = self.next_path("Client")
        self.clients[path] = Client(self.bus, path, app_id, startup_id, sender, pid)
        self.ClientAdded(dbus.ObjectPath(path))
        return dbus.ObjectPath(path)

    @dbus.service.method(NAME, in_signature="o")
    def UnregisterClient(self, path):
        self.forget_client(str(path))

    @dbus.service.method(NAME, in_signature="susu", out_signature="u", sender_keyword="sender")
    def Inhibit(self, app_id, toplevel_xid, reason, flags, sender=None):
        path = self.next_path("Inhibitor")
        cookie = self.counter
        self.inhibitors[cookie] = Inhibitor(self.bus, path, app_id, int(toplevel_xid), reason, flags, sender)
        self.InhibitorAdded(dbus.ObjectPath(path))
        self.inhibited_changed()
        return u32(cookie)

    @dbus.service.method(NAME, in_signature="u")
    def Uninhibit(self, cookie):
        self.forget_inhibitor(int(cookie))

    @dbus.service.method(NAME, in_signature="u", out_signature="b")
    def IsInhibited(self, flags):
        return any(inhibitor.flags & int(flags) for inhibitor in self.inhibitors.values())

    @dbus.service.method(NAME, out_signature="ao")
    def GetInhibitors(self):
        return dbus.Array([dbus.ObjectPath(i.path) for i in self.inhibitors.values()], signature="o")

    @dbus.service.method(NAME, out_signature="ao")
    def GetClients(self):
        return dbus.Array([dbus.ObjectPath(path) for path in self.clients], signature="o")

    @dbus.service.method(NAME, in_signature="ss")
    def Setenv(self, variable, value):
        pass

    @dbus.service.method(NAME, in_signature="i", out_signature="s")
    def GetLocale(self, category):
        return os.environ.get("LANG", "")

    @dbus.service.method(NAME, in_signature="sb")
    def InitializationError(self, message, fatal):
        say("a program reports:", message)

    @dbus.service.method(NAME, in_signature="s", out_signature="b")
    def IsAutostartConditionHandled(self, condition):
        return False

    @dbus.service.method(NAME, out_signature="b")
    def IsSessionRunning(self):
        return True

    # --- the end of the session

    @dbus.service.method(NAME, out_signature="u")
    def CanShutdown(self):
        return u32(AVAILABLE)

    @dbus.service.method(NAME, out_signature="u")
    def CanReboot(self):
        return u32(AVAILABLE)

    @dbus.service.method(NAME, out_signature="u")
    def CanSuspend(self):
        return u32(UNAVAILABLE)  # Android puts the tablet to sleep

    @dbus.service.method(NAME)
    def Shutdown(self):
        self.ask(ASK_POWER_OFF)

    @dbus.service.method(NAME)
    def Reboot(self):
        self.ask(ASK_RESTART)

    @dbus.service.method(NAME, in_signature="u")
    def Logout(self, mode):
        if int(mode) == 0:
            self.ask(ASK_LOGOUT)
        else:  # without asking
            self.confirmed("restart")

    @dbus.service.method(NAME)
    def RequestShutdown(self):
        self.confirmed("poweroff")

    @dbus.service.method(NAME)
    def RequestReboot(self):
        self.confirmed("restart")

    def ask(self, what):
        """Has the shell show its dialog; the answer comes as a signal."""
        if self.asking or self.ending:
            return
        self.asking = True
        blockers = dbus.Array(
            [dbus.ObjectPath(i.path) for i in self.inhibitors.values() if i.flags & INHIBIT_LOGOUT], signature="o"
        )

        def failed(error):
            self.asking = False
            say("the shell could not ask:", error)

        dialog = dbus.Interface(self.bus.get_object("org.gnome.Shell", DIALOG_PATH), DIALOG)
        dialog.Open(
            u32(what), u32(0), u32(COUNTDOWN), blockers, reply_handler=lambda *_: None, error_handler=failed, timeout=600
        )

    def closed(self, *_):
        self.asking = False

    def confirmed(self, action):
        if self.ending:
            return
        self.ending = True
        say("the session ends:", action)
        for client in list(self.clients.values()):
            client.EndSession(u32(0))
        # a moment for the programs to save what they keep
        GLib.timeout_add(1500, self.over, action)

    def over(self, action):
        self.SessionOver()
        self.end(action)
        return False

    @dbus.service.signal(NAME, signature="o")
    def ClientAdded(self, path):
        pass

    @dbus.service.signal(NAME, signature="o")
    def ClientRemoved(self, path):
        pass

    @dbus.service.signal(NAME, signature="o")
    def InhibitorAdded(self, path):
        pass

    @dbus.service.signal(NAME, signature="o")
    def InhibitorRemoved(self, path):
        pass

    @dbus.service.signal(NAME, signature="")
    def SessionRunning(self):
        pass

    @dbus.service.signal(NAME, signature="")
    def SessionOver(self):
        pass


def main():
    session = int(os.environ.get("LINUXLAB_SESSION_PID", "0"))
    wish = os.environ.get("LINUXLAB_SESSION_END", "")

    def end(action):
        if not session or not wish:
            say("nobody to tell that the session ends")
            return
        with open(wish, "w") as file:
            file.write(action + "\n")
        os.kill(session, signal.SIGTERM)

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    manager = Manager(bus, end)
    Presence(bus)
    if bus.request_name(NAME, dbus.bus.NAME_FLAG_DO_NOT_QUEUE) != dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER:
        say("the name is taken: a real gnome-session runs")
        return 0
    manager.SessionRunning()
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
