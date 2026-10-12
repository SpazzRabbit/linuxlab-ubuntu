#!/usr/bin/python3
"""What the tablet tells Linux through the helper app of Android: the battery, the keyboard, how it is held.

Linux Lab runs inside an Android app and sees neither the battery (Android closes
/sys/class/power_supply and its battery service to it) nor the keyboard of Android (the stock app
gives the keyboard no text field to write into, so only Latin letters come through, as key codes).
The helper app (overlay/client) has both: it reads the battery the Android way, and its touch layer
is a text field as far as the Android keyboard is concerned. It connects here, to 127.0.0.1, and
talks in lines:

  hello <version>                     once
  battery <percent> <status> <plugged> <seconds to full or -1>
                                      status is Android's: 2 charging, 3 discharging, 4 not charging, 5 full
  text <JSON string>                  text to insert
  preedit <JSON string>               text being composed (a word under completion, a syllable of
                                      Chinese or Japanese); an empty one clears it
  delete <before> <after>             characters to remove around the cursor
  key <name>                          BackSpace, Return, Tab, Escape, Left, Right, Up, Down, Delete,
                                      Home, End, Page_Up, Page_Down
  rotation <0|90|180|270>             how far the picture has to be turned clockwise to stand upright
                                      for the person holding the tablet

and hears "keyboard <0|1|2>": hide, show, toggle the keyboard of Android, and "ime <0|1>": which
keyboard is chosen, Linux (0) or Android (1), so that the app can show it.

Which keyboard types is Linux Lab's own setting, "Screen keyboard" in its settings: Android's or Linux's.
Honor's LXQt follows it, and the app mirrors it into a file of the home directory of Linux,
~/.config/linuxlab-use-android-ime ("1" for Android, no file for Linux), which is read here. With the
keyboard of Linux GNOME brings up its own screen keyboard. Before 0.9 the helper app had a switch of its
own for this, which GNOME followed and LXQt did not: two settings, and Linux Lab's one said nothing to
GNOME.

The battery becomes org.freedesktop.UPower on the system bus: GNOME shows the charge in the top bar and
in Settings exactly as on a laptop. The text goes to the extension of the shell (extension/), which
hands it to the program in focus the way GNOME's own screen keyboard does, in any language, since it
is text and not key codes. The extension also says when a text field wants a keyboard.

The window of Linux Lab is always a landscape one: Android turns it upside down with the tablet and
never upright. So when the tablet is held upright the helper says so, and the monitor of GNOME is
turned inside the window (the bridge turns the picture and the touches back, see llbridge.c). The
switch is GNOME's own: "Auto Rotate" in the quick settings, the setting orientation-lock. Upright,
the keyboard is GNOME's: the one of Android would lie on its side.

Started by gnome-session.sh. Any program on the tablet can connect to the port and type, as it can
send touches to lltouch: both listen on 127.0.0.1 only, and neither asks who is there.
"""
import json
import os
import socket
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import Gio, GLib

PORT = 54399
GREETING = b"LLANDROID 1\n"
# Linux Lab's setting "Screen keyboard", as the app writes it into Linux (see above)
ANDROID_IME_FLAG = os.path.expanduser("~/.config/linuxlab-use-android-ime")

UPOWER = "org.freedesktop.UPower"
UPOWER_PATH = "/org/freedesktop/UPower"
DEVICE = UPOWER + ".Device"
DISPLAY_DEVICE = UPOWER_PATH + "/devices/DisplayDevice"
BATTERY = UPOWER_PATH + "/devices/battery_android"
PROPERTIES = "org.freedesktop.DBus.Properties"

SHELL = "org.gnome.Shell"
INPUT = "dev.linuxlab.Input"
INPUT_PATH = "/dev/linuxlab/Input"

MUTTER = "org.gnome.Mutter.DisplayConfig"
MUTTER_PATH = "/org/gnome/Mutter/DisplayConfig"
TOUCHSCREEN = "org.gnome.settings-daemon.peripherals.touchscreen"
# degrees the picture is to be turned clockwise, and the transform of Mutter that does it
TRANSFORMS = {0: 0, 90: 3, 180: 2, 270: 1}

