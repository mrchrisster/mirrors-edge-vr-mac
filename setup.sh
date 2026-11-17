#!/bin/bash
# One-time setup: everything from your GOG installer to a game that runs in the Quest.
#
#   ./setup.sh ~/Downloads/'setup_mirrors_edge_1.01_jpfix_(41238).exe'
#
# Needs: an Apple Silicon Mac (macOS 14+), Xcode Command Line Tools, Homebrew, ~20 GB free, and the GOG
# installer with its -1/-2/-3.bin parts next to it. Downloads and builds, all inside this folder:
#   RunnerRecompiled (pinned, + patches/RunnerRecompiled-mevr.patch) -> the game lifted to arm64
#   DXVK 3.1.1 (RunnerRecompiled's macOS build, + mevr-mac/dxvk_patch.py)
#   mirrors-edge-vr-mod (pinned) -> compiled for macOS as mevr-mac/build/libmevr.dylib
#   Khronos OpenXR loader, OXRSys (pinned, + patches/OXRSys-mevr.patch) -> the macOS OpenXR runtime
#   the OXRSys Quest app (prebuilt from this repo's release, or built if the Android SDK is set up)
# Then installs the Quest app when a headset is connected with USB debugging allowed. Safe to re-run: finished
# steps are skipped. Nothing built from your game files ever leaves your Mac.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
INSTALLER=${1:-}
RR_URL=https://github.com/prasys/RunnerRecompiled.git
RR_BASE=$(cat patches/RunnerRecompiled.base)
MOD_URL=https://github.com/letsgosportsteam/mirrors-edge-vr-mod.git
MOD_BASE=$(cat patches/vr-mod.base)
OXR_URL=https://github.com/demonixis/OXRSys.git
OXR_BASE=$(cat patches/OXRSys.base)
OPENXR_TAG=release-1.1.54
APK_URL=https://github.com/mrchrisster/mirrors-edge-vr-mac/releases/latest/download/oxrsys-quest-mevr.apk
LLVM=/opt/homebrew/opt/llvm@23

step() { printf '\n==> %s\n' "$*"; }
die() { echo "setup: $*" >&2; exit 1; }

[ "$(uname -m)" = arm64 ] || die "needs an Apple Silicon Mac"
command -v brew >/dev/null || die "needs Homebrew (https://brew.sh)"
xcode-select -p >/dev/null 2>&1 || die "needs the Xcode Command Line Tools: xcode-select --install"

