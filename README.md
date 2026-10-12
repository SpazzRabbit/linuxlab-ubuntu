# Ubuntu desktop for HONOR Linux Lab

Turns the Ubuntu of Linux Lab (`com.hihonor.pcengine`) into what a PC shows after a fresh installation of
Ubuntu 26.04: GNOME Shell 50 with the Ubuntu session, the dock, Yaru and the stock programs.
Honor's own LXQt desktop stays installed and is one command away.

This repository is the Linux side of Linux Lab Helper; the app of Android is
https://github.com/SpazzRabbit/linuxlab-helper and is built with this repository checked out next to it
as `../gnome`. `host/` holds the touch module of Honor's X server (`lltouch.c`) and the installer that
packs all of it into one file.

## Install

The Linux Lab Helper app of Android carries the installer: its button puts `linuxlab-helper.sh` into
Downloads. Inside Linux Lab, in a terminal:

    bash /tablet/Download/linuxlab-helper.sh ubuntu

That takes the Ubuntu 24.04 Linux Lab comes with to 26.04 (`upgrade.sh`), sets up the desktop
(`install.sh`) and installs the touch module of the helper. It wants a network, 6 GB of free space and
20 to 40 minutes, and goes on when the terminal is closed: the same command shows how far it is, or
picks up a run that was cut short. Everything it prints is kept in `/var/log/linuxlab-helper.log`.
Afterwards Linux Lab is closed and opened again.

By hand, from this directory, on a Linux Lab that already has Ubuntu 26.04: `sudo bash install.sh`.

## Updates

`llupdate.py` runs in the session and looks at the latest release of this repository a minute and a
half after the start and then every six hours. A release carries `manifest.json`, signed
(`manifest.json.sig`) with the key whose public half is `release-key.gpg`; `gpgv` checks it. When its
serial is higher than the one the installer wrote to `/opt/linuxlab-gnome/release`, a notification
offers it, and "Install" downloads the installer of that release, checks its size and SHA-256 against
the manifest and runs `linuxlab-helper.sh update`: `install.sh update` and the touch module again,
the chosen desktop stays. Linux Lab is then closed and opened again. `llupdate.py check` looks once;
`LINUXLAB_UPDATE=off` in `/etc/default/linuxlab-gnome` switches it off.

The app offers the same update while the Linux side was set up by an older version of it (the
greeting of `lldesktop.py` says which serial). A release is made with `host/release.sh`; the version
and the serial are in `host/version`, the notes of the notification in `host/notes/<version>.json`.

Honor's LXQt desktop stays installed. The switch between the two is in the helper app ("Ubuntu desktop
(GNOME)"); the change takes a few seconds, closes the programs that are open and removes nothing. From a
terminal: `python3 /opt/linuxlab-gnome/lldesktop.py lxqt` or `gnome`.

Touch needs the helper app as before: the desktop gets real touches from it.

## Why it is not just `apt install ubuntu-desktop`

GNOME 50 is a Wayland compositor that expects systemd, logind and a display to drive. Linux Lab is PRoot
inside an Android app; the only thing the tablet shows is the framebuffer of Honor's own Xvfb. So:

