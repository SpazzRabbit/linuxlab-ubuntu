#!/usr/bin/python3
"""Starts the programs of the XDG autostart directories.

On a PC the user instance of systemd does this for a GNOME session. Linux Lab has no systemd,
so gnome-session.sh calls this once the shell is up.
"""
import os

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GioUnix", "2.0")
from gi.repository import Gio, GioUnix, GLib  # noqa: E402


def entries():
    """Desktop files by name; a file in the user's directory hides the system one of the same name."""
    found = {}
    directories = [GLib.get_user_config_dir()] + list(GLib.get_system_config_dirs())
    for directory in directories:
        directory = os.path.join(directory, "autostart")
        try:
            names = sorted(os.listdir(directory))
        except OSError:
            continue
        for name in names:
            if name.endswith(".desktop"):
                found.setdefault(name, os.path.join(directory, name))
    return found


def condition_holds(condition):
    """AutostartCondition of gnome-session: only the kinds met in practice."""
    kind, _, rest = condition.partition(" ")
    rest = rest.strip()
    if kind == "GSettings":
        schema, _, key = rest.partition(" ")
        source = Gio.SettingsSchemaSource.get_default()
        found = source.lookup(schema, True) if source else None
        return bool(found and found.has_key(key) and Gio.Settings.new(schema).get_boolean(key))
    path = rest if os.path.isabs(rest) else os.path.join(GLib.get_user_config_dir(), rest)
    if kind == "if-exists":
        return os.path.exists(path)
    if kind == "unless-exists":
        return not os.path.exists(path)
    return True


# Entries that must not run here.
SKIPPED = {
    # It loads the X11 modules into the PulseAudio that Honor starts, and they hold on to the X
    # server of the session. When that goes, PulseAudio says "X11 I/O error exit handler called" in
    # the output of the container, and the app of Linux Lab takes any line with " exit" in it for
    # the end and stops playing sound. (install.sh sets aside the script that does the same when
    # Xwayland starts.)
    "pulseaudio.desktop",
}


def wanted(info):
    # the parts of Honor's LXQt session are marked OnlyShowIn=LXQt and drop out here
    if not info.get_show_in(None):
        return False
    # started in phases by gnome-session on a PC; here gnome-session.sh starts what can work
    if info.has_key("X-GNOME-Autostart-Phase") or info.get_boolean("X-GNOME-HiddenUnderSystemd"):
        return False
    if info.has_key("X-GNOME-Autostart-enabled") and not info.get_boolean("X-GNOME-Autostart-enabled"):
        return False
    if info.has_key("AutostartCondition") and not condition_holds(info.get_string("AutostartCondition")):
        return False
    return True


def main():
    for name, path in sorted(entries().items()):
        try:
            info = GioUnix.DesktopAppInfo.new_from_filename(path)  # None when hidden or TryExec is missing
        except TypeError:
            info = None
        if info is None or name in SKIPPED or not wanted(info):
            continue
        try:
            info.launch([], None)
            print("started", name, flush=True)
        except GLib.Error as error:
            print("failed", name, error.message, flush=True)


if __name__ == "__main__":
    main()
