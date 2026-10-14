#!/bin/sh
# Cross-builds USPF.exe with MinGW-w64. Needs ../d3d12async built first (its d3d12.dll and
# d3d12async.ini are embedded and installed into games by the app).
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build/obj payload
cp ../d3d12async/build/d3d12.dll ../d3d12async/d3d12async.ini payload/
FLAGS="-O2 -g0 -Wall -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -std=c++20 -Ithird_party/webview2 -Isrc"
for f in util games runs ops ini net icon streaming engineini main; do
  $CXX $FLAGS -c src/$f.cpp -o build/obj/$f.o &
done
wait
$WINDRES -I res res/uspf.rc -O coff -o build/obj/res.o
$CXX -municode -mwindows -o build/USPF.exe build/obj/*.o -static -static-libgcc -static-libstdc++ -s \
  -lole32 -luuid -lshell32 -lshlwapi -lcomdlg32 -ldwmapi -lwinhttp -lwindowscodecs -lversion -luser32 -lgdi32 -ladvapi32 -ldxgi
echo "built build/USPF.exe"
if [ "$1" = "test" ]; then
  $CXX $FLAGS tests/backend_test.cpp build/obj/util.o build/obj/games.o build/obj/runs.o build/obj/ops.o build/obj/streaming.o build/obj/engineini.o build/obj/ini.o \
    build/obj/net.o build/obj/icon.o build/obj/res.o -o build/backend_test.exe -static -static-libgcc -static-libstdc++ \
    -lole32 -luuid -lshell32 -lshlwapi -lwinhttp -lwindowscodecs -lversion -luser32 -lgdi32 -ladvapi32 -ldxgi
  echo "built build/backend_test.exe"
fi