| piece | what it does |
| --- | --- |
| `gnome-session.sh` | the work of systemd and gnome-session by hand: buses, PipeWire, `gnome-shell --headless` with a virtual monitor of the size of the X screen, settings daemons, autostart, the bridge |
| `llbridge.c` | the screen and the input devices: frames of the virtual monitor (screen cast over PipeWire) go straight into the screen file Linux Lab shows, whole frames swapped in atomically so that moving windows do not tear, and a video on the beat of its own rhythm; touches, pointer and keys of a window over the X screen go back through `org.gnome.Mutter.RemoteDesktop`; the text of the clipboard goes both ways between GNOME and the CLIPBOARD selection of the X display, which Honor's X server keeps the same as the clipboard of Android |
| `llcapture.c` | a preload for GNOME Shell alone: answers the screen cast by reading back the frame Mutter has just painted, only where it changed, instead of painting everything a second time; tells the bridge exactly what changed; gives the frame clock of the virtual monitor the steady grid of a real one |
| `llcompat.c` | a preload for the calls that fail inside Linux Lab: `fstat` on a memfd, opening `/proc/<pid>/root`, `sd_pid_get_user_unit` (GNOME stops on these), and the terminal calls `tcgetattr`, `tcsetattr`, `isatty`, `ttyname`, which glibc 2.42 and later do with ioctls Android refuses; it also keeps PipeWire from setting the real-time limit to 0, which got the shell killed (see "When the desktop dies"), and does `openat2()` by hand, without which the `tar` of 26.04 unpacks nothing below the top directory of an archive |
| `extension/` | a shell extension: hides the screen sharing indicator (the screen *is* a screen cast here), tells the shell it runs on a tablet so the on-screen keyboard works, starts `gsd-xsettings` for X11 programs; takes the text of the Android keyboard and hands it to the program in focus (see Battery and keyboard) |
| `99_linuxlab.gschema.override` | defaults that must differ from a PC: no screen lock (nothing could unlock it), a dock without snap programs |
| `startlxqt` | the switch: Honor starts the desktop with the bare command `startlxqt`, this file is found first and starts GNOME when `/etc/linuxlab-desktop` says so. A GNOME that dies at once is replaced by LXQt, never by a black screen. It starts the desktop anew after Restart and Log Out, ends with it after Power Off, keeps the sound of Linux playing (see Sound), and ends one desktop and starts the other when the choice is changed |
| `lldesktop.py` | the switches between GNOME and LXQt and between PRoot and chroot: writes the choice and tells `startlxqt`; listens on 127.0.0.1:54397 for the helper app of Android |
| `llupdate.py` | updates from the releases of this repository, signed; a notification offers them (see Updates) |
| `llchroot.py` | the PRoot side of the chroot mode: asks llroot (`overlay/root/llroot.c`) to run the desktop and waits for it (see "The chroot mode") |
| `sudo-chroot` | sudo in the chroot mode: fakeroot, bound over `/usr/bin/sudo` there |
| `upgrade.sh` | Ubuntu 24.04 of Linux Lab to 26.04 with apt alone, in the order that works under PRoot (run by the installer before `install.sh`) |
| `autostart.py` | starts the XDG autostart entries, which systemd does on a PC (not the one that ties PulseAudio to the X server, see Sound) |
| `llsession.py` | the part of gnome-session that the shell and the programs talk to (`org.gnome.SessionManager`): Power Off, Restart and Log Out of the system menu with the usual dialog and countdown, registration of programs, inhibitors (unsaved work is listed in the dialog) |
| `lloff.py` | the screen of a powered-off Linux Lab: black, "touch to switch on" |
| `llandroid.py` | what only Android has, brought by the helper app (`overlay/client`): the charge of the battery becomes `org.freedesktop.UPower`, the text typed on the Android keyboard goes to the shell extension, and the monitor is turned the way the tablet is held (see Turning the screen) |
| `bin/systemd-run` | first in the PATH of the session: runs the command of a "run this in a scope" request as it is. The terminal of Ubuntu (Ptyxis) starts every tab that way and has no shell without it |
| `hidden-apps`, `tablet.desktop` | programs of Honor's LXQt desktop that GNOME has a counterpart for are taken out of the app grid of GNOME (LXQt keeps them); "My Tablet" opens in Files |
| `system-bus.conf`, `service-helper` | a system bus owned by the desktop user (GNOME gives up without one) that starts system services on demand through sudo: AccountsService, polkit, aptdaemon, the backend of Language Support |
| `llnetwork.py` | the reading half of NetworkManager's D-Bus interface: one wired device that stands for the connection Android provides, with the real interface, addresses, gateway and name servers (from rtnetlink), kept current. Gives the Network page of Settings, the network icon in the top bar and an answer to "are we online"; every request to change something is refused |
| `bluetooth-panel.desktop` | hides the Bluetooth page of Settings, which could only ever say "No Bluetooth Found" |
| `aptd` | starts the stock aptdaemon with the uid of the Android app read as the user it runs as; without that it refuses the terminal of every caller, and Language Support cannot install anything |
| `49-linuxlab.rules` | polkit: the desktop user may do what sudo lets him do anyway; there is no login session for polkit to find, and its password dialog cannot work here |

