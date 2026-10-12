#!/bin/bash
# Linux Lab Helper: the Linux side. Run inside Linux Lab, in a terminal:
#
#   bash /tablet/Download/linuxlab-helper.sh          the touch module alone
#   bash /tablet/Download/linuxlab-helper.sh ubuntu   Ubuntu 26.04 with its GNOME desktop, and the module
#   bash /tablet/Download/linuxlab-helper.sh update   the Ubuntu desktop and the module of this helper version
#   bash /tablet/Download/linuxlab-helper.sh lxqt     Honor's desktop again (nothing is removed)
#   bash /tablet/Download/linuxlab-helper.sh gnome    the Ubuntu desktop again
#   bash /tablet/Download/linuxlab-helper.sh remove   remove the touch module
#
# The touch module is lltouch.so, a small library for the X server of Linux Lab that receives touches
# from the Linux Lab Helper app. It is loaded through /etc/ld.so.preload and does nothing in any
# program but Xvfb.
#
# "ubuntu" takes the Ubuntu 24.04 of Linux Lab to 26.04 and sets up the GNOME desktop of a freshly
# installed Ubuntu, with everything it needs to live inside Linux Lab (gnome/upgrade.sh and
# gnome/install.sh of the project, packed below with the module). It takes 20 to 40 minutes, wants
# 6 GB of free space and a network, and goes on if the terminal is closed: the same command shows how
# far it is. Everything it prints is kept in /var/log/linuxlab-helper.log.
#
# "update" brings an Ubuntu desktop set up by an earlier version of the helper to this one: the same
# install.sh, without the upgrade and with the desktop that was chosen. A few minutes, with a network.
set -e
umask 022
self=$(readlink -f "$0")
lib=/usr/local/lib/lltouch.so
state=/var/lib/linuxlab-helper
log=/var/log/linuxlab-helper.log
# what the Linux side of this installer is made of (make_installer.sh puts it here); written to $payload_file
# when it is installed, and lldesktop.py tells the helper app, which offers an update while it differs
payload=unknown
payload_file=/opt/linuxlab-gnome/payload
# the release this installer belongs to (host/version), written to $release_file: llupdate.py offers
# a release of the repository with a higher serial
version=unknown
serial=0
release_file=/opt/linuxlab-gnome/release

# in the language of the desktop: say "English" "Russian"
say() {
    case "${LANG:-}${LANGUAGE:-}" in *ru*) echo "$2" ;; *) echo "$1" ;; esac
}
die() {
    say "Linux Lab Helper: $1" "Linux Lab Helper: $2" >&2
    exit 1
}
unpack() {
    # Python and not tar: the tar of Ubuntu 26.04 cannot unpack below the top directory inside Linux
    # Lab until llcompat.so, which is in this very archive, is installed (see gnome/llcompat.c)
    sed '1,/^__PAYLOAD__$/d' "$self" | base64 -d | python3 -c '
import sys, tarfile
tarfile.open(fileobj=sys.stdin.buffer, mode="r|gz").extractall(sys.argv[1], filter="data")' "$1"
}

remove_old_helper() {
    # leftovers of the first version, which had a Python helper
    rm -f "$HOME/.config/autostart/llhost.desktop" /usr/local/sbin/Xvfb
    pkill -f 'llhost\.py' 2>/dev/null || true
    rm -rf "$HOME/.local/share/llhost"
}

install_module() {
    mkdir -p /usr/local/lib
    cp "$1" "$lib.new"
    chmod 755 "$lib.new"
    mv "$lib.new" "$lib"
    # the module switches itself off after a start that failed; a fresh install tries again
    rm -f /usr/local/lib/lltouch.starting /usr/local/lib/lltouch.failed
    grep -qxF "$lib" /etc/ld.so.preload 2>/dev/null || echo "$lib" >> /etc/ld.so.preload
    # Firefox only listens to touch events when asked to
    echo 'export MOZ_USE_XINPUT2=1' > /etc/profile.d/lltouch.sh
    remove_old_helper
}

# apt wants root; the user of Linux Lab may become root without a password. The language goes through
# env: the sudo of the chroot mode up to helper 0.10 took "LANG=..." for the command to run.
if [ "$(id -u)" != 0 ] && sudo -n true 2>/dev/null; then
    exec sudo -n env LANG="${LANG:-}" LANGUAGE="${LANGUAGE:-}" bash "$self" "$@"
fi

case "${1:-module}" in
remove)
    if [ -f /etc/ld.so.preload ]; then
        grep -vxF "$lib" /etc/ld.so.preload > /etc/ld.so.preload.new || true
        if [ -s /etc/ld.so.preload.new ]; then
            mv /etc/ld.so.preload.new /etc/ld.so.preload
        else
            rm -f /etc/ld.so.preload.new /etc/ld.so.preload
        fi
    fi
    rm -f "$lib" /usr/local/lib/lltouch.starting /usr/local/lib/lltouch.failed /etc/profile.d/lltouch.sh
    remove_old_helper
    say "Linux Lab Helper: module removed. Close Linux Lab and open it again." \
        "Linux Lab Helper: модуль удалён. Закройте Linux Lab и откройте снова."
    exit 0
    ;;
gnome | lxqt)
    [ -f /opt/linuxlab-gnome/lldesktop.py ] ||
        die "the Ubuntu desktop is not installed: run this file with the word ubuntu" \
            "рабочий стол Ubuntu не установлен: запустите этот файл со словом ubuntu"
    exec python3 /opt/linuxlab-gnome/lldesktop.py "$1"
    ;;
module | ubuntu | update | ubuntu-run) ;;
*)
    sed -n '2,9p' "$self"
    exit 2
    ;;
esac

