// GNOME Shell inside HONOR Linux Lab.
//
// The screen of this session is a screen cast (see llbridge.c), so the shell believes the screen is
// being shared and shows the orange indicator whose button would cut the picture off. It is hidden.
//
// The touchscreen of this session is a virtual device, and Mutter counts only real ones when it
// decides whether the machine is in touch mode. The shell is told that it is: this is a tablet.
// The on-screen keyboard then comes up after a touch in a text field, as on any tablet with GNOME,
// and stays away while a mouse or a real keyboard is in use. A keyboard button in the top bar opens and
// closes it by hand, for terminals and other programs that do not tell the shell about their text fields.
//
// The keyboard of Android can stand in for the one of GNOME. The helper app of Android sends what is
// typed on it as text (through llandroid.py), and the shell hands the text to the program in focus
// the way its own screen keyboard does. So the object exported below takes text, text in the
// making (a word being completed, a syllable of Chinese), deletions and a few keys. While the helper
// offers its keyboard, the moments when GNOME would bring its own keyboard up or down are passed on
// instead, and the button in the top bar asks for the keyboard of Android.
//
// The same helper says how the tablet is held, and llandroid.py turns the monitor to match. GNOME has a
// button for that in the quick settings, "Auto Rotate", but shows it only when Mutter itself reads a
// sensor of a built-in panel. While the helper is the sensor, the button is shown; it is the stock
// one and switches the stock setting, which llandroid.py obeys.
//
// The updates of Linux Lab Helper (llupdate.py, which is dev.linuxlab.Update on the session bus) have an
// icon in the top bar: its menu says which version is installed and what was found, looks for updates
// on request and installs one. The icon turns into the one of an available update while there is one.
//
// On a PC systemd starts gsd-xsettings together with Xwayland. There is no systemd here, so the
// daemon is started from the shell, which knows the X display. Without it X11 programs get neither
// the theme nor the scale.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Pango from 'gi://Pango';
import St from 'gi://St';