The shell draws on the tablet's GPU, through Mesa's Zink over Turnip, the Vulkan driver Honor puts into
Linux Lab; that works without a screen too. The app of Linux Lab points Mesa at the GPU only when hardware
acceleration is switched on in its own settings, and on a fresh Linux Lab it is off, so `gnome-session.sh`
sets the same variables itself whenever the driver and the device are there. Drawn by the CPU, GNOME at this
resolution is a slide show: the overview opened in one to three frames, against 19 to 48 frames at 86 to 105
a second with the GPU (YLE-W09, `overlay/tests/fps.sh`). `LINUXLAB_GNOME_GPU=0` in
`/etc/default/linuxlab-gnome` switches to the CPU.

Against Honor's LXQt on the same tablet, with the GPU: a window dragged by a finger gets 95 to 107 frames a
second (LXQt: 120, every refresh of the screen), and the time from the finger to the first new frame is the
same, 55 to 68 ms against 54 to 59 (`overlay/tests/lag.sh`): most of it is the way of the touch through
Android and of the picture through the app, which both desktops share.

The virtual monitor runs at 120 Hz, the rate the display refreshes at while Linux Lab is open (Android's
frame rate policy for the app is 60 to 120 Hz, whatever the tablet's own setting; it never went down to
60 Hz in either desktop while a video played). It ran at 165 Hz before, which gave the shell's own
animations about a tenth more frames (117 against 82 to 96 of 120, the overview opening), because a screen
cast never passes frames closer together than the refresh period of its monitor. But the frames of a video
then landed anywhere between the refreshes of the display (see "Video in Firefox"), and the video is what
the tablet shows most. `LINUXLAB_GNOME_REFRESH=<Hz>` in `/etc/default/linuxlab-gnome` changes it.

A virtual monitor has no vertical blank, so Mutter takes the moment a frame is painted for the moment it is
shown, and its frame clock stops whenever nothing has to be painted. The frame callbacks of the windows
(what a Wayland program takes for the vertical blank) then come at the pace of the painting itself, which
is never even. `llcapture.so` reports every frame as shown at the next tick of a steady grid at the refresh
rate, and at that moment, the way a real monitor does (`export LLCAPTURE_VSYNC=off` in
`/etc/default/linuxlab-gnome` leaves Mutter to itself, `LLCAPTURE_VSYNC_LOG=<file>` writes a line per frame).

How many frames really reach the screen depends on how much of it changes, because every frame is read
back from the GPU. Measured on YLE-W09 on the display itself (`dumpsys SurfaceFlinger --latency`):

| what changes | stock Mutter | with `llcapture.so` |
| --- | --- | --- |
| an animation of 1 megapixel | about 45 | 117 of 120 |
| a window of 2 megapixels being dragged | about 45 | about 90 |

Stock Mutter paints the whole stage a second time for every frame of a screen cast into plain memory and
reads all of it back (15 ms). `llcapture.so` reads back the frame that is already painted, only where it
changed, and while the screen keeps changing it collects each read one frame later instead of waiting for
the GPU (`dev/readback-test.c` measures both ways: 9.5 ms against 2.4 ms for 2.3 megapixels). The price is
that the picture is one frame behind during motion. `export LLCAPTURE=sync` in
`/etc/default/linuxlab-gnome` reads at once again, `export LLCAPTURE=off` brings stock Mutter back,
`export LLCAPTURE_STATS=1` writes a line of numbers per second into `shell.log`.

## The chroot mode

On a rooted tablet (KernelSU, Magisk, APatch) the desktop can run in a chroot of Linux Lab's tree
instead of under PRoot: the switch "How Linux runs" in the helper app. PRoot catches every system call
of every program and rewrites its paths, and the app's seccomp filter and SELinux domain refuse a good
part of Linux; in the chroot the programs talk to the kernel directly. Measured on YLE-W09 with the
same commands as the user: `find /usr` 6.36 s under PRoot and 0.14 s in the chroot, `tar` over
`/usr/share/doc` 0.86 and 0.03 s, creating 1000 files 0.36 and 0.03 s, starting a program 2 ms and
0.9 ms. The picture is the same: Linux Lab still starts its X server under PRoot and shows its screen
file, and the bridge draws into it from the chroot.

