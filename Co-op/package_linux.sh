#!/bin/bash
# Builds the ready-made Linux co-op package (x86-64): no compiler or Python needed on the player's PC. No game code or
# game files are included: the program uses the game library that normal Savage Wind (the official Linux AppImage)
# builds on the player's PC from their own game files.
#   build/dist/SavageWind-Coop-linux-x86_64.tar.gz (+ .sha256), holding the Co-op/ folder
# Run it from the repository root inside the Ubuntu 22.04 build image of tools/linux/Containerfile, so the result runs
# on glibc 2.35+ distributions:
#   docker build -t savagewind-linux-build -f tools/linux/Containerfile tools/linux
#   docker run --rm -v "$PWD:/src" -w /src savagewind-linux-build bash Co-op/package_linux.sh
# The library part (tools/linux/build_deps.sh, as for Savage Wind's own AppImage) downloads pinned sources; the setup
# program needs oead and PyInstaller from PyPI.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)
ARCH=x86_64
L=$root/build/linux/$ARCH
P=$L/deps
B=$L/game
VERSION=$(tr -d '\r\n' < Co-op/VERSION)
dist=$root/build/dist
stage=$L/coop-package
out=$stage/Co-op
name=SavageWind-Coop-linux-$ARCH.tar.gz

echo "== 1/4 libraries"
tools/linux/build_deps.sh $ARCH

echo "== 2/4 the game program"
make -j"$(nproc)" ARCH=$ARCH B="$B" DEPS="$P" "$B/savagewind"
if nm "$B/savagewind" | grep ' f_70' >/dev/null; then echo "game code is linked in: not packaging"; exit 1; fi
# the ids of the game library this program accepts (normal and, for CPUs without AVX2, compatible)
ids=$(make -s ARCH=$ARCH -f Makefile -f - print-coop-ids <<<'print-coop-ids: ; @printf "%s\n" $(GAMELIB_ID) $(GAMELIB_ID_COMPAT)')
echo "game library ids: $(echo $ids)"

echo "== 3/4 the setup program (builds the co-op files from the player's game files)"
V=$L/coop-venv
[ -x "$V/bin/pyinstaller" ] || { rm -rf "$V" && python3.12 -m venv "$V" &&
    "$V/bin/python" -m pip install -q --disable-pip-version-check oead==1.3.0 pyinstaller==6.22.3; }
"$V/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --onedir --name coop-overlay \
    --paths Co-op --hidden-import astree_tables --distpath "$L/coop-overlay-dist" --workpath "$L/coop-pyinstaller" \
    --specpath "$L/coop-pyinstaller" Co-op/build_overlay.py

echo "== 4/4 the Co-op folder"
rm -rf "$stage" && mkdir -p "$out/lib" "$out/colour-luts" "$out/licenses" "$dist"
cp "$B/savagewind" "$out/"
llvm-strip --strip-debug "$out/savagewind"
printf '%s\n' $ids > "$out/gamelib.id"
cp -R "$L/coop-overlay-dist/coop-overlay" "$out/overlay-builder"
cp Co-op/play_coop.sh Co-op/setup_coop.sh Co-op/build_overlay.py Co-op/astree_tables.py Co-op/CHANGELOG.md \
   Co-op/VERSION docs/coop-linux.md "$out/"
chmod +x "$out/play_coop.sh" "$out/setup_coop.sh" "$out/savagewind"
cp docs/icon/icon_1024.png "$out/icon.png"
cp runtime/nvn/vulkan/vk_pipelines.seed "$out/"
cp third_party/fonts/FiraSans/FiraSans-SemiBold.ttf third_party/fonts/FiraSans/FiraSans-Medium.ttf \
   third_party/fonts/NotoSansThai/NotoSansThai-Medium.ttf third_party/fonts/NotoSansHebrew/NotoSansHebrew-Medium.ttf "$out/"
cp third_party/luts/*.cube third_party/luts/README.md "$out/colour-luts/"
cp LICENSE "$out/LICENSE-SavageWind.txt"
cp README.md "$out/licenses/README.md"  # (licences and source links of the bundled components)
cp third_party/fonts/FiraSans/OFL.txt "$out/licenses/FiraSans-OFL.txt"
cp third_party/fonts/NotoSansThai/OFL.txt "$out/licenses/NotoSansThai-OFL.txt"
cp third_party/fonts/NotoSansHebrew/OFL.txt "$out/licenses/NotoSansHebrew-OFL.txt"
cp third_party/FidelityFX/LICENSE.txt "$out/licenses/FidelityFX-LICENSE.txt"

# libraries: what the program needs (recursively) that the build above provides; anything else must be a system one
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
SYSTEM_LIBS='^(libc|libm|libdl|libpthread|librt|libresolv|libstdc\+\+|libgcc_s|libz|libvulkan)\.so|^ld-linux'
todo=("$out/savagewind")
while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    for n in $(needed "$f"); do
        [ -e "$out/lib/$n" ] && continue
        if [ -e "$P/lib/$n" ]; then
            cp -L "$P/lib/$n" "$out/lib/$n"
            patchelf --set-rpath '$ORIGIN' "$out/lib/$n"
            todo+=("$out/lib/$n")
        elif ! [[ $n =~ $SYSTEM_LIBS ]]; then
            echo "package: $(basename "$f") needs $n, neither bundled nor a base system library"; exit 1
        fi
    done
done
patchelf --set-rpath '$ORIGIN/lib' "$out/savagewind"
if ls "$out/lib" | grep -iE 'x264|x265|xvid|postproc|vulkan'; then echo "package: library that must not ship"; exit 1; fi
echo "bundled: $(ls "$out/lib" | tr '\n' ' ')"

tar -C "$stage" --owner=0 --group=0 -czf "$dist/$name" Co-op
(cd "$dist" && sha256sum "$name" > "$name.sha256")
echo "Done: $dist/$name (version $VERSION)"
ls -la "$dist/$name"