[ "$(uname -m)" = aarch64 ] ||
    die "this is for aarch64, this system is $(uname -m)." "это для aarch64, а здесь $(uname -m)."
if ! command -v Xvfb >/dev/null 2>&1 && [ ! -x /usr/local/bin/Xvfb ] && [ ! -x /usr/bin/Xvfb ]; then
    die "no Xvfb here. Run this inside Linux Lab." "здесь нет Xvfb. Запустите это внутри Linux Lab."
fi

case "${1:-module}" in
module)
    work=$(mktemp -d)
    unpack "$work"
    install_module "$work/lltouch.so"
    rm -rf "$work"
    say "Linux Lab Helper: module installed ($(wc -c < "$lib") bytes). Close Linux Lab and open it again." \
        "Linux Lab Helper: модуль установлен ($(wc -c < "$lib") байт). Закройте Linux Lab и откройте снова."
    ;;
ubuntu | update)
    [ "$(id -u)" = 0 ] || die "this needs root, and sudo asks for a password here" "нужны права root, а sudo здесь просит пароль"
    version=$(. /etc/os-release && echo "$ID $VERSION_ID")
    case "$version" in
    "ubuntu 24.04") need=6000000 ;;
    "ubuntu 26.04") need=3000000 ;;
    *) die "this is $version; it works from Ubuntu 24.04, the one Linux Lab comes with" \
        "здесь $version; нужна Ubuntu 24.04, с которой Linux Lab поставляется" ;;
    esac
    if [ "$1" = update ]; then
        [ "$version" = "ubuntu 26.04" ] && [ -f /opt/linuxlab-gnome/lldesktop.py ] ||
            die "there is no Ubuntu desktop to update: run this file with the word ubuntu" \
                "обновлять нечего, рабочий стол Ubuntu не установлен: запустите этот файл со словом ubuntu"
        need=1000000
    fi
    mkdir -p $state
    if [ -s $state/pid ] && kill -0 "$(cat $state/pid)" 2>/dev/null; then
        say "The installation is running, this is how far it is:" "Установка идёт, вот где она сейчас:"
        from=-20
    else
        free=$(df -Pk / | awk 'NR == 2 {print $4}')
        [ "$free" -ge $need ] || die "$((need / 1000000)) GB of free space are needed, there are $((free / 1000000))" \
            "нужно $((need / 1000000)) ГБ свободного места, а есть $((free / 1000000))"
        # a copy: the run must not depend on the shared storage of the tablet, nor on this terminal
        if [ "$self" != $state/linuxlab-helper.sh ]; then
            cp "$self" $state/linuxlab-helper.sh.new
            mv $state/linuxlab-helper.sh.new $state/linuxlab-helper.sh
        fi
        rm -f $state/result
        touch $log
        from=+$(($(wc -l < $log) + 1))
        setsid nohup bash $state/linuxlab-helper.sh ubuntu-run "$1" >> $log 2>&1 < /dev/null &
        echo $! > $state/pid
        if [ "$1" = update ]; then
            say "Updating the Ubuntu desktop, a few minutes. Closing this terminal does not stop it." \
                "Обновляю рабочий стол Ubuntu, несколько минут. Если закрыть этот терминал, обновление продолжится."
        else
            say "Installing Ubuntu 26.04 with GNOME. Keep Linux Lab open; closing this terminal does not stop it." \
                "Ставлю Ubuntu 26.04 с GNOME. Держите Linux Lab открытым; если закрыть этот терминал, установка продолжится."
        fi
    fi
    tail -n $from -f --pid="$(cat $state/pid)" $log || true
    if [ "$(cat $state/result 2>/dev/null)" = ok ] && [ "$1" = update ]; then
        say "Linux Lab Helper: updated. Close Linux Lab and open it again." \
            "Linux Lab Helper: обновлено. Закройте Linux Lab и откройте снова."
    elif [ "$(cat $state/result 2>/dev/null)" = ok ]; then
        say "Linux Lab Helper: done. Close Linux Lab and open it again: it starts with the Ubuntu desktop. The switch back to Honor's desktop is in the Linux Lab Helper app." \
            "Linux Lab Helper: готово. Закройте Linux Lab и откройте снова: он запустится с рабочим столом Ubuntu. Переключатель обратно на рабочий стол Honor находится в приложении Linux Lab Helper."
    else
        die "it did not finish. Run the same command again to go on; the log is $log (a copy is in Downloads)." \
            "установка не завершилась. Повторите ту же команду, чтобы продолжить; журнал лежит в $log (копия в «Загрузках»)."
    fi
    ;;
ubuntu-run)
    # the work itself, apart from any terminal (started by "ubuntu" above)
    echo $$ > $state/pid
    finish() {
        code=$?
        if [ $code != 0 ]; then
            echo failed > $state/result
            echo "=== $(date +%T) FAILED with code $code"
        fi
        cp $log /tablet/Download/linuxlab-helper.log 2>/dev/null || true
        rm -f $state/pid
    }
    trap finish EXIT
    what=${2:-ubuntu}
    echo "=== $(date '+%F %T') Linux Lab Helper $version: $what, Ubuntu 26.04 with GNOME ($payload)"
    rm -rf $state/src.new
    mkdir -p $state/src.new
    unpack $state/src.new
    rm -rf $state/src
    mv $state/src.new $state/src
    if [ "$what" = update ]; then
        bash $state/src/gnome/install.sh update
    else
        bash $state/src/gnome/upgrade.sh
        bash $state/src/gnome/install.sh
    fi
    install_module $state/src/lltouch.so
    echo "$payload" > $payload_file.new
    mv $payload_file.new $payload_file
    echo "$serial $version" > $release_file.new
    mv $release_file.new $release_file
    echo ok > $state/result
    echo "=== $(date +%T) done"
    ;;
esac
exit 0
__PAYLOAD__