How it goes:

- The helper app has llroot (`overlay/root/llroot.c`, static, in its assets) and starts it through su
  when the chroot is chosen, with its accessibility service after a reboot, and once a minute if it is
  gone. It lives in the app's own files: it runs as root, and nothing in Linux Lab's tree may.
- llroot listens on 127.0.0.1:54396. `startlxqt` (with `/etc/linuxlab-engine` saying "chroot") runs
  `llchroot.py run gnome|lxqt` instead of the desktop, which hands its environment over and waits.
  llroot takes only the owner of the tree, by the uid of the connection in /proc/net/tcp, and only
  starts one of two fixed commands. When llroot does not answer in 8 seconds, `startlxqt` starts the
  desktop under PRoot as before.
- The desktop runs in a mount namespace of its own (private before anything is mounted: toybox's
  `unshare -m` is not, and its mounts leak into the whole of Android), with `/proc`, `/dev`, `/sys`,
  `/dev/pts`, the shared storage on `/tablet` and `/dev/shm` on `/tmp`, as PRoot gives them. It runs
  as the app's user with the app's groups (3003 is the network), in the SELinux domain of su but with
  the level of the app's files, so that what it creates is the app's and PRoot can use it later.
- Things PRoot hid or faked, put right: PRoot runs with `--link2symlink`, and its hard links are
  symbolic links to the tree's path on Android (`/usr/bin/perl`, the archive of the locales), so the
  tree is mounted under that path once more inside the chroot. `/dev/dri` is hidden: it is the display
  controller, holds no GPU, and Mutter took it for its renderer and hung. A second `honor` line with
  the real uid goes into `/etc/passwd` and `/etc/group`; PRoot never shows that uid.
- sudo: every file of the tree belongs to the app's user, root's files too, so the real sudo refuses
  in the chroot. `sudo-chroot`, bound over it there, runs the command under fakeroot, which is what
  PRoot gave: told it is root, its files the user's. `sudo apt install` works. Daemons of the system
  bus are started without it (`service-helper`): under fakeroot a program cannot show its credentials
  to D-Bus, and GNOME Shell waited 25 seconds for org.freedesktop.Accounts at every start.
- When the desktop ends, or Linux Lab is closed (the connection of `llchroot.py` goes), llroot ends
  every process left in the namespace, and the mounts go with it. Its log: `/var/log/linuxlab-chroot.log`.

`lldesktop.py chroot` and `lldesktop.py proot` switch from a terminal; `/etc/linuxlab-engine` is the
choice. Without root nothing of this is used.

## Video in Firefox

Firefox draws its pages with the CPU here (software WebRender) under either desktop. Asked to use the GPU
(`MOZ_WEBRENDER=1`, `gfx.webrender.all`) it draws as an X11 program and shows nothing as a Wayland one:
Mesa has no way to hand a GPU picture to this compositor without a DRM device.

With the CPU a video is cheap as long as only the video changes: Firefox hands its frames over apart from
the rest of the page. YouTube breaks that for five seconds out of every ten with an animation around the
player; in its light theme Firefox then draws the whole column of the page again for every frame, 4.3 to
4.9 megapixels instead of 2.2, and the video falls from 50 frames a second to 13. Honor's LXQt shows the
page in the dark theme, where the same animation costs little, which is why the same Firefox looked fine
there. `firefox-linuxlab.js` (installed into `/usr/lib/firefox/defaults/pref/`) makes Firefox tell pages
that less animation is wanted (`ui.prefersReducedMotion`): YouTube leaves it out, and the video keeps 31 to
54 frames a second. It is a default: about:config changes it back. A system policy file
(`/etc/firefox/policies`, `distribution/policies.json`) did not take effect in Mozilla's package.