# X keysyms of the keys the Android keyboard sends as keys
KEYS = {
    "BackSpace": 0xFF08,
    "Tab": 0xFF09,
    "Return": 0xFF0D,
    "Escape": 0xFF1B,
    "Home": 0xFF50,
    "Left": 0xFF51,
    "Up": 0xFF52,
    "Right": 0xFF53,
    "Down": 0xFF54,
    "Page_Up": 0xFF55,
    "Page_Down": 0xFF56,
    "End": 0xFF57,
    "Delete": 0xFFFF,
}

# UPower's states, and what Android's are in them
UNKNOWN, CHARGING, DISCHARGING, FULL, PENDING_CHARGE = 0, 1, 2, 4, 5
ANDROID_CHARGING, ANDROID_DISCHARGING, ANDROID_NOT_CHARGING, ANDROID_FULL = 2, 3, 4, 5
TYPE_BATTERY = 2

u32 = dbus.UInt32


def say(*words):
    print("[llandroid]", *words, flush=True)


# ---------------------------------------------------------------------------------------------
# the battery


class Properties(dbus.service.Object):
    def __init__(self, bus, path, interface, values):
        super().__init__(bus, path)
        self.path = path
        self.interface = interface
        self.values = values

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if interface == self.interface and name in self.values:
            return self.values[name]
        raise dbus.exceptions.DBusException(
            "No property %s.%s" % (interface, name), name="org.freedesktop.DBus.Error.UnknownProperty"
        )

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.values if interface == self.interface else {}, signature="sv")

    @dbus.service.signal(PROPERTIES, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def update(self, values):
        changed = {name: value for name, value in values.items() if self.values.get(name) != value}
        if changed:
            self.values.update(changed)
            self.PropertiesChanged(self.interface, dbus.Dictionary(changed, signature="sv"), dbus.Array([], signature="s"))


class Device(Properties):
    def __init__(self, bus, path):
        super().__init__(
            bus,
            path,
            DEVICE,
            {
                "NativePath": "android",
                "Vendor": "",
                "Model": "",
                "Serial": "",
                "UpdateTime": dbus.UInt64(0),
                "Type": u32(TYPE_BATTERY),
                "PowerSupply": True,
                "HasHistory": False,
                "HasStatistics": False,
                "Online": False,
                "Energy": dbus.Double(0),
                "EnergyEmpty": dbus.Double(0),
                "EnergyFull": dbus.Double(0),
                "EnergyFullDesign": dbus.Double(0),
                "EnergyRate": dbus.Double(0),
                "Voltage": dbus.Double(0),
                "ChargeCycles": dbus.Int32(-1),
                "Luminosity": dbus.Double(0),
                "TimeToEmpty": dbus.Int64(0),
                "TimeToFull": dbus.Int64(0),
                "Percentage": dbus.Double(0),
                "Temperature": dbus.Double(0),
                "IsPresent": False,
                "State": u32(UNKNOWN),
                "IsRechargeable": True,
                "Capacity": dbus.Double(100),
                "Technology": u32(1),  # lithium ion
                "WarningLevel": u32(1),  # none
                "BatteryLevel": u32(1),  # none: the percentage is known
                "IconName": "battery-missing-symbolic",
            },
        )

    @dbus.service.method(DEVICE)
    def Refresh(self):
        pass

    @dbus.service.method(DEVICE, in_signature="suu", out_signature="a(udu)")
    def GetHistory(self, kind, timespan, resolution):
        return dbus.Array([], signature="(udu)")

    @dbus.service.method(DEVICE, in_signature="s", out_signature="a(dd)")
    def GetStatistics(self, kind):
        return dbus.Array([], signature="(dd)")


class Power(Properties):
    """org.freedesktop.UPower with one battery: the one of the tablet."""

    def __init__(self, bus):
        super().__init__(
            bus,
            UPOWER_PATH,
            UPOWER,
            {"DaemonVersion": "1.90.9", "OnBattery": False, "LidIsClosed": False, "LidIsPresent": False},
        )
        self.display = Device(bus, DISPLAY_DEVICE)
        self.battery = Device(bus, BATTERY)
        self.known = False

    @dbus.service.method(UPOWER, out_signature="ao")
    def EnumerateDevices(self):
        return dbus.Array([dbus.ObjectPath(BATTERY)] if self.known else [], signature="o")

    @dbus.service.method(UPOWER, out_signature="o")
    def GetDisplayDevice(self):
        return dbus.ObjectPath(DISPLAY_DEVICE)

    @dbus.service.method(UPOWER, out_signature="s")
    def GetCriticalAction(self):
        return "PowerOff"

    @dbus.service.signal(UPOWER, signature="o")
    def DeviceAdded(self, path):
        pass

    @dbus.service.signal(UPOWER, signature="o")
    def DeviceRemoved(self, path):
        pass

    def set(self, percent, status, plugged, to_full):
        percent = max(0, min(100, percent))
        if status == ANDROID_FULL or (plugged and percent >= 100):
            state = FULL
        elif status == ANDROID_CHARGING:
            state = CHARGING
        elif status == ANDROID_NOT_CHARGING and plugged:
            state = PENDING_CHARGE
        elif status in (ANDROID_DISCHARGING, ANDROID_NOT_CHARGING):
            state = DISCHARGING
        else:
            state = UNKNOWN
        if state == FULL:
            icon = "battery-level-100-charged-symbolic"
        else:
            icon = "battery-level-%d%s-symbolic" % (percent // 10 * 10, "-charging" if state == CHARGING else "")
        warning = 1
        if state == DISCHARGING:
            warning = 4 if percent <= 5 else 3 if percent <= 15 else 1
        values = {
            "IsPresent": True,
            "Online": bool(plugged),
            "Percentage": dbus.Double(percent),
            "State": u32(state),
            "TimeToFull": dbus.Int64(to_full if state == CHARGING and to_full > 0 else 0),
            "WarningLevel": u32(warning),
            "IconName": icon,
            "UpdateTime": dbus.UInt64(GLib.get_real_time() // 1000000),
        }
        self.battery.update(values)
        self.display.update(values)
        self.update({"OnBattery": state == DISCHARGING})
        if not self.known:
            self.known = True
            self.DeviceAdded(dbus.ObjectPath(BATTERY))


# ---------------------------------------------------------------------------------------------
# the keyboard


class Shell:
    """The extension of GNOME Shell: it knows the program in focus and can hand it text."""

    def __init__(self, bus, on_wanted, on_appeared):
        self.bus = bus
        bus.add_signal_receiver(lambda state: on_wanted(int(state)), "Wanted", INPUT, None, INPUT_PATH)
        bus.add_signal_receiver(
            lambda name, old, new: new and on_appeared(),
            "NameOwnerChanged",
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            arg0=SHELL,
        )

    def call(self, method, *arguments, failed=None):
        failed = failed or self.failed
        try:
            target = dbus.Interface(self.bus.get_object(SHELL, INPUT_PATH, introspect=False), INPUT)
            getattr(target, method)(*arguments, reply_handler=lambda *_: None, error_handler=failed)
        except dbus.exceptions.DBusException as error:
            failed(error)

    @staticmethod
    def failed(error):
        say("the shell did not take it:", error.get_dbus_message() or error)


class Monitor:
    """The monitor of GNOME, turned the way the tablet is held unless "Auto Rotate" is off."""

    def __init__(self, bus, on_turned):
        self.bus = bus
        self.on_turned = on_turned
        self.wanted = None  # degrees the helper asked for; None while it has not said
        self.transform = 0  # what the monitor has now
        self.settings = None
        source = Gio.SettingsSchemaSource.get_default()
        if source and source.lookup(TOUCHSCREEN, True):
            self.settings = Gio.Settings.new(TOUCHSCREEN)
            self.settings.connect("changed::orientation-lock", lambda *_: self.apply())
        # whoever turns the monitor (Settings can too), the keyboard has to follow
        bus.add_signal_receiver(lambda: self.look(None), "MonitorsChanged", MUTTER, None, MUTTER_PATH)
        self.look(None)

    def upright(self):
        return self.transform in (1, 3)

    def set(self, degrees):
        self.wanted = degrees
        self.apply()

    def gone(self):
        """The helper left: a tablet nobody watches is taken to lie the way its window does."""
        self.wanted = None
        GLib.timeout_add(3000, lambda: self.wanted is None and self.set(0) and False)

    def apply(self):
        if self.wanted is not None and not (self.settings and self.settings.get_boolean("orientation-lock")):
            self.look(TRANSFORMS[self.wanted])

    def look(self, target):
        """Reads how the monitor is turned; with @target, turns it that way."""

        def got(serial, monitors, logical, _properties):
            if not monitors or not logical:
                return
            _x, _y, scale, transform = logical[0][:4]
            if int(transform) != self.transform:
                self.transform = int(transform)
                self.on_turned()
            mode = next((mode[0] for mode in monitors[0][1] if mode[6].get("is-current")), None)
            if target is None or target == self.transform or mode is None:
                return
            # method 1: for now, not written to monitors.xml; the scale and the mode stay
            config = [(0, 0, float(scale), target, True, [(str(monitors[0][0][0]), str(mode), {})])]
            self.bus.call_async(MUTTER, MUTTER_PATH, MUTTER, "ApplyMonitorsConfig", "uua(iiduba(ssa{sv}))a{sv}",
                                (int(serial), 1, config, {}), lambda *_: say("the monitor is turned, transform", target),
                                self.failed)

        self.bus.call_async(MUTTER, MUTTER_PATH, MUTTER, "GetCurrentState", "", (), got, self.failed)

    @staticmethod
    def failed(error):
        say("the monitor did not answer:", error.get_dbus_message() or error)


class Tablet:
    """The helper app, one at a time."""

    def __init__(self, power, shell, monitor):
        self.power = power
        self.shell = shell
        self.monitor = monitor
        self.sensor = False  # the helper says how the tablet is held
        self.client = None
        self.watch = 0
        self.pending = b""
        self.keyboard = False  # the Android keyboard is to be used: it is chosen and the helper app is here
        self.chosen = self.android_chosen()
        GLib.timeout_add_seconds(2, self.check_choice)
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", PORT))
        self.server.listen(2)
        self.server.setblocking(False)
        GLib.io_add_watch(self.server.fileno(), GLib.IO_IN, self.accept)

    def accept(self, *_):
        try:
            client, _address = self.server.accept()
        except OSError:
            return True
        self.drop()  # the newest wins
        client.setblocking(False)
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.client = client
        self.pending = b""
        self.watch = GLib.io_add_watch(client.fileno(), GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self.readable)
        self.send(GREETING)
        self.send(b"ime %d\n" % self.chosen)
        say("the helper app is here")
        return True

    def drop(self):
        if not self.client:
            return
        GLib.source_remove(self.watch)
        self.client.close()
        self.client = None
        self.gone()

    def gone(self):
        self.sensor = False
        self.monitor.gone()
        self.set_keyboard(False)
        say("the helper app is gone")

    def send(self, data):
        if self.client:
            try:
                self.client.send(data)
            except OSError:
                pass

    def readable(self, _fd, condition):
        try:
            data = self.client.recv(65536) if condition & GLib.IO_IN else b""
        except BlockingIOError:
            return True
        except OSError:
            data = b""
        if not data:
            self.watch = 0
            self.client.close()
            self.client = None
            self.gone()
            return False
        self.pending += data
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            try:
                self.line(line.decode("utf-8", "replace"))
            except (ValueError, IndexError) as error:
                say("a line that makes no sense:", repr(line[:80]), error)
        if len(self.pending) > 1 << 20:
            self.pending = b""
        return True

    def line(self, line):
        word, _, rest = line.partition(" ")
        if word == "battery":
            percent, status, plugged, to_full = (int(part) for part in rest.split()[:4])
            if self.power:
                self.power.set(percent, status, plugged, to_full)
        elif word == "text":
            text = json.loads(rest)
            if text:
                self.shell.call("Commit", dbus.String(text))
        elif word == "preedit":
            self.shell.call("Preedit", dbus.String(json.loads(rest)))
        elif word == "delete":
            before, after = (max(0, min(1000, int(part))) for part in rest.split()[:2])
            self.shell.call("Delete", u32(before), u32(after))
        elif word == "key":
            if rest in KEYS:
                self.shell.call("Key", u32(KEYS[rest]))
        elif word == "hello":
            # a helper app before 0.9 says which keyboard it wants: Linux Lab's setting decides now
            self.set_keyboard(self.chosen)
        elif word == "rotation":
            degrees = int(rest)
            if degrees in TRANSFORMS:
                self.monitor.set(degrees)
                if not self.sensor:
                    self.sensor = True
                    self.tell_shell()

    def set_keyboard(self, active):
        self.keyboard = active
        self.tell_shell()

    @staticmethod
    def android_chosen():
        """Linux Lab's setting says the keyboard of Android."""
        try:
            with open(ANDROID_IME_FLAG) as f:
                return f.read().strip().lower() in ("1", "true", "yes", "on")
        except OSError:
            return False

    def check_choice(self):
        chosen = self.android_chosen()
        if chosen != self.chosen:
            self.chosen = chosen
            say("the keyboard chosen in Linux Lab:", "Android" if chosen else "Linux")
            if self.client:
                if not chosen:
                    self.send(b"keyboard 0\n")
                self.send(b"ime %d\n" % chosen)
                self.set_keyboard(chosen)
        return True

    def android_keyboard(self):
        """The keyboard of Android is to be used: not upright, where it would lie on its side."""
        return self.keyboard and not self.monitor.upright()

    def tell_shell(self, attempt=0):
        """Whose keyboard it is, and whether there is a sensor for "Auto Rotate".

        The shell may still be starting, its extension comes last: ask again.
        """

        def failed(error):
            if attempt < 30:
                GLib.timeout_add(1000, lambda: self.tell_shell(attempt + 1) and False)
            else:
                Shell.failed(error)

        self.shell.call("SetAndroidKeyboard", dbus.Boolean(self.android_keyboard()), failed=failed)
        self.shell.call("SetSensor", dbus.Boolean(self.sensor), failed=lambda error: None)

    def shell_appeared(self):
        # a shell that was started again knows nothing, and its monitor is turned as the file says
        self.tell_shell()
        GLib.timeout_add(2000, lambda: self.monitor.apply() and False)

    def turned(self):
        if self.keyboard and self.monitor.upright():
            self.send(b"keyboard 0\n")
        self.tell_shell()

    def wanted(self, state):
        if self.android_keyboard():
            self.send(b"keyboard %d\n" % state)


def main():
    DBusGMainLoop(set_as_default=True)
    power = None
    try:
        system = dbus.SystemBus()
        power = Power(system)
        if system.request_name(UPOWER, dbus.bus.NAME_FLAG_DO_NOT_QUEUE) != dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER:
            say("a real UPower runs: the battery is its business")
            power = None
    except dbus.exceptions.DBusException as error:
        say("no system bus, so no battery:", error.get_dbus_message())
    tablet = None

    def wanted(state):
        if tablet:
            tablet.wanted(state)

    def appeared():
        if tablet:
            tablet.shell_appeared()

    def turned():
        if tablet:
            tablet.turned()

    session = dbus.SessionBus()
    shell = Shell(session, wanted, appeared)
    try:
        tablet = Tablet(power, shell, Monitor(session, turned))
    except OSError as error:
        say("cannot listen on 127.0.0.1:%d:" % PORT, error)
        return 1
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
