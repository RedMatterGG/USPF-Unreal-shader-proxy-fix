#!/bin/sh
# Cross-builds d3d12.dll with MinGW-w64 (works on Linux and on Windows with MSYS2).
set -e
cd "$(dirname "$0")"
if [ ! -d third_party/DirectX-Headers ]; then  # Microsoft's official D3D12 headers (MIT)
  git clone -q https://github.com/microsoft/DirectX-Headers third_party/DirectX-Headers
  git -C third_party/DirectX-Headers checkout -q adbd6f3ba40795c46a8d0f33af00bcb57ff0f0a4
fi
CXX=${CXX:-x86_64-w64-mingw32-g++}
CC=${CC:-x86_64-w64-mingw32-gcc}
INC="-Ithird_party/DirectX-Headers/include -Ithird_party/DirectX-Headers/include/directx"
rm -rf build/obj && mkdir -p build/obj
FLAGS="-O2 -g0 -Wall -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -DUNICODE -D__REQUIRED_RPCNDR_H_VERSION__=475"
$CC  $FLAGS $INC -c src/vtbl_index.c -o build/obj/vtbl_index.o
$CC  -c src/stubs.S -o build/obj/stubs.o
for f in util desc_copy async_pso hooks proxy guids crashlog overlay phase present runlog tracker; do
  $CXX $FLAGS -std=c++20 $INC -c src/$f.cpp -o build/obj/$f.o
done
$CXX -shared -o build/d3d12.dll build/obj/*.o src/d3d12.def \
  -static -static-libgcc -static-libstdc++ -s -Wl,--enable-stdcall-fixup -lole32 -luuid -lpsapi -lgdi32 -luser32
echo "built build/d3d12.dll"
if [ "$1" = "test" ]; then
  $CXX $FLAGS -std=c++20 $INC tests/pso_test.cpp src/guids.cpp -Isrc -o build/pso_test.exe \
    -static -static-libgcc -static-libstdc++ -ld3d12 -ld3dcompiler_47 -ldxgi -lgdi32 -luser32
  echo "built build/pso_test.exe"
fi
