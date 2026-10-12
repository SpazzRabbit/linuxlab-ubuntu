#!/bin/bash
# Ubuntu GNOME session inside Linux Lab.
#
# Linux Lab has no init system and no Wayland output, only Honor's Xvfb. So this script does by hand
# what systemd and gnome-session do on a PC: buses, PipeWire, GNOME Shell with a virtual monitor,
# the settings daemons, and llbridge, which shows the monitor in the X server and feeds the input back.
#
# Started by /usr/local/bin/startlxqt (see install.sh) in place of the LXQt session.
# Logs: ~/.cache/linuxlab-gnome/
D=/opt/linuxlab-gnome

# Linux Lab hands the library directory of PRoot to everything it starts. It holds an Android build of
# libtalloc.so.2, which then wins over Ubuntu's: every program that links Samba (Settings among them)
# dies at start with "libc.so: invalid ELF header". Nothing inside Linux needs that directory.
unset LD_LIBRARY_PATH

LOG=$HOME/.cache/linuxlab-gnome
if [ "$1" != "--inside" ]; then
    mkdir -p "$LOG"
    for f in shell bridge session pipewire wireplumber daemons bus; do
        [ -f "$LOG/$f.log" ] && mv -f "$LOG/$f.log" "$LOG/$f.log.1"
    done
    # Nothing of the session may reach the output of the container. The app of Linux Lab reads it
    # line by line, and a line with " exit" or "signal" in it, from any program, makes the app stop
    # playing the sound of Linux until the next start. The session bus alone did that a second after
    # every start ("Process org.freedesktop.systemd1 exited with status 1"). The bus and everything
    # it starts write to bus.log.
    exec dbus-run-session -- "$0" --inside >"$LOG/bus.log" 2>&1
fi

[ -f /etc/default/linuxlab-gnome ] && . /etc/default/linuxlab-gnome
X_DISPLAY=${DISPLAY:-:99}
# stand-ins for tools of systemd that programs of the desktop call (see bin/)
export PATH=$D/bin:$PATH
exec >"$LOG/session.log" 2>&1
log() { echo "[gnome-session] $(date '+%H:%M:%S') $*"; }

SIZE=${LINUXLAB_GNOME_SIZE:-$(xdpyinfo -display "$X_DISPLAY" 2>/dev/null | awk '/dimensions:/ {print $2; exit}')}
SIZE=${SIZE:-1920x1200}
# The virtual monitor runs at 120 Hz, the most the tablet shows (Android's frame rate policy for the app
# is 60 to 120 Hz, whatever the tablet's own setting): llcapture.so gives its frame clock a steady grid
# at that rate, and the bridge shows a video on the beat of that grid (see "pacing" in llbridge.c), which
# only works out when the display refreshes in step with the monitor. With a monitor at 165 Hz the
# shell's own animations got a tenth more frames on the display (117 against 82 to 96 of 120), but the
# frames of a 60 Hz video landed anywhere between the refreshes.
REFRESH=${LINUXLAB_GNOME_REFRESH:-120}
log "X display $X_DISPLAY, screen $SIZE at $REFRESH Hz"

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/runtime-$USER}
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# left by a session that was killed; PipeWire and Mutter refuse to start over their old locks
find "$XDG_RUNTIME_DIR" -maxdepth 1 \( -name 'pipewire-*' -o -name 'wayland-*' \) -exec rm -f {} + 2>/dev/null

