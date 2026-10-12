#!/bin/bash
# Ubuntu desktop (GNOME Shell 50) for HONOR Linux Lab. Run inside Linux Lab, then restart Linux Lab:
#
#   sudo bash install.sh          install and make GNOME the desktop
#   sudo bash install.sh update   install again, the desktop that was chosen stays
#   sudo bash install.sh lxqt     back to Honor's LXQt (nothing is removed)
#   sudo bash install.sh gnome    GNOME again
#
# Needs Ubuntu 26.04 in Linux Lab (upgrade.sh takes Honor's 24.04 there) and about 2 GB of space.
# The installer the helper app of Android puts into Downloads runs both, see host/.
# What it sets up and why is told in README.md next to this file.
set -e
src=$(dirname "$(readlink -f "$0")")
D=/opt/linuxlab-gnome
EXT=/usr/share/gnome-shell/extensions/linuxlab@linuxlab-touchfix

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

case "${1:-install}" in
lxqt | gnome)
    # at once when Linux Lab was started with the startlxqt of this directory, else at its next start
    if [ -f $D/lldesktop.py ]; then
        exec python3 $D/lldesktop.py "$1"
    fi
    echo "$1" > /etc/linuxlab-desktop
    echo "desktop: $1. Close Linux Lab and open it again."
    exit 0
    ;;
install | update) ;;
*)
    echo "usage: install.sh [update|lxqt|gnome]"
    exit 2
    ;;
esac

. /etc/os-release
if [ "$VERSION_ID" != 26.04 ]; then
    echo "this is Ubuntu $VERSION_ID; GNOME 50 needs 26.04, upgrade first (upgrade.sh)"
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
APT=(apt-get -y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold --no-install-recommends install)

echo "== packages: the desktop of a fresh Ubuntu"
# No recommended packages: they bring gdm3, snapd, NetworkManager, printing and other services
# that need an init system. The lists name what a default installation shows to the user.
apt-get update
"${APT[@]}" \
    gnome-shell mutter xwayland pipewire wireplumber dbus-user-session \
    ubuntu-session ubuntu-settings gsettings-ubuntu-schemas gnome-shell-ubuntu-extensions \
    gnome-shell-extension-ubuntu-dock gnome-shell-extension-appindicator \
    gnome-shell-extension-desktop-icons-ng gnome-shell-extension-ubuntu-tiling-assistant \
    yaru-theme-gnome-shell yaru-theme-gtk yaru-theme-icon yaru-theme-sound ubuntu-wallpapers \
    fonts-ubuntu fonts-noto-core fonts-noto-color-emoji fonts-liberation \
    gnome-control-center gnome-keyring gnome-menus gir1.2-gmenu-3.0 gir1.2-dbusmenu-glib-0.4 \
    xdg-desktop-portal-gnome xdg-user-dirs-gtk gvfs-backends evolution-data-server \
    accountsservice polkitd aptdaemon language-selector-gnome \
    ibus ibus-gtk3 ibus-gtk4 gnome-session-canberra libcanberra-pulse libglib2.0-bin python3-gi python3-dbus gir1.2-nm-1.0 \
    update-manager software-properties-gtk update-notifier
"${APT[@]}" \
    nautilus ptyxis gnome-text-editor gnome-calculator gnome-characters gnome-clocks \
    gnome-disk-utility gnome-font-viewer gnome-logs loupe papers baobab seahorse resources \
    yelp ubuntu-docs gnome-user-docs
echo "== packages: to build the two small programs of this directory"
"${APT[@]}" gcc libc6-dev pkg-config libglib2.0-dev libpipewire-0.3-dev libx11-dev libxi-dev libxfixes-dev
# sudo of the chroot mode of a rooted tablet (sudo-chroot); its TCP flavour needs no System V IPC
"${APT[@]}" fakeroot

echo "== build"
mkdir -p $D
# Every file is put in place in one step (mv): the session of a running desktop reads its script
# as it goes, and a file changed under it would be run from the middle of a line.
put() {
    cp "$1" "$2.new"
    chmod "${3:-644}" "$2.new"
    mv "$2.new" "$2"
}
for f in llcompat.c llbridge.c llcapture.c llcapture.h system-bus.conf; do
    put "$src/$f" "$D/$f"