import {Extension, InjectionManager, gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as SystemActions from 'resource:///org/gnome/shell/misc/systemActions.js';

const XSETTINGS = '/usr/libexec/gsd-xsettings';

const INPUT_PATH = '/dev/linuxlab/Input';
// Wanted: 0 hide the keyboard, 1 show it, 2 toggle
const INPUT_INTERFACE = `
<node>
  <interface name="dev.linuxlab.Input">
    <method name="Commit"><arg type="s" direction="in" name="text"/></method>
    <method name="Preedit"><arg type="s" direction="in" name="text"/></method>
    <method name="Delete">
      <arg type="u" direction="in" name="before"/>
      <arg type="u" direction="in" name="after"/>
    </method>
    <method name="Key"><arg type="u" direction="in" name="keyval"/></method>
    <method name="SetAndroidKeyboard"><arg type="b" direction="in" name="active"/></method>
    <method name="Request"><arg type="u" direction="in" name="state"/></method>
    <method name="SetSensor"><arg type="b" direction="in" name="present"/></method>
    <signal name="Wanted"><arg type="u" name="state"/></signal>
  </interface>
</node>`;

const UPDATE_INTERFACE = `
<node>
  <interface name="dev.linuxlab.Update">
    <method name="Check"/>
    <method name="Install"/>
    <method name="GetState"><arg type="a{ss}" direction="out" name="state"/></method>
    <signal name="Changed"><arg type="a{ss}" name="state"/></signal>
  </interface>
</node>`;
const UpdateProxy = Gio.DBusProxy.makeProxyWrapper(UPDATE_INTERFACE);
const RELEASES = 'https://github.com/SpazzRabbit/linuxlab-ubuntu/releases';

// in the language of the session: this extension has no translations of its own
const RUSSIAN = GLib.get_language_names().some(name => name.startsWith('ru'));
function tr(english, russian) {
    return RUSSIAN ? russian : english;
}

const UpdateButton = GObject.registerClass(
class UpdateButton extends PanelMenu.Button {
    _init(path) {
        super._init(0.5, 'Linux Lab Helper');
        this._ownIcon = Gio.icon_new_for_string(`${path}/icons/linuxlab-helper-symbolic.svg`);
        this._icon = new St.Icon({gicon: this._ownIcon, style_class: 'system-status-icon'});
        this.add_child(this._icon);

        this._title = new PopupMenu.PopupMenuItem('Linux Lab Helper', {reactive: false});
        this._title.label.add_style_class_name('popup-subtitle-menu-item');
        this._status = new PopupMenu.PopupMenuItem('', {reactive: false});
        this._status.label.style = 'max-width: 22em;';
        this._status.label.clutter_text.line_wrap = true;
        this._status.label.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
        this.menu.addMenuItem(this._title);
        this.menu.addMenuItem(this._status);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._install = this.menu.addAction('', () => this._proxy?.InstallAsync().catch(logError));
        this._check = this.menu.addAction(tr('Check for updates', 'Проверить обновления'),
            () => this._proxy?.CheckAsync().catch(logError));
        this._news = this.menu.addAction(tr('What is new', 'Что нового'),
            () => Gio.AppInfo.launch_default_for_uri(this._page ?? RELEASES, null));

        this._proxy = null;
        this._changedId = 0;
        this.hide();
        // shown while the service runs (LINUXLAB_UPDATE=off switches it off)
        this._watchId = Gio.bus_watch_name(Gio.BusType.SESSION, 'dev.linuxlab.Update',
            Gio.BusNameWatcherFlags.NONE, () => this._connect(), () => this._gone());
    }

    _connect() {
        new UpdateProxy(Gio.DBus.session, 'dev.linuxlab.Update', '/dev/linuxlab/Update', (proxy, error) => {
            if (error || this._watchId === 0) {
                if (error)
                    logError(error);
                return;
            }
            this._gone();
            this._proxy = proxy;
            this._changedId = proxy.connectSignal('Changed', (_proxy, _owner, [state]) => this._show(state));
            proxy.GetStateAsync().then(([state]) => this._show(state)).catch(logError);
        });
    }

    _gone() {
        if (this._proxy && this._changedId)
            this._proxy.disconnectSignal(this._changedId);
        this._proxy = null;
        this._changedId = 0;
        this.hide();
    }

    _show(state) {
        if (!this._proxy)
            return;
        this._page = state.page;
        this._title.label.text = state.installed ? `Linux Lab Helper ${state.installed}` : 'Linux Lab Helper';
        const latest = state.latest ?? '';
        const texts = {
            idle: tr('Updates are looked for every few hours.', 'Обновления проверяются раз в несколько часов.'),
            checking: tr('Looking for updates', 'Проверяю обновления'),
            current: tr('This is the latest version.', 'Установлена последняя версия.'),
            available: tr(`Version ${latest} is out.`, `Вышла версия ${latest}.`) + (state.notes ? `\n\n${state.notes}` : ''),
            installing: tr('Updating, a few minutes. Programs keep running.', 'Обновляю, несколько минут. Программы продолжают работать.'),
            installed: tr('Updated. Close Linux Lab and open it again.', 'Обновлено. Закройте Linux Lab и откройте снова.'),
            failed: state.error || tr('The update failed.', 'Обновление не удалось.'),
            offline: tr('GitHub does not answer.', 'GitHub не отвечает.'),
        };
        this._status.label.text = texts[state.state] ?? state.state;
        const busy = state.state === 'checking' || state.state === 'installing';
        this._check.setSensitive(!busy);
        this._install.visible = state.state === 'available';
        this._install.label.text = tr(`Install ${latest}`, `Установить ${latest}`);
        this._news.visible = state.state === 'available';
        this._icon.gicon = state.state === 'available'
            ? new Gio.ThemedIcon({name: 'software-update-available-symbolic'}) : this._ownIcon;
        this.show();
    }

    _onDestroy() {
        Gio.bus_unwatch_name(this._watchId);
        this._watchId = 0;
        this._gone();
        super._onDestroy();
    }
});

const KeyboardButton = GObject.registerClass(
class KeyboardButton extends PanelMenu.Button {
    _init(onRequest) {
        super._init(0.0, _('Screen Keyboard'), true);
        this.add_child(new St.Icon({
            icon_name: 'input-keyboard-symbolic',
            style_class: 'system-status-icon',
        }));

        // on release: a finger that lands on the button and slides away has not asked for anything
        const tap = new Clutter.ClickGesture();
        tap.connect('recognize', () => onRequest());
        this.add_action(tap);

        Main.keyboard.connectObject('visibility-changed', () => this._sync(), this);
        this._sync();
    }

    _sync() {
        if (Main.keyboard.visible)
            this.add_style_pseudo_class('checked');
        else
            this.remove_style_pseudo_class('checked');
    }
});

export default class LinuxLabExtension extends Extension {
    enable() {
        this._indicator = Main.panel.statusArea.screenSharing;
        if (this._indicator) {
            this._visibleId = this._indicator.connect('notify::visible', () => {
                if (this._indicator.visible)
                    this._indicator.hide();
            });
            this._indicator.hide();
        }

        this._injections = new InjectionManager();
        this._injections.overrideMethod(Clutter.Seat.prototype, 'get_touch_mode', () => function () {
            return true;
        });
        // the shell keeps the keyboard only while the last device used was a touchscreen;
        // asked for by the button, it stays until it is closed
        this._keyboardWanted = false;
        const extension = this;
        // (and while the keyboard of Android is on offer, whatever was used last: the helper itself
        // stays away when a real keyboard is attached)
        this._injections.overrideMethod(Main.keyboard, '_lastDeviceIsTouchscreen', original => function () {
            return extension._keyboardWanted || extension._androidKeyboard || original.call(this);
        });
        Main.keyboard._syncEnabled();
        // "Auto Rotate" in the quick settings, while the helper tells how the tablet is held
        this._sensor = false;
        this._systemActions = SystemActions.getDefault();
        this._injections.overrideMethod(this._systemActions, '_updateOrientationLock', original => function () {
            if (!extension._sensor)
                return original.call(this);
            this._actions.get('lock-orientation').available = true;
            this.notify('can-lock-orientation');
            return undefined;
        });
        // while the keyboard of Android is on offer, it comes and goes when GNOME's own would
        this._androidKeyboard = false;
        this._injections.overrideMethod(KeyboardUI.Keyboard.prototype, 'open', original => function (...args) {
            if (!extension._androidKeyboard)
                return original.apply(this, args);
            extension._want(1);
            return undefined;
        });
        this._injections.overrideMethod(KeyboardUI.Keyboard.prototype, 'close', original => function (...args) {
            if (!extension._androidKeyboard)
                return original.apply(this, args);
            extension._want(0);
            return undefined;
        });
        // the swipe from the bottom edge that pulls GNOME's keyboard up
        for (const [gesture, state] of [['gestureProgress', -1], ['gestureActivate', 1], ['gestureCancel', -1]]) {
            this._injections.overrideMethod(KeyboardUI.Keyboard.prototype, gesture, original => function (...args) {
                if (!extension._androidKeyboard)
                    return original.apply(this, args);
                if (state >= 0)
                    extension._want(state);
                return undefined;
            });
        }
        // A terminal has a text field of a kind and never asks for a keyboard. A tap in its window does.
        this._touchDown = null;
        global.stage.connectObject('captured-event', (_stage, event) => {
            if (this._androidKeyboard)
                this._watchTap(event);
            return Clutter.EVENT_PROPAGATE;
        }, this);
        // made now and not at the first key: the first event of a device that has just appeared is lost
        this._virtualKeyboard = global.stage.context.get_backend().get_default_seat()
            .create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        this._input = Gio.DBusExportedObject.wrapJSObject(INPUT_INTERFACE, this);
        this._input.export(Gio.DBus.session, INPUT_PATH);
        // The touchscreen of the bridge is a device of its remote desktop session and goes away with it.
        // The shell keeps the last device it saw and asks the dead object for its type ever after.
        const seat = global.stage.context.get_backend().get_default_seat();
        seat.connectObject('device-removed', (_seat, device) => {
            if (Main.keyboard._lastDevice === device)
                Main.keyboard._lastDevice = null;
        }, this);

        this._keyboardButton = new KeyboardButton(() => this._toggleKeyboard());
        Main.panel.addToStatusArea('linuxlab-keyboard', this._keyboardButton, 0, 'right');
        this._updateButton = new UpdateButton(this.path);
        Main.panel.addToStatusArea('linuxlab-update', this._updateButton, 0, 'right');
        Main.keyboard.connectObject('visibility-changed', () => {
            if (!Main.keyboard.visible)
                this._keyboardWanted = false;
        }, this);

        this._openedId = global.display.connect('x11-display-opened', () => this._startXSettings());
        this._closingId = global.display.connect('x11-display-closing', () => this._stopXSettings());
        // Mutter names the X display in the environment from the start; Xwayland itself comes up on the first client
        this._startXSettings();
    }

    disable() {
        if (this._indicator && this._visibleId)
            this._indicator.disconnect(this._visibleId);
        this._indicator = null;
        this._visibleId = 0;

        Main.keyboard.disconnectObject(this);
        global.stage.context.get_backend().get_default_seat().disconnectObject(this);
        this._keyboardButton.destroy();
        this._keyboardButton = null;
        this._updateButton.destroy();
        this._updateButton = null;

        global.stage.disconnectObject(this);
        this._input.unexport();
        this._input = null;
        this._virtualKeyboard?.run_dispose();
        this._virtualKeyboard = null;
        this._androidKeyboard = false;
        this._sensor = false;
        this._injections.clear();
        this._injections = null;
        this._systemActions._updateOrientationLock();
        this._systemActions = null;
        this._keyboardWanted = false;
        Main.keyboard._syncEnabled();

        global.display.disconnect(this._openedId);
        global.display.disconnect(this._closingId);
        this._stopXSettings();
    }

    // ---- dev.linuxlab.Input

    Commit(text) {
        // a program that told the shell about its text field takes text as such, in any language
        if (Main.inputMethod.currentFocus) {
            Main.inputMethod.commit(text);
            return;
        }
        // the others (X11 programs) get key presses, which can only say what the layout in use has
        for (const character of text)
            this._tap(Clutter.unicode_to_keysym(character.codePointAt(0)));
    }

    Preedit(text) {
        if (!Main.inputMethod.currentFocus)
            return;
        const length = [...text].length;
        Main.inputMethod.set_preedit_text(text.length > 0 ? text : null, length, length,
            Clutter.PreeditResetMode.COMMIT);
    }

    Delete(before, after) {
        // as keys: every program understands them, a terminal included
        for (let i = 0; i < before; i++)
            this._tap(Clutter.KEY_BackSpace);
        for (let i = 0; i < after; i++)
            this._tap(Clutter.KEY_Delete);
    }

    Key(keyval) {
        this._tap(keyval);
    }

    SetAndroidKeyboard(active) {
        if (active && Main.keyboard.visible)
            Main.keyboard.close();
        this._androidKeyboard = active;
        Main.keyboard._syncEnabled();
    }

    Request(state) {
        this._want(state);
    }

    SetSensor(present) {
        this._sensor = present;
        this._systemActions._updateOrientationLock();
    }

    _watchTap(event) {
        const type = event.type();
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            const [x, y] = event.get_coords();
            this._touchDown = {x, y, time: event.get_time()};
            return;
        }
        if (type !== Clutter.EventType.TOUCH_END || !this._touchDown)
            return;
        const down = this._touchDown;
        this._touchDown = null;
        const [x, y] = event.get_coords();
        // a tap, not a scroll and not a long press
        if (Math.hypot(x - down.x, y - down.y) > 30 || event.get_time() - down.time > 600)
            return;
        const method = Main.inputMethod;
        if (!method.currentFocus || method.content_purpose !== Clutter.InputContentPurpose.TERMINAL)
            return;
        const rect = global.display.focus_window?.get_frame_rect();
        if (rect && x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height)
            this._want(1);
    }

    _want(state) {
        this._input?.emit_signal('Wanted', new GLib.Variant('(u)', [state]));
    }

    _tap(keyval) {
        const time = GLib.get_monotonic_time();
        this._virtualKeyboard.notify_keyval(time, keyval, Clutter.KeyState.PRESSED);
        this._virtualKeyboard.notify_keyval(time, keyval, Clutter.KeyState.RELEASED);
    }

    _toggleKeyboard() {
        if (this._androidKeyboard) {
            this._want(2);
            return;
        }
        if (Main.keyboard.visible) {
            Main.keyboard.close();
            return;
        }
        this._keyboardWanted = true;
        Main.keyboard._syncEnabled();
        Main.keyboard.open(Main.layoutManager.primaryIndex);
    }

    _startXSettings() {
        if (this._xsettings || !GLib.getenv('DISPLAY') || !Gio.File.new_for_path(XSETTINGS).query_exists(null))
            return;
        try {
            // the environment of the shell already names the X display Mutter has just opened
            this._xsettings = Gio.Subprocess.new([XSETTINGS], Gio.SubprocessFlags.NONE);
            this._xsettings.wait_async(null, (process, result) => {
                try {
                    process.wait_finish(result);
                } catch (e) {
                    logError(e);
                }
                if (this._xsettings === process)
                    this._xsettings = null;
            });
        } catch (e) {
            logError(e, 'Linux Lab: cannot start gsd-xsettings');
        }
    }

    _stopXSettings() {
        this._xsettings?.force_exit();
        this._xsettings = null;
    }
}