# what a stock Ubuntu session has in its environment
export XDG_SESSION_TYPE=wayland XDG_SESSION_CLASS=user
export XDG_CURRENT_DESKTOP=ubuntu:GNOME XDG_SESSION_DESKTOP=ubuntu DESKTOP_SESSION=ubuntu
export GNOME_SHELL_SESSION_MODE=ubuntu XDG_MENU_PREFIX=gnome-
export XDG_CONFIG_DIRS=/etc/xdg/xdg-ubuntu:/etc/xdg
# $D/share comes first and belongs to this session alone: it hides what GNOME should not show (see install.sh)
export XDG_DATA_DIRS=$D/share:/usr/share/ubuntu:/usr/share/gnome:/usr/local/share:/usr/share
# The language chosen in Language Support or in Settings. Ubuntu keeps it in ~/.pam_environment,
# which PAM applies at login on a PC; nobody logs in here. Honor's script sets LC_ALL from the system
# locale, and LC_ALL would override all of it.
if [ -f "$HOME/.pam_environment" ]; then
    unset LC_ALL
    while read -r name value; do
        case "$name" in
        LANG=* | LANGUAGE=* | LC_[A-Z]*=* | PAPERSIZE=*)
            value=${name#*=}
            name=${name%%=*}
            ;;
        LANG | LANGUAGE | LC_[A-Z]* | PAPERSIZE) ;;
        *) continue ;;
        esac
        value=${value#DEFAULT=}
        value=${value%%[[:space:]]OVERRIDE=*}
        value=${value#\"}
        value=${value%\"}
        [ -n "$value" ] && export "$name=$value"
    done <"$HOME/.pam_environment"
    log "language of the user: LANG=$LANG LANGUAGE=${LANGUAGE:-}"
fi

# Honor's settings for LXQt on X11: here the programs are Wayland clients of GNOME
unset GTK_IM_MODULE QT_IM_MODULE XMODIFIERS GDK_BACKEND QT_QPA_PLATFORM QT_X11_NO_MITSHM GDK_DISABLE_SHM
unset DISPLAY WAYLAND_DISPLAY
# The GPU of the tablet: Mesa's Zink over Turnip, the Vulkan driver Honor puts into Linux Lab. The app
# of Linux Lab points Mesa at it only when hardware acceleration is switched on in its own settings, and
# after a fresh installation it is off: the shell and every program would be drawn by the CPU. So the
# same variables are set here whenever the driver and the device are there.
# LINUXLAB_GNOME_GPU=0 in /etc/default/linuxlab-gnome makes everything draw on the CPU instead.
if [ "${LINUXLAB_GNOME_GPU:-1}" != "1" ]; then
    unset GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE LIBGL_KOPPER_DRI2 TU_DEBUG MESA_VK_WSI_DEBUG
elif [ -z "${GALLIUM_DRIVER:-}" ] && [ -c /dev/kgsl-3d0 ] && [ -e /usr/lib/aarch64-linux-gnu/libvulkan_freedreno.so ]; then
    export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=true TU_DEBUG=sysmem MESA_VK_WSI_DEBUG=sw
    log "the GPU is used: Linux Lab did not ask for it, its hardware acceleration is switched off"
fi

# a fresh profile: the scale an Ubuntu installer picks for a screen this dense; Settings can change it later
MONITORS=$HOME/.config/monitors.xml
if [ ! -f "$MONITORS" ]; then
    width=${SIZE%x*}
    height=${SIZE#*x}
    scale=1
    [ "$width" -ge 2560 ] && scale=2
    mkdir -p "$HOME/.config"
    cat >"$MONITORS" <<XML
<monitors version="2">
  <configuration>
    <layoutmode>logical</layoutmode>
    <logicalmonitor>
      <x>0</x>
      <y>0</y>
      <scale>$scale</scale>
      <primary>yes</primary>
      <monitor>
        <monitorspec>
          <connector>Meta-0</connector>
          <vendor>MetaVendor</vendor>
          <product>MetaVirtualMonitor</product>
          <serial>0x00</serial>
        </monitorspec>
        <mode>
          <width>$width</width>
          <height>$height</height>
          <rate>$REFRESH.000</rate>
        </mode>
      </monitor>
    </logicalmonitor>
  </configuration>
</monitors>
XML
    log "first start: scale $scale"
else
    # the saved layout names the rate of the monitor; with another rate Mutter would drop the whole layout
    sed -i "s#<rate>[0-9.]*</rate>#<rate>$REFRESH.000</rate>#" "$MONITORS"
fi

# GNOME Shell asks logind for everything when this directory exists, and there is no logind here
rmdir /run/systemd/seats 2>/dev/null

# "Restart to install updates": packages leave this note for the next boot, and on a PC it goes with
# /run at boot. Here /run is a plain directory that stays, and the start of the desktop is the boot.
rm -f /run/reboot-required /run/reboot-required.pkgs

# the system bus: nothing runs on it, but GNOME gives up without one
if ! dbus-send --system --dest=org.freedesktop.DBus --print-reply /org/freedesktop/DBus org.freedesktop.DBus.GetId >/dev/null 2>&1; then
    mkdir -p /run/dbus
    rm -f /run/dbus/system_bus_socket
    dbus-daemon --config-file=$D/system-bus.conf --nofork >>"$LOG/daemons.log" 2>&1 &
    log "system bus started"
fi

dbus-update-activation-environment --all

pipewire >"$LOG/pipewire.log" 2>&1 &
PIDS="$!"
for _ in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break; sleep 0.1; done
wireplumber >"$LOG/wireplumber.log" 2>&1 &
PIDS="$PIDS $!"

# the network of the tablet the way NetworkManager would show it: the Network page of Settings, the icon
# in the top bar, the answer to "are we online" (see llnetwork.py)
python3 $D/llnetwork.py >>"$LOG/daemons.log" 2>&1 &
PIDS="$PIDS $!"

# Power Off, Restart and Log Out in the menu of the shell (see llsession.py). It leaves the wish in
# this file and sends SIGTERM here.
END=$XDG_RUNTIME_DIR/linuxlab-session-end
rm -f "$END"
LINUXLAB_SESSION_PID=$$ LINUXLAB_SESSION_END=$END python3 $D/llsession.py >>"$LOG/daemons.log" 2>&1 &
PIDS="$PIDS $!"

# the battery and the keyboard of the tablet, which the helper app of Android brings (see llandroid.py)
python3 $D/llandroid.py >>"$LOG/daemons.log" 2>&1 &
# updates of all this, from the releases on GitHub (see llupdate.py)
python3 $D/llupdate.py >>"$LOG/daemons.log" 2>&1 &
PIDS="$PIDS $!"

# starts the shell and waits for its Wayland socket
start_shell() {
    find "$XDG_RUNTIME_DIR" -maxdepth 1 -name 'wayland-*' -exec rm -f {} + 2>/dev/null
    log "starting GNOME Shell"
    # llcapture.so spares the shell a second painting of every frame (see llcapture.c)
    # LINUXLAB_GNOME_PRELOAD in /etc/default/linuxlab-gnome goes before it: a recorder such as dev/lltrace.c
    preload=${LINUXLAB_GNOME_PRELOAD:-}
    [ -f $D/llcapture.so ] && preload="${preload:+$preload:}$D/llcapture.so"
    env ${preload:+LD_PRELOAD=$preload} gnome-shell --headless --virtual-monitor "$SIZE@$REFRESH" ${LINUXLAB_GNOME_SHELL_ARGS:-} >>"$LOG/shell.log" 2>&1 &
    SHELL_PID=$!
    SHELL_STARTED=$(date +%s)
    for _ in $(seq 300); do
        [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && return 0
        kill -0 $SHELL_PID 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

cleanup() {
    log "session ends"
    kill $BRIDGE_PID $SHELL_PID $PIDS 2>/dev/null
    # Wait for these alone: the system bus is a child too, and it never ends. One that does not go
    # in five seconds is made to.
    (
        sleep 5
        kill -9 $BRIDGE_PID $SHELL_PID $PIDS 2>/dev/null
    ) &
    wait $BRIDGE_PID $SHELL_PID $PIDS 2>/dev/null
}
trap cleanup EXIT
trap 'exit 0' INT
# what startlxqt does with the codes: 82 starts the desktop anew, 83 closes Linux Lab
trap 'case "$(cat "$END" 2>/dev/null)" in restart) exit 82 ;; poweroff) exit 83 ;; *) exit 0 ;; esac' TERM

start_shell || { log "GNOME Shell did not start"; exit 1; }
export WAYLAND_DISPLAY=wayland-0
dbus-update-activation-environment WAYLAND_DISPLAY

# the settings daemons gnome-session would start; the rest need hardware services that do not exist here
# (gsd-xsettings is started by the shell extension, which knows the display of Xwayland).
# They go with the display when the shell dies; a copy that is still running keeps its bus name, and
# the new one gives up.
start_daemons() {
    for daemon in gsd-media-keys gsd-keyboard gsd-sound gsd-datetime gsd-housekeeping gsd-a11y-settings gsd-screensaver-proxy; do
        [ -x /usr/libexec/$daemon ] || continue
        /usr/libexec/$daemon >>"$LOG/daemons.log" 2>&1 &
        PIDS="$PIDS $!"
    done
}
start_daemons

# what the user and the packages put into the autostart directories
python3 $D/autostart.py >>"$LOG/daemons.log" 2>&1 &

# The screen file Linux Lab reads (Xvfb -fbdir, see start-desktop.sh): the bridge swaps whole frames into it,
# so that the app never copies a half-written one. Without it the bridge draws through X and windows tear.
FRAMEBUFFER_ARGS=
FB=/home/XvfbScreen/Xvfb_screen0
[ -f "$FB" ] && [ "${LINUXLAB_GNOME_FRAMEBUFFER:-1}" = 1 ] && FRAMEBUFFER_ARGS="--framebuffer $FB"

# The screen and the input, restarted if the bridge dies while the shell lives.
# A shell that dies is started again: a black tablet helps nobody. Its programs are gone with it, the
# desktop comes back. Three deaths in a row, each within 45 seconds of the start, end the session, and
# Honor's LXQt comes up instead (see startlxqt).
failures=0
quick=0
while :; do
    while kill -0 $SHELL_PID 2>/dev/null; do
        started=$(date +%s)
        $D/llbridge --display "$X_DISPLAY" $FRAMEBUFFER_ARGS ${LINUXLAB_GNOME_BRIDGE_ARGS:-} >>"$LOG/bridge.log" 2>&1 &
        BRIDGE_PID=$!
        wait -n $BRIDGE_PID $SHELL_PID
        ended=$?
        kill -0 $SHELL_PID 2>/dev/null || break
        if [ $ended = 75 ]; then
            # the monitors were changed (a turn, another scale), and the bridge asks for a new stream
            continue
        fi
        [ $(($(date +%s) - started)) -lt 10 ] && failures=$((failures + 1)) || failures=0
        if [ $failures -ge 5 ]; then
            log "the bridge keeps dying, giving up"
            exit 1
        fi
        log "bridge exited, starting it again"
        sleep 1
    done
    wait $SHELL_PID
    code=$?
    lived=$(($(date +%s) - SHELL_STARTED))
    log "GNOME Shell exited with $code after $lived s"
    # Killed by a signal: what can still be learned is kept. 137 is SIGKILL, which leaves no word in
    # the shell's own log; the list of kills comes from llcompat.so, which sees every SIGKILL a
    # program of Linux sends. An empty list means the kill came from outside: the kernel, PRoot, Android.
    if [ $code -ge 128 ]; then
        {
            echo "GNOME Shell (pid $SHELL_PID) ended with $code at $(date '+%F %T'), after $lived s"
            echo "--- SIGKILLs sent by programs of Linux in the last 30 s (/tmp/llcompat-kill.log):"
            awk -v t="$(date +%s)" '{ split($1, a, "."); if (a[1] >= t - 30) print }' /tmp/llcompat-kill.log 2>/dev/null
            echo "--- memory:"
            grep -E 'MemTotal|MemAvailable|SwapFree' /proc/meminfo
            echo "--- the largest programs (resident kB):"
            ps -eo rss=,comm= --sort=-rss 2>/dev/null | head -8
            echo "--- the bridge:"
            tail -8 "$LOG/bridge.log"
            echo "--- the last lines of the shell:"
            tail -25 "$LOG/shell.log"
        } >"$LOG/crash-$(date +%m%d-%H%M%S).txt" 2>&1
    fi
    [ $lived -lt 45 ] && quick=$((quick + 1)) || quick=0
    if [ $quick -ge 3 ]; then
        log "GNOME Shell keeps dying, giving up"
        exit 1
    fi
    # the bridge belongs to the dead shell; on its way out it puts the screen file of Xvfb back
    kill $BRIDGE_PID 2>/dev/null
    wait $BRIDGE_PID 2>/dev/null
    start_shell || { log "GNOME Shell did not start again"; exit 1; }
    start_daemons
done
