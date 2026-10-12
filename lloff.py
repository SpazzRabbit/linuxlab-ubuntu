#!/usr/bin/python3
"""The screen of a Linux Lab that was powered off: black, with a line that says how to switch it on.

"Power Off" ends the desktop and everything it runs. Ending the container as well would leave the
Android app open on a dead black screen: the app starts Linux once in the life of its window, and
tapping its icon again brings the same black screen back. Nothing from inside can close the app
either. So the container stays, with nothing in it but Honor's X server and sound server and this
window. A touch, a click or a key ends it, and startlxqt starts the desktop anew. To be rid of
Linux Lab altogether, the app is closed the Android way, as always.

Run by startlxqt on Honor's X display. Exit code 0: switch on.
"""
import os
import sys

import gi

gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

TEXTS = {
    "ru": ("Linux Lab выключен", "Коснитесь экрана, чтобы включить"),
    "en": ("Linux Lab is off", "Touch the screen to switch it on"),
}


def language():
    """The language the user chose for the desktop (see gnome-session.sh), not the one of Honor's script."""
    try:
        with open(os.path.expanduser("~/.pam_environment")) as file:
            for line in file:
                if line.startswith("LANG") and not line.startswith("LANGUAGE"):
                    value = line.replace("=", " ").split()[-1].replace("DEFAULT", "").strip('="')
                    return value[:2]
    except OSError:
        pass
    return os.environ.get("LANG", "en")[:2]


def main():
    title, hint = TEXTS.get(language(), TEXTS["en"])
    screen = Gdk.Screen.get_default()
    if screen is None:
        return 1
    style = Gtk.CssProvider()
    style.load_from_data(b"window { background: #000; } label { color: #8a8a8a; }")
    Gtk.StyleContext.add_provider_for_screen(screen, style, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    window = Gtk.Window()
    window.set_decorated(False)
    area = Gdk.Display.get_default().get_monitor(0).get_geometry()
    window.set_default_size(area.width, area.height)
    window.move(0, 0)
    label = Gtk.Label()
    label.set_markup(
        '<span size="xx-large">%s</span>\n\n<span size="large">%s</span>'
        % (GLib.markup_escape_text(title), GLib.markup_escape_text(hint))
    )
    label.set_justify(Gtk.Justification.CENTER)
    window.add(label)
    window.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.TOUCH_MASK | Gdk.EventMask.KEY_PRESS_MASK)

    armed = []

    def wake(*_):
        if armed:
            Gtk.main_quit()
        return True

    # the finger that chose "Power Off" may still be on the screen
    GLib.timeout_add(1500, lambda: armed.append(True) and False)
    for event in ("button-press-event", "touch-event", "key-press-event"):
        window.connect(event, wake)
    window.connect("destroy", Gtk.main_quit)
    window.show_all()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
