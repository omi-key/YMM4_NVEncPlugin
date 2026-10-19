#!/usr/bin/env bash
# Cross-build NvencNative.dll and the smoke harness from WSL/Linux with mingw-w64.
# Usage: build-wsl.sh <nvEncodeAPI.h include dir> <out dir> [NvencNative source dir]
# The header dir is e.g. nv-codec-headers' include/ffnvcodec (ffnvcodec's nvEncodeAPI.h).
set -euo pipefail
HDR=${1:?nvEncodeAPI.h include dir}
OUT=${2:?output dir}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${3:-$HERE/../../NvencNative}
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}   # posix threads: std::thread/mutex/condition_variable
mkdir -p "$OUT"
# mingw already defines NOMINMAX; the source redefines it -> harmless warning, silenced.
"$CXX" -std=c++17 -O2 -shared -Wno-builtin-macro-redefined \
  -I"$HDR" "$SRC/NvencNative.cpp" -o "$OUT/NvencNative.dll" \
  -static -ld3d11 -ldxgi -lmfplat -lmf -lmfuuid -lole32 -luuid 2>&1 | grep -v -E "NOMINMAX|os_defines|^\s+\||In file included|^\s+from" || true
"$CXX" -std=c++17 -O2 "$HERE/NvencSmoke.cpp" -o "$OUT/NvencSmoke.exe" -static -ld3d11 -ldxgi -luuid
ls -l "$OUT"