A video that keeps every frame can still judder: what matters is how long each frame stays on the screen.
Measured with `adb shell screenrecord` of the same 60 Hz video (the recording keeps the moment every frame
reached the display, and the intervals between the frames in which the picture of the video changed are
counted), Honor's LXQt showed the frames for one or two refreshes of 60 Hz (16.7 ms: 71 per cent of the
frames, 33 ms: 13), hardly ever for one or three of 120 Hz (8.3 ms: 4 per cent, 25 ms: 3). GNOME as it was
showed them for anything: 8.3 ms 20 per cent, 16.7 ms 40, 25 ms 20. Where that comes from, measured
stage by stage: Firefox on Wayland draws each frame of the video 5 to 10 ms after the frame callback of the
compositor, with the time varying from frame to frame; the shell paints the frame the moment it comes
(the frame clock of a virtual monitor stops in between, see above); the bridge put it on the screen the
moment it was read back. The frames left the shell 9 to 28 ms apart, and whether the display showed one
after one, two or three refreshes was a matter of a millisecond. Three things against it:

- Firefox is given its own 60 Hz timer instead of the frame callbacks (`widget.wayland.vsync.enabled`
  in `firefox-linuxlab.js`): the frames of a video then come evenly, as under X11 in LXQt.
- The frame clock of the shell gets its grid (`llcapture.so`, above).
- The bridge shows frames that keep a rhythm on the beat of that rhythm ("pacing" in `llbridge.c`): the
  rhythm is read from the last sixteen intervals, snapped to 60, 50, 30, 25 or 24 frames a second, and
  each frame is swapped onto the screen three quarters of a period after its due moment, so that a frame
  up to that much late is still on time and one that came early waits in the second spare buffer.
  Frames that keep no rhythm, which is everything faster than 80 or slower than 20 a second, a drag, an
  animation of the shell, the first frames of a video, are shown at once. `--no-pacing` in
  `LINUXLAB_GNOME_BRIDGE_ARGS` switches it off; the line of statistics (SIGUSR1) counts the frames shown
  on the beat, late, and replaced by a later one while they waited.

With all three the same video showed 30 frames a second on the display in GNOME as in LXQt (the rest are
dropped by YouTube itself in both desktops, a 4K stream decoded on the CPU), 56 per cent of them for
16.7 ms, 16 per cent for 8.3 and 13 for 25. That is better than it was, and still not LXQt: the frames
leave Firefox less evenly than under X11, and when the beat of the bridge and a refresh of the display
nearly coincide, Honor's app, which has 8 ms to copy a frame at 120 Hz, is sometimes a refresh late.
Real damage rectangles instead of the one pixel the bridge reports (the app might then copy less) made
no difference, nor did a 60 Hz display mode asked for through Android's settings: Honor's own policy
pins the app at 120 Hz.

`overlay/tests/watch.sh` counts the frames that reach the screen without touching anything, `fps.sh` does it
during a drag or after a tap, `lag.sh` times a touch; the bridge counts frames and megapixels per frame
when sent SIGUSR1 (`pkill -USR1 llbridge`, lines in `bridge.log`).

## Why windows do not tear

Linux Lab's app copies the screen file of Xvfb (`/home/XvfbScreen/Xvfb_screen0`) into a texture whenever
Xvfb reports a change, without any lock against the X server writing into it. A big change (a window being
dragged) is half written when the copy starts, and the window tears. The app opens the file by name for
every copy and maps it again when the inode is a different one, so the bridge keeps three buffers of its
own and swaps a complete one in under that name with `renameat2(RENAME_EXCHANGE)`. One pixel drawn through
X afterwards makes Xvfb report the change (the app does not watch the file itself: without that pixel
the screen stays black). Xvfb's own file is put back when the bridge exits. A bridge
that was killed cannot do that, so the next one, and `startlxqt` before it starts LXQt, look into the
memory map of the X server for the file it really draws into: a file left aside by an earlier run of
Linux Lab belongs to an X server that is gone, and putting that one back left LXQt without a picture.
`LINUXLAB_GNOME_FRAMEBUFFER=0` in `/etc/default/linuxlab-gnome` goes back to drawing through X.

If GNOME Shell dies, the session starts it again (open programs are lost). After three deaths in a row,
each within 45 seconds, the session gives up and Honor's LXQt comes up.

## Where to look when something is wrong

