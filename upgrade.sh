#!/bin/bash
# Ubuntu 26.04 for Linux Lab: takes the Ubuntu 24.04 that Honor ships to 26.04, with apt alone.
#
# Run by the installer of the helper app (host/installer-head.sh), as root, before install.sh.
# do-release-upgrade is of no use here: it wants an init system and a reboot, and knows no way from
# 24.04 straight to 26.04. What is done instead is what worked by hand on the tablet, in that order:
#
# - apt is pointed at 26.04 in a file of its own. Linux Lab writes /etc/apt/sources.list anew, with
#   24.04 in it, on every start of the desktop; apt is told not to read that file.
# - The Rust versions of sudo and of the core utilities, which 26.04 picks, are kept out: under PRoot
#   the C ones are the ones known to work.
# - Honor replaced /usr/bin/pcmanfm-qt with a script and keeps the program next to it; a diversion
#   sends the new program there too.
# - One full upgrade cannot be planned by apt ("Conf Broken coreutils-from-gnu, sudo-common"), so it
#   goes in steps: what can be upgraded in place, then the two packages by name, then the rest.
# - Honor holds its Firefox (the one of Ubuntu is a snap, and snaps need systemd); it stays held.
#
# Every step can be done twice, so a run that was cut short is simply started again.
set -e
STATE=/var/lib/linuxlab-helper
export DEBIAN_FRONTEND=noninteractive UCF_FORCE_CONFFOLD=1 NEEDRESTART_SUSPEND=1
APT=(apt-get -y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold -o Acquire::Retries=5)

step() { echo "=== $(date +%T) $*"; }
version() { (. /etc/os-release && echo "$VERSION_ID"); }

[ "$(id -u)" = 0 ] || { echo "upgrade.sh: run as root"; exit 1; }
mkdir -p $STATE
if [ "$(version)" = 26.04 ] && [ ! -e $STATE/upgrade-started ]; then
    step "this is Ubuntu 26.04 already"
    exit 0
fi
if [ "$(version)" != 24.04 ] && [ ! -e $STATE/upgrade-started ]; then
    echo "upgrade.sh: this is Ubuntu $(version); only 24.04 can be taken to 26.04"
    exit 1
fi

if [ ! -e $STATE/upgrade-started ]; then
    step "a copy of /etc as it is in 24.04: /var/backups/linuxlab-etc-24.04.tgz"
    mkdir -p /var/backups
    tar -C / -czf /var/backups/linuxlab-etc-24.04.tgz etc

    step "the updates of 24.04"
    apt-get update
    "${APT[@]}" full-upgrade
else
    step "an upgrade was cut short here: going on with it"
    dpkg --configure -a || true
fi
touch $STATE/upgrade-started

step "apt is pointed at 26.04"
cat > /etc/apt/sources.list.d/ubuntu.sources <<'SRC'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-backports resolute-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
SRC
echo "# left empty on purpose, see /etc/apt/apt.conf.d/99linuxlab-resolute" > /etc/apt/sources.list.linuxlab-ignored
cat > /etc/apt/apt.conf.d/99linuxlab-resolute <<'CONF'
// Linux Lab rewrites /etc/apt/sources.list with noble on every start of the desktop.
// The sources live in /etc/apt/sources.list.d/ubuntu.sources, the rewritten file is not read.
Dir::Etc::SourceList "/etc/apt/sources.list.linuxlab-ignored";
CONF
cat > /etc/apt/preferences.d/linuxlab-resolute <<'PREF'
# Under proot keep the C sudo and GNU coreutils, not the Rust ones that 26.04 picks by default.
Package: sudo-rs rust-coreutils coreutils-from-uutils
Pin: version *
Pin-Priority: -1
PREF
apt-get update

# Honor's Firefox comes from Mozilla and is held; the "firefox" of Ubuntu only installs a snap
case "$(dpkg-query -W -f '${Version}' firefox 2>/dev/null)" in
"" | *snap*) ;;
*) apt-mark hold firefox >/dev/null ;;
esac

if [ -e /usr/bin/pcmanfm-qt.real ] && [ "$(head -c 2 /usr/bin/pcmanfm-qt 2>/dev/null)" = "#!" ] &&
    ! dpkg-divert --list /usr/bin/pcmanfm-qt | grep -q .; then
    step "Honor's start script of the file manager stays, the new program goes next to it"
    dpkg-divert --local --divert /usr/bin/pcmanfm-qt.real --add /usr/bin/pcmanfm-qt
fi

step "what can be upgraded in place"
"${APT[@]}" upgrade
step "the core utilities and sudo, the C ones"
"${APT[@]}" install coreutils-from-gnu
"${APT[@]}" install sudo
step "the rest"
"${APT[@]}" full-upgrade
if dpkg-query -W -f '${Status}' fcitx5 2>/dev/null | grep -q "ok installed"; then
    # the input method of Honor's desktop: its Qt 6 part is not pulled in by the upgrade
    "${APT[@]}" install fcitx5-frontend-qt6
fi

step "checks"
[ "$(version)" = 26.04 ] || { echo "upgrade.sh: still Ubuntu $(version)"; exit 1; }
dpkg --audit
apt-get check
# what Linux Lab starts on its own: one of these without its libraries and the desktop does not come up
broken=
for program in /usr/local/bin/Xvfb lxqt-session lxqt-panel openbox /usr/bin/pcmanfm-qt.real qterminal pulseaudio pactl \
    xdpyinfo xrdb python3 sudo dbus-launch firefox; do
    path=$(command -v "$program" 2>/dev/null) || continue
    if ldd "$path" 2>/dev/null | grep -q "not found"; then
        broken="$broken $program"
        ldd "$path" | grep "not found"
    fi
done
if [ -n "$broken" ]; then
    echo "upgrade.sh: programs with missing libraries:$broken"
    exit 1
fi
bash -n /usr/local/bin/start-desktop.sh
apt-get clean
rm -f $STATE/upgrade-started
step "Ubuntu $(version)"
