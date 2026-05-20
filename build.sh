#!/bin/bash
set -e

DXHEADERS="./DirectX-Headers"

if [ ! -d "$DXHEADERS" ]; then
  echo "Cloning DirectX-Headers..."
  git clone --depth=1 https://github.com/microsoft/DirectX-Headers "$DXHEADERS"
fi

# mingw-w64 (POSIX threading model) injects -lpthread via its GCC spec, which
# resolves to the DLL import library (libpthread.dll.a) and creates a runtime
# dependency on libwinpthread-1.dll.  CrossOver/Wine doesn't ship that DLL.
#
# Fix: put a private lib directory that contains ONLY the static archives
# (libpthread.a, libwinpthread.a — same file) first in the search path.
# The linker then picks the static archive for every -lpthread reference,
# whether it comes from our explicit flags or from the spec.
STATIC_PTHREAD_DIR="$(mktemp -d)"
trap 'rm -rf "$STATIC_PTHREAD_DIR"' EXIT
MINGW_LIB="/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64/x86_64-w64-mingw32/lib"
ln -s "$MINGW_LIB/libpthread.a"    "$STATIC_PTHREAD_DIR/libpthread.a"
ln -s "$MINGW_LIB/libwinpthread.a" "$STATIC_PTHREAD_DIR/libwinpthread.a"

echo "Compiling d3d12.dll..."
x86_64-w64-mingw32-g++ \
  -shared -O2 -std=c++17 \
  -fms-extensions \
  -I "$DXHEADERS/include/directx" \
  -I "$DXHEADERS/include" \
  -o d3d12.dll \
  d3d12_hook.cpp \
  d3d12.def \
  -static-libgcc \
  -static-libstdc++ \
  -L "$STATIC_PTHREAD_DIR" \
  -lkernel32 -lole32 -luuid \
  -Wl,--kill-at \
  -Wl,--enable-stdcall-fixup

echo "Done: d3d12.dll"
file d3d12.dll