Logs of the last two sessions are in `~/.cache/linuxlab-gnome/`: `session.log` (this directory's script),
`shell.log` (GNOME Shell), `bridge.log`, `daemons.log`, `bus.log` (the session bus and the programs it starts). `pkill -USR1 llbridge` makes the bridge write a line
of frame statistics every second.

## Sound

Honor's scheme: its PulseAudio plays into a null sink ("Dummy Output" in Settings), a TCP module hands
the monitor of that sink to the Android app on 127.0.0.1:54321, and the app plays it. The app connects
when it reads the line `StartFinished` in the output of the container, and it stops for good when any
line there contains ` exit` or `signal`, whoever wrote it. Under GNOME three writers did that:

- the session bus ("Process org.freedesktop.systemd1 exited with status 1", a second after every start):
  `gnome-session.sh` sends the output of the bus and of everything it starts to `bus.log`;
- PulseAudio itself, when its X11 modules lose the X server of the session ("X11 I/O error exit handler
  called"): `install.sh` sets aside the script that loads those modules when Xwayland starts, and
  `autostart.py` skips the autostart entry that does the same;
- anything else, Honor's own programs included: `startlxqt` checks every five seconds that somebody
  listens on the TCP module and says `StartFinished` again when nobody does.

To see whether Android is listening: `pactl list short source-outputs` shows a `protocol-simple.c` client.

## Battery and keyboard

Linux Lab sees neither the battery (`/sys/class/power_supply` and Android's battery service are closed
to it) nor the keyboard of Android (the stock app gives the keyboard no text field, so it can only send
key codes, which means Latin letters). The helper app of this project (`overlay/client`, the one that
brings real touches) has both and opens a second connection, to `llandroid.py` on 127.0.0.1:54399.

- The charge shows in the top bar and in Settings as on a laptop: `upower -d` lists the battery.
- The touch layer of the helper is a text field as far as the Android keyboard is concerned. When a
  text field of GNOME asks for a keyboard (the moments when GNOME would show its own), the layer takes
  the input focus, the Android keyboard comes up and types into it; text, text in the making (a word
  under completion, pinyin), deletions and the editing keys go to the shell extension, which hands
  them to the program in focus the way GNOME's own screen keyboard does. So every language the Android
  keyboard has works, in terminals too. A terminal never asks for a keyboard, so a tap in its window
  counts as asking. The keyboard button of the top bar brings the Android keyboard up and down by hand.
  Which keyboard it is, is Linux Lab's own setting "Screen keyboard" (Android or Linux), the one Honor's
  LXQt follows: the app mirrors it into `~/.config/linuxlab-use-android-ime`, `llandroid.py` reads the
  file every two seconds, and with "Linux" GNOME's own screen keyboard is back. The helper only shows
  the choice; up to version 0.8 it had a switch of its own, which GNOME followed and LXQt did not, so
  "Linux keyboard" chosen in Linux Lab still brought up the Android one in GNOME. With a real keyboard
  attached the helper stays out of it.
- X11 programs (through Xwayland) get key presses instead of text, and those can only say what the
  keyboard layout in use in GNOME has.
- The windows of GNOME do not move out of the way of the Android keyboard.

`overlay/tests/ime-languages.sh` types thirteen languages, a composed word, pinyin and the deleting
keys through a keyboard made for tests (`overlay/tests/testime`), which Android treats as the real one.

## Turning the screen

The window of Linux Lab is a landscape one whatever happens: Android turns it upside down with the
tablet, never upright. So the turning is done inside. The helper app reads the tilt sensor and says how
far the picture still has to be turned for the person holding the tablet (the line `rotation`, see
`llandroid.py`); `llandroid.py` turns the monitor of GNOME through `org.gnome.Mutter.DisplayConfig`, for
now only, nothing is written to `monitors.xml`; the bridge turns the picture and the touches back into
the landscape screen Linux Lab shows (`llbridge.c`).

The switch is GNOME's own: "Auto Rotate" in the quick settings (the setting `orientation-lock`), on from
the start, as on any tablet with GNOME. The shell offers that button only when Mutter itself reads the
sensor of a built-in panel, so the extension shows it while the helper is connected. Off, the screen
stays as it is. Settings, Displays, Orientation turns it by hand.

Upright, the keyboard is GNOME's: the one of Android belongs to the landscape window and would lie on
its side. A turn takes about a second, the time the bridge needs to get the new stream of the screen.
When the helper goes away the screen returns to landscape.

## When the desktop dies

GNOME Shell used to be killed with SIGKILL (exit code 137 in `session.log`) with no word in any log:
on every other change of the monitors (a turn, another scale, another refresh rate), sometimes seconds
after its start, three shells in a row. It died only when it drew with the GPU.

The cause is a limit, not the GPU. PipeWire's `module-rt`, which lives in the shell and in every program
that plays or records, asks RealtimeKit how long a real-time thread may run. There is no RealtimeKit
here, and it sets `RLIMIT_RTTIME` to 0. No thread of Linux Lab can become real-time by asking, so
nothing should follow. But the GPU driver of the kernel has real-time threads of its own, and a thread of
the shell that holds the driver's lock in the middle of an ioctl is lent their priority for that moment.
When the timer tick of the scheduler finds it there, it sees a real-time thread whose allowance is 0
and kills the process: silently, since the kernel prints such a kill only when asked to. Programs the
shell started inherited the limit.

How it was found, for the next riddle of this kind:

- `dev/lltrace.c`, preloaded into the shell (`LINUXLAB_GNOME_PRELOAD=/path/lltrace.so` in
  `/etc/default/linuxlab-gnome`), records every ioctl to the GPU into a ring in a file that outlives the
  process; `dev/lltrace.py` reads it. Three deaths out of three ended inside an ioctl of the GPU driver,
  in different ioctls.
- `adb bugreport` taken within four minutes of a death holds the kernel log of that moment (the
  section `KERNEL LOG (dmesg)`) and Android's own record of every signal sent (`KStateManager`). Neither
  had a GPU fault, a kill for memory, or a SIGKILL sent to the shell by anybody.
- `/proc/<pid>/limits` of the shell said `Max realtime timeout 0 0`, and that of its parent `unlimited`.

`llcompat.so` now answers "done" to a request to lower that limit and does not pass it on
(`/tmp/llcompat-rlimit.log` names who asked). Measured by changing the monitor every 2.5 s: before, the
shell died on the first to the ninth change; after, it lived through 96 in a row. Software rendering
(`LINUXLAB_GNOME_GPU=0`) never died because it never enters the GPU driver.

Every death of the shell still leaves `crash-<time>.txt` next to the logs, with what could be learned.

A change of the monitors also ends the stream of the screen (Mutter closes it and tells nobody). The
bridge starts over then, and the picture is back in about a second.

## Language

Language Support and Settings work as on a PC: language packs are installed through aptdaemon, the choice
goes to AccountsService, which writes `~/.pam_environment`. Nobody logs in here, so `gnome-session.sh` reads
that file when the desktop starts. A new language shows after Linux Lab is restarted.

## What a PC has and this does not

- No snaps: snapd needs an init system. Firefox is the package from Mozilla, there is no App Center.
  Updates come the usual way, through Software Updater.
- No lock screen, no suspend, no users: Android owns all of that.
- Wi-Fi and Bluetooth are switched and set up in Android. An app may not open the Bluetooth adapter, see
  the radios or change the network (`/sys/class/net`, `/sys/class/rfkill`, HCI sockets are all closed), so
  NetworkManager and BlueZ cannot run. Settings shows the connection of the tablet as a wired one, read
  only, like the network card of a virtual machine; the Bluetooth page is hidden.
- Power Off ends the desktop and all its programs and leaves a black screen that says "touch to switch
  on" (`lloff.py`); the Android app itself is closed the Android way. Nothing from inside can close it:
  its channel to the X server has no such message in the Linux Lab mode, and Android does not let a
  program started here run its own tools (`cmd activity` is refused as a foreign package, a fresh
  `app_process` cannot read the system properties it needs). And a container that ends leaves the app on
  a dead black screen that even its icon does not revive, so the container is kept. Restart and Log Out
  both start the desktop anew: there is neither a firmware to go through nor a login screen to return to.
- The clipboard is shared with Android as text only (pictures and files are not).

`dev/` holds what was used to build this over adb; it is not installed.
