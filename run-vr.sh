#!/bin/sh
# Mirror's Edge VR on macOS -> Quest (WiVRn). Start the WiVRn server and connect the headset first:
#   ../deps/wivrn-macos/build/wivrn/server/wivrn-server-headless
# Without a connected headset the game runs flat (the VR module falls back on its own).
# MEVR_FLAT=1 forces flat. Log: mevr-mac/build/MirrorsEdgeVR/mevr.log
here=$(cd "$(dirname "$0")" && pwd)
R=$here/RunnerRecompiled
M=$here/mevr-mac/build
[ -f "$M/mevr.ini" ] || cp "$M/mevr.ini.default" "$M/mevr.ini"
# Profile: the normal one, or a separate copy for quick-start and tests (MEVR_START / MEVR_PROFILE=quick),
# so automated "New Game" presses never touch the real save.
PROFILE=pc-user
if [ -n "$MEVR_START" ] && [ "$MEVR_START" != --list ] || [ "$MEVR_PROFILE" = quick ]; then
  PROFILE=pc-user-quick
  [ -d "$R/work/$PROFILE" ] || cp -R "$R/work/pc-user" "$R/work/$PROFILE"
fi
mkdir -p "$R/work/$PROFILE"
cd "$R" || exit 1
# OpenXR runtime: OXRSys (default; Mac-native, GPU H.265 path) or WiVRn (MEVR_RUNTIME=wivrn).
if [ "${MEVR_RUNTIME:-oxrsys}" = wivrn ]; then
  export XR_RUNTIME_JSON="${XR_RUNTIME_JSON:-${WIVRN_DIR:-$here/deps/wivrn-macos}/build/wivrn/openxr_wivrn-dev.json}"
else
  export XR_RUNTIME_JSON="${XR_RUNTIME_JSON:-${OXRSYS_DIR:-$here/deps/oxrsys}/build/macos-arm64/runtime/oxrsys-runtime.json}"
  for p in 9944 9945 9946 9948 9950; do adb reverse tcp:$p tcp:$p >/dev/null 2>&1; done
  # bring the OXRSys app on the Quest to the front (starts it if needed)
  adb shell monkey -p net.demonixis.oxrsys.android -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
fi
export MEVR_ENV_LOCALAPPDATA="$M"
# The mod finds TdEngine.ini under %USERPROFILE%\Documents (a link to the runtime's "My Documents").
export MEVR_ENV_USERPROFILE="$R/work/$PROFILE/other/C/Documents and Settings/Runner"
ln -sfn "My Documents" "$MEVR_ENV_USERPROFILE/Documents" 2>/dev/null
export MEVR_GAME_EXE="$R/work/pc/Binaries/MirrorsEdge.exe"
export ME_VR="$M/libmevr.dylib"
export ME_PC_DXVK="$R/work/dxvk/bin-vr/libdxvk_d3d9.0.dylib"
# Straight into VR: MEVR_START=<checkpoint> (list: MEVR_START=--list) skips the intro movie, starts a game in the
# quick-start profile (Return taps), then loads that checkpoint with F11 (frame MEVR_START_FRAME, default 4800).
if [ -n "$MEVR_START" ]; then
  INI="$MEVR_ENV_USERPROFILE/My Documents/EA Games/Mirror's Edge/TdGame/Config/TdInput.ini"
  python3 -I "$here/mevr-mac/start_checkpoint.py" "$INI" "$R/pc/tools/checkpoints.txt" "$MEVR_START" || exit 1
  [ "$MEVR_START" = --list ] && exit 0
  export ME_PC_NO_INTRO=1
  export ME_PC_AUTO_KEYS="${ME_PC_AUTO_KEYS:-32,36,40,44,48}" ME_PC_ASSUME_FOCUS=1
  f=${MEVR_START_FRAME:-4800}
  export ME_PC_AUTO_INPUT="${f}:down=F11;$((f + 10)):up=F11"
fi
# One game at a time: an old instance (often hung after a lost GPU device) ignores SIGTERM.
pkill -f "^work/pc-recomp/bin-macos/me_pc" 2>/dev/null && sleep 2; pkill -9 -f "^work/pc-recomp/bin-macos/me_pc" 2>/dev/null
exec work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps "work/$PROFILE" "$@"
