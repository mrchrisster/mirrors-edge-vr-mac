#!/bin/sh
# Builds libmevr.dylib (arm64): the mod's own source (../vr-mod/src, branch macos, MEVR_MAC)
# plus the macOS glue (src/mevr_mac.cpp). Run from anywhere.
set -e
here=$(cd "$(dirname "$0")" && pwd)
XR=${OPENXR_DIR:-$here/../deps/openxr-loader}   # Khronos OpenXR loader install (include/ + lib/)
DX=$here/../RunnerRecompiled/work/dxvk/dxvk-3.1.1-git/include/native
python3 -I "$here/gen_inl.py" >/dev/null
mkdir -p "$here/build/obj"
CXX="clang++ -std=c++17 -O2 -g -fPIC -arch arm64 -fvisibility=hidden -DMEVR_MAC=1 -DNOMINMAX
  -I$here/compat -I$DX/windows -I$DX/directx -I$here/gen -I$XR/include -I/opt/homebrew/include
  -Wno-int-to-void-pointer-cast -Wno-ignored-attributes -Wno-macro-redefined -Wno-format"
$CXX -c "$here/../vr-mod/src/d3d9.cpp" -o "$here/build/obj/mod.o" &
$CXX -c "$here/src/mevr_mac.cpp" -o "$here/build/obj/glue.o" &
clang -std=c11 -D_M_IX86 -I"$here/compat/c" -O2 -fPIC -arch arm64 -c "$here/../vr-mod/third_party/minhook/src/hde/hde32.c" -o "$here/build/obj/hde32.o" &
wait
clang++ -arch arm64 -dynamiclib "$here/build/obj/mod.o" "$here/build/obj/glue.o" "$here/build/obj/hde32.o" -o "$here/build/libmevr.dylib" \
  -L"$XR/lib" -lopenxr_loader -Wl,-rpath,"$XR/lib" -install_name @rpath/libmevr.dylib
cp "$here/gen/mevr.ini" "$here/build/mevr.ini.default"
echo "built $here/build/libmevr.dylib"