step "Homebrew packages"
pkgs=(llvm@23 sdl3 molten-vk vulkan-loader vulkan-headers glslang meson ninja pkgconf cmake innoextract cabextract msitools)
missing=(); for p in "${pkgs[@]}"; do brew list --versions "$p" >/dev/null 2>&1 || missing+=("$p"); done
[ ${#missing[@]} -eq 0 ] || brew install "${missing[@]}"
command -v adb >/dev/null || brew install --cask android-platform-tools

clone() { # clone <url> <dir> <commit>
    if [ ! -d "$2/.git" ]; then git clone -q "$1" "$2"; fi
    git -C "$2" fetch -q origin "$3" 2>/dev/null || true
    if [ "$(git -C "$2" rev-parse HEAD)" != "$3" ] && [ -z "$(git -C "$2" status --porcelain)" ]; then
        git -C "$2" checkout -q "$3"
    fi
}
apply_patch() { # apply_patch <dir> <patch>: once
    if git -C "$1" apply --reverse --check "$2" >/dev/null 2>&1; then return; fi
    git -C "$1" apply "$2"
}

# ---------------------------------------------------------------- RunnerRecompiled + the game
step "RunnerRecompiled"
clone "$RR_URL" RunnerRecompiled "$RR_BASE"
git -C RunnerRecompiled checkout -q "$RR_BASE" 2>/dev/null || true
apply_patch RunnerRecompiled "$here/patches/RunnerRecompiled-mevr.patch"
R=$here/RunnerRecompiled

if [ ! -f "$R/work/pc/Binaries/MirrorsEdge.exe" ]; then
    [ -n "$INSTALLER" ] || die "first run: pass the GOG installer, e.g. ./setup.sh ~/Downloads/'setup_mirrors_edge_1.01_jpfix_(41238).exe'"
    [ -f "$INSTALLER" ] || die "installer not found: $INSTALLER"
    step "Extracting the game (about 11 GB)"
    mkdir -p "$R/work/pc"
    innoextract -q -d "$R/work/pc" "$INSTALLER"
fi
sha=$(shasum -a 256 "$R/work/pc/Binaries/MirrorsEdge.exe" | cut -d' ' -f1)
[ "$sha" = 58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd ] || die "MirrorsEdge.exe is not the GOG 1.01 build (sha $sha)"
cp -n "$R/work/pc/Binaries/PhysXCore.bak" "$R/work/pc/Binaries/PhysXCore.dll" 2>/dev/null || true

cd "$R"
if [ ! -f work/pc-deps/msvcr80.dll ]; then
    step "Visual C++ 2005 runtime (from the GOG installer)"
    mkdir -p work/x/vcredist work/pc-deps
    cabextract -q -d work/x/vcredist work/pc/__redist/MSVC2005/vcredist_x86.exe
    cabextract -q -d work/x/vcredist/u work/x/vcredist/VCREDI~3.EXE
    msiextract -C work/x/vcredist/u/msi work/x/vcredist/u/vcredist.msi >/dev/null
    cp work/x/vcredist/u/msi/Windows/system32/msvcr80.dll work/x/vcredist/u/msi/Windows/system32/msvcp80.dll work/pc-deps/
    rm -rf work/x
fi
if [ ! -f work/pc-deps/rebased/binkw32.dll ]; then
    step "Rebasing the game's DLLs"
    mkdir -p work/pc-deps/rebased
    for m in PhysXLoader:20000000 PhysXCore:21000000 NxCooking:23000000 PhysXExtensions:24000000 \
             NxCharacter:25000000 ogg:26000000 vorbis:27000000 vorbisfile:28000000 vorbisenc:29000000 \
             libresample:2a000000 binkw32:2b000000; do
        python3 -I pc/tools/rebase_pe.py "work/pc/Binaries/${m%%:*}.dll" "work/pc-deps/rebased/${m%%:*}.dll" "${m##*:}"
    done
fi
W=work/pc-recomp; L=$LLVM/lib
if [ ! -f $W/summary.json ]; then
    step "Function catalogs (a few minutes)"
    python3 -I pc/tools/catalog.py work/pc/Binaries/MirrorsEdge.exe --llvm $L --out $W
    python3 -I pc/tools/catalog.py work/pc-deps/msvcr80.dll --llvm $L --out $W/msvcr80
    python3 -I pc/tools/catalog.py work/pc-deps/msvcp80.dll --llvm $L --out $W/msvcp80
    for m in PhysXLoader PhysXCore NxCooking PhysXExtensions NxCharacter ogg vorbis vorbisfile vorbisenc libresample binkw32; do
        python3 -I pc/tools/catalog.py work/pc-deps/rebased/$m.dll --llvm $L --out $W/$(echo $m | tr A-Z a-z)
    done
fi

step "DXVK for macOS + VR hooks"
[ -d work/dxvk/dxvk-3.1.1-git ] || pc/dxvk/build-macos.sh || true   # the first build may stop on a header newer LLVM rejects
python3 -I "$here/mevr-mac/dxvk_patch.py" work/dxvk/dxvk-3.1.1-git
ninja -C work/dxvk/dxvk-3.1.1-git/build-macos >/dev/null
mkdir -p work/dxvk/bin work/dxvk/bin-vr
cp work/dxvk/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib work/dxvk/bin/
cp work/dxvk/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib work/dxvk/bin-vr/

step "Lifting and compiling the game (6-10 minutes the first time)"
python3 -I pc/tools/gen_d3d9_bridge.py work/dxvk/dxvk-3.1.1-git/include/native/directx/d3d9.h pc/runtime/rt_d3d9_gen.c >/dev/null
python3 -I pc/tools/build_pc_runtime.py --macos "$LLVM" --work work/pc-recomp | tail -1
cd "$here"

# ---------------------------------------------------------------- the VR mod
step "VR mod (macOS port)"
clone "$MOD_URL" vr-mod "$MOD_BASE"
git -C vr-mod checkout -q "$MOD_BASE" -- src 2>/dev/null || true
grep -q MEVR_MAC vr-mod/src/d3d9.cpp || python3 -I mevr-mac/port_patch.py

# ---------------------------------------------------------------- OpenXR loader + OXRSys
mkdir -p deps
if [ ! -f deps/openxr-loader/lib/libopenxr_loader.dylib ]; then
    step "Khronos OpenXR loader ($OPENXR_TAG)"
    [ -d deps/OpenXR-SDK ] || git clone -q --depth 1 --branch $OPENXR_TAG https://github.com/KhronosGroup/OpenXR-SDK.git deps/OpenXR-SDK
    cmake -S deps/OpenXR-SDK -B deps/openxr-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$here/deps/openxr-loader" -DBUILD_SHARED_LIBS=ON -DBUILD_LOADER=ON \
        -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_TESTS=OFF -DBUILD_TESTING=OFF >/dev/null
    cmake --build deps/openxr-build --target install >/dev/null
fi

step "OXRSys (macOS OpenXR runtime)"
clone "$OXR_URL" deps/oxrsys "$OXR_BASE"
apply_patch deps/oxrsys "$here/patches/OXRSys-mevr.patch"
(cd deps/oxrsys && cmake --preset macos-arm64 >/dev/null && cmake --build build/macos-arm64 --target oxrsys-runtime >/dev/null 2>&1 || cmake --build build/macos-arm64 >/dev/null)
CFG="$HOME/Library/Application Support/OXRSys"; mkdir -p "$CFG"
if [ ! -f "$CFG/oxrsys-runtime.toml" ]; then
    cp deps/oxrsys/build/macos-arm64/runtime/oxrsys-runtime.toml "$CFG/"
    sed -i '' -e 's/^bitrate_mbps = .*/bitrate_mbps = 150/' -e 's/^resolution_scale = .*/resolution_scale = 1.0/' \
              -e 's/^render_device = .*/render_device = "quest3_native"/' -e 's/^transport = .*/transport = "usb_adb"/' \
              -e 's/^abr_mode = .*/abr_mode = "off"/' -e 's/^foveated_encoding_preset = .*/foveated_encoding_preset = "off"/' \
              "$CFG/oxrsys-runtime.toml"
    echo "OXRSys settings written to $CFG/oxrsys-runtime.toml (150 Mbit/s: use a USB 3 cable, or lower bitrate_mbps to 80)"
fi

step "OXRSys Quest app"
APK=deps/oxrsys-quest-mevr.apk
if [ ! -f "$APK" ]; then
    # The prebuilt app from this repo's release; MEVR_BUILD_APK=1 builds it instead (needs the Android SDK/NDK).
    if [ "${MEVR_BUILD_APK:-0}" = 1 ]; then
        (cd deps/oxrsys/clients/android-vr && ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk} \
            JAVA_HOME=${JAVA_HOME:-"/Applications/Android Studio.app/Contents/jbr/Contents/Home"} ./gradlew -q assembleRelease) &&
            cp deps/oxrsys/clients/android-vr/app/build/outputs/apk/release/app-release.apk "$APK" || true
    fi
    [ -f "$APK" ] || curl -fsSL -o "$APK" "$APK_URL" || echo "could not get the Quest app; download it from this repo's Releases page"
fi
if [ -f "$APK" ] && adb devices | grep -q 'device$'; then
    adb install -r "$APK" >/dev/null && echo "Quest app installed"
else
    echo "Connect the Quest by USB (allow USB debugging) and run: adb install -r $APK"
fi

# ---------------------------------------------------------------- the VR module
step "VR module"
mevr-mac/build.sh | tail -1
[ -f mevr-mac/build/mevr.ini ] || cp mevr-mac/build/mevr.ini.default mevr-mac/build/mevr.ini
sed -i '' 's/^Resolution = .*/Resolution = 3456x1944/' mevr-mac/build/mevr.ini

step "Done"
cat <<EOF
Put on the Quest (USB connected), then:
  ./run-vr.sh                                   start the game in VR
  MEVR_START=Before_Balance_Walk ./run-vr.sh    jump straight into a checkpoint (MEVR_START=--list lists them)
The first start of each level compiles shaders and can freeze for up to a minute.
EOF
