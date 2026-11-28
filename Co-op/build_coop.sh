#!/bin/bash
# Builds the co-op player folder from this source, the same files as the release zip (no game files involved):
#   build/coop-dist/Co-op/   savagewind.exe, Savage Wind Co-op.exe, gamelib.id, py/ (portable Python + oead),
#                            build_overlay.py, astree_tables.py, COOP-HOWTO.md, CHANGELOG.md, licenses, manifest.json
#   build/coop-dist/SavageWind-Coop.zip
# Run it through Co-op\build_coop.ps1 (it checks the tools first), or from Git Bash / MSYS2 UCRT64 in the repo root:
#   bash Co-op/build_coop.sh
# Needs (tools\setup_windows.ps1 installs them): MSYS2 UCRT64 at C:\msys64 (clang, make, windres) and the python.org
# Python 3.12. Downloads only the Python package oead (from PyPI, with pip) the first time.
set -euo pipefail
PROTOCOL=101
# The game code library id this co-op build must match: the one Savage Wind 0.6.0-alpha.6 builds on the player's PC.
# If it differs, the files the id is made from changed (or the clone has CRLF line endings) and the launcher would
# refuse to start, so the build stops.
EXPECTED_GAMELIB_ID=f63df6b1a6001541

cd "$(dirname "$0")/.."
root=$(pwd)
VERSION=$(tr -d '\r\n' < Co-op/VERSION 2>/dev/null || echo 0.1.0)
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
for t in clang make windres; do command -v $t >/dev/null || { echo "missing $t: run tools\\setup_windows.ps1 first"; exit 1; }; done
py312="${PY312:-$(cygpath -u "$LOCALAPPDATA")/Programs/Python/Python312/python.exe}"
[[ -x $py312 ]] || { echo "missing Python 3.12 ($py312): run tools\\setup_windows.ps1 first, or set PY312"; exit 1; }

echo "== 1/4 the game program (savagewind.exe with the co-op code; no game code is built or needed)"
make -j"$(nproc)" build/savagewind.exe
id=$(make -s -f Makefile -f - print-coop-gamelib-id <<<'print-coop-gamelib-id: ; @echo $(GAMELIB_ID)')
if [[ $id != "$EXPECTED_GAMELIB_ID" ]]; then
    echo "game library id $id, expected $EXPECTED_GAMELIB_ID: the clone must have LF line endings"
    echo "(git config core.autocrlf false, then check out again) and the generator files must be unchanged."
    exit 1
fi

echo "== 2/4 the starter program (Savage Wind Co-op.exe)"
mkdir -p build/coop-launcher
(cd Co-op && windres -O coff launcher/coop_launcher.rc -o ../build/coop-launcher/rc.o)
clang -O2 -municode -mwindows Co-op/launcher/coop_launcher.c build/coop-launcher/rc.o -lwinhttp -lbcrypt -o "build/coop-launcher/Savage Wind Co-op.exe"

echo "== 3/4 portable Python with oead (for the one-time setup on the player's PC)"
venv=build/coop-py-venv
[[ -x $venv/Scripts/python.exe ]] || "$py312" -m venv "$venv"
"$venv/Scripts/python.exe" -m pip install -q --disable-pip-version-check oead==1.3.0
COOP_PY_OUT="$(cygpath -w "$root/build/coop-py")" COOP_PY_SITE="$(cygpath -w "$root/$venv/Lib/site-packages")" \
    "$venv/Scripts/python.exe" Co-op/make_portable_py.py

echo "== 4/4 the Co-op folder and zip"
dist=build/coop-dist
rm -rf -- "$dist" && mkdir -p "$dist/Co-op"
out=$dist/Co-op
cp build/savagewind.exe "$out/"
echo "$id" > "$out/gamelib.id"
cp LICENSE "$out/LICENSE-SavageWind.txt"
cp "build/coop-launcher/Savage Wind Co-op.exe" Co-op/build_overlay.py Co-op/astree_tables.py Co-op/COOP-HOWTO.md \
    Co-op/CHANGELOG.md "$out/"
cp -r build/coop-py "$out/py"
"$venv/Scripts/python.exe" - "$(cygpath -w "$out")" "$VERSION" "$PROTOCOL" <<'EOF'
import hashlib, json, os, sys
out, version, protocol = sys.argv[1], sys.argv[2], int(sys.argv[3])
files = {}
for name in sorted(os.listdir(out)):
    p = os.path.join(out, name)
    if os.path.isfile(p):
        files[name] = hashlib.sha256(open(p, 'rb').read()).hexdigest().upper()
json.dump({'protocol': protocol, 'version': version, 'files': files}, open(os.path.join(out, 'manifest.json'), 'w'), indent=2)
EOF
(cd "$dist" && /c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "Compress-Archive -Path Co-op -DestinationPath SavageWind-Coop.zip -Force")
sha256sum "$dist/SavageWind-Coop.zip" | awk '{print $1}' > "$dist/SavageWind-Coop.zip.sha256"
echo
echo "Done: $root/$dist/Co-op (and SavageWind-Coop.zip + .sha256 next to it)."
echo "Put that Co-op folder inside your Savage Wind folder, next to savagewind.exe, and start Co-op\\Savage Wind Co-op.exe."
echo "Online play: everyone needs the same co-op version ($VERSION, protocol $PROTOCOL)."
