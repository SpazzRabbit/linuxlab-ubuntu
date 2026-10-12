#!/usr/bin/env bash
# Glue the installer: the shell part, then the touch module and this repository (what Linux needs of
# it) as a compressed archive in base64. Output: build/linuxlab-helper.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
# what the payload is made of: the contents of its files, so that the same files give the same mark
payload=$({
    (cd host/prebuilt && sha256sum lltouch.so)
    find . \( -path ./dev -o -path ./host -o -path ./build -o -path ./.git -o -name __pycache__ \) -prune \
        -o -type f ! -name .gitignore -print | LC_ALL=C sort | xargs -d '\n' sha256sum
} | sha256sum | cut -c1-12)
# the release: "<version> <serial>", the serial only ever grows
read -r version serial < host/version
{
    sed -e "s/^payload=unknown\$/payload=$payload/" -e "s/^version=unknown\$/version=$version/" \
        -e "s/^serial=0\$/serial=$serial/" host/installer-head.sh
    tar -cz --owner=0 --group=0 --sort=name \
        -C "$PWD/host/prebuilt" lltouch.so \
        -C "$PWD" --exclude=./dev --exclude=./host --exclude=./build --exclude=./.git --exclude=./.gitignore \
        --exclude=__pycache__ --transform 's,^\.,gnome,' . | base64 -w 76
} > build/linuxlab-helper.sh
bash -n build/linuxlab-helper.sh
grep -q "^payload=$payload\$" build/linuxlab-helper.sh
grep -q "^serial=$serial\$" build/linuxlab-helper.sh
ls -la build/linuxlab-helper.sh