done
put "$src/gnome-session.sh" $D/gnome-session.sh 755
put "$src/service-helper" $D/service-helper 755
put "$src/aptd" $D/aptd 755
# service-helper looks here first: aptdaemon is started through the wrapper (see aptd)
mkdir -p $D/system-services
sed "s#^Exec=/usr/sbin/aptd#Exec=$D/aptd#" /usr/share/dbus-1/system-services/org.debian.apt.service >$D/system-services/org.debian.apt.service.new
mv $D/system-services/org.debian.apt.service.new $D/system-services/org.debian.apt.service
put "$src/autostart.py" $D/autostart.py 755
put "$src/llnetwork.py" $D/llnetwork.py 755
put "$src/llsession.py" $D/llsession.py 755
put "$src/lloff.py" $D/lloff.py 755
put "$src/llandroid.py" $D/llandroid.py 755
put "$src/lldesktop.py" $D/lldesktop.py 755
# updates from the releases on GitHub, signed with the key whose public half is release-key.gpg
put "$src/llupdate.py" $D/llupdate.py 755
put "$src/release-key.gpg" $D/release-key.gpg
# the chroot mode: the PRoot side of it (the root side, llroot, comes with the helper app)
put "$src/llchroot.py" $D/llchroot.py 755
put "$src/sudo-chroot" $D/sudo-chroot 755
mkdir -p $D/bin
put "$src/bin/systemd-run" $D/bin/systemd-run 755
# gsd-xsettings runs the scripts of /etc/xdg/Xwayland-session.d when Xwayland comes up. This one loads
# the X11 modules into the PulseAudio that Honor starts, and they hold on to Xwayland: when the shell
# goes, PulseAudio writes "X11 I/O error exit handler called" to the output of the container, and the
# app of Linux Lab takes any line with " exit" in it for the end and stops playing sound.
# (Set aside into another directory: everything in that one is run, whatever its name.)
if [ -e /etc/xdg/Xwayland-session.d/00-pulseaudio-x11 ]; then
    mkdir -p $D/set-aside
    dpkg-divert --local --rename --divert $D/set-aside/00-pulseaudio-x11 --add /etc/xdg/Xwayland-session.d/00-pulseaudio-x11
fi
# A data directory of the GNOME session alone (gnome-session.sh puts it first in XDG_DATA_DIRS): an
# entry here with Hidden=true takes a program out of the app grid of GNOME and leaves LXQt as it was.
mkdir -p $D/share/applications
for app in $(grep -v '^#' "$src/hidden-apps"); do
    printf '[Desktop Entry]\nType=Application\nName=%s\nHidden=true\n' "$app" >$D/share/applications/$app.desktop
done
put "$src/tablet.desktop" $D/share/applications/tablet.desktop
mkdir -p /usr/local/share/applications
put "$src/bluetooth-panel.desktop" /usr/local/share/applications/gnome-bluetooth-panel.desktop
# fortify would turn open() into a macro and the replacements in llcompat.c could not be defined
gcc -O2 -fPIC -shared -Wall -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -o $D/llcompat.so.new $D/llcompat.c
mv $D/llcompat.so.new $D/llcompat.so
gcc -O2 -Wall -o $D/llbridge.new $D/llbridge.c \
    $(pkg-config --cflags --libs gio-unix-2.0 libpipewire-0.3 x11 xi xfixes)
mv $D/llbridge.new $D/llbridge
gcc -O2 -fPIC -shared -Wall -o $D/llcapture.so.new $D/llcapture.c -lm
mv $D/llcapture.so.new $D/llcapture.so
chmod -R u=rwX,go=rX $D

echo "== system"
# every process gets llcompat.so; the file is replaced in one step so no program ever sees half of it
grep -v -e "^$D/" /etc/ld.so.preload 2>/dev/null > /etc/ld.so.preload.new || true
echo "$D/llcompat.so" >> /etc/ld.so.preload.new
mv /etc/ld.so.preload.new /etc/ld.so.preload

mkdir -p $EXT
put "$src/extension/metadata.json" $EXT/metadata.json
put "$src/extension/extension.js" $EXT/extension.js
mkdir -p $EXT/icons
put "$src/extension/icons/linuxlab-helper-symbolic.svg" $EXT/icons/linuxlab-helper-symbolic.svg
put "$src/99_linuxlab.gschema.override" /usr/share/glib-2.0/schemas/99_linuxlab.gschema.override
glib-compile-schemas /usr/share/glib-2.0/schemas

# Firefox (see "Video in Firefox" in README.md): defaults that every profile gets, Honor's one included
if [ -d /usr/lib/firefox/defaults/pref ]; then
    put "$src/firefox-linuxlab.js" /usr/lib/firefox/defaults/pref/linuxlab.js
fi

# polkit, which the system services ask: the desktop user may do what sudo lets him do anyway
mkdir -p /etc/polkit-1/rules.d
put "$src/49-linuxlab.rules" /etc/polkit-1/rules.d/49-linuxlab.rules

put "$src/startlxqt" /usr/local/bin/startlxqt 755
if [ "${1:-install}" = install ] || [ ! -s /etc/linuxlab-desktop ]; then
    echo gnome > /etc/linuxlab-desktop
fi

echo
echo "Done. Close Linux Lab and open it again: it starts with the Ubuntu desktop."
echo "Honor's desktop again: the switch in the Linux Lab Helper app, or python3 $D/lldesktop.py lxqt"
