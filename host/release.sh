#!/usr/bin/env bash
# Publishes a release of the Linux side as the GitHub release v<version> of SpazzRabbit/linuxlab-ubuntu,
# where llupdate.py looks: the installer, manifest.json (version, serial, the installer's size and
# SHA-256, the notes) and manifest.json.sig.
#
#   host/release.sh    version and serial from host/version, notes from host/notes/<version>.json
#                      ({"ru": ..., "en": ...}: one or two sentences for the notification)
#
# The secret key lives on the computer releases are made on, in ~/.config/linuxlab-release/gnupg
# (or $LINUXLAB_RELEASE_GNUPG); its public half is release-key.gpg, which every installation has.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=SpazzRabbit/linuxlab-ubuntu
export GNUPGHOME=${LINUXLAB_RELEASE_GNUPG:-$HOME/.config/linuxlab-release/gnupg}
read -r version serial < host/version
notes=host/notes/$version.json
[ -f "$notes" ] || { echo "release: no $notes" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "release: commit first, a release is made of what is committed" >&2; exit 1; }
# llupdate.py offers only a serial higher than the one installed
published=$(curl -fsL "https://github.com/$repo/releases/latest/download/manifest.json" |
    python3 -c 'import json, sys; print(json.load(sys.stdin)["serial"])' 2>/dev/null || echo 0)
[ "$serial" -gt "$published" ] || { echo "release: serial $serial is not above $published, raise it in host/version" >&2; exit 1; }

host/make_installer.sh >/dev/null
python3 - "$version" "$serial" "$notes" >build/manifest.json <<'PY'
import hashlib, json, sys
version, serial, notes = sys.argv[1], int(sys.argv[2]), json.load(open(sys.argv[3]))
data = open("build/linuxlab-helper.sh", "rb").read()
payload = next(line.split("=", 1)[1] for line in data.decode().splitlines() if line.startswith("payload="))
json.dump({"serial": serial, "version": version, "tag": "v" + version, "payload": payload,
           "installer": {"name": "linuxlab-helper.sh", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()},
           "notes": {"ru": notes["ru"], "en": notes["en"]}}, sys.stdout, ensure_ascii=False, indent=1)
print()
PY
gpg --batch --yes --detach-sign -o build/manifest.json.sig build/manifest.json
gpgv --keyring ./release-key.gpg build/manifest.json.sig build/manifest.json 2>/dev/null

body=$(python3 -c 'import json, sys; n = json.load(open(sys.argv[1])); print(n["ru"] + "\n\n" + n["en"])' "$notes")
git tag -a "v$version" -m "Linux Lab Helper $version, Linux side"
git push origin "v$version"
gh release create "v$version" -R "$repo" --verify-tag --title "Linux Lab Helper $version" --notes "$body" \
    build/linuxlab-helper.sh build/manifest.json build/manifest.json.sig
