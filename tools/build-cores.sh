#!/usr/bin/env bash
# Cross-compile libretro cores for the cabinet (aarch64, glibc <= 2.26) with the
# AtGames SDK toolchain image. Expects the core sources cloned into
# <workspace>/cores-src (see README.md, "Building the cores"), and runs inside
# the SDK's Docker image with the SDK mounted at /workspace:
#   docker run --rm --platform linux/amd64 -v "$SDK:/workspace" \
#     atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/cores-src/build-cores.sh [name...]
# With names (e.g. fbneo mame2003_plus), only those cores are built.
# Notes: snes9x needs -std=gnu++11 (GCC 5 defaults to C++98); gpsp needs
# CPU_ARCH=arm64 and tools/patches/gpsp-arm64-old-gas.patch (the SDK's older
# assembler does not know the lr/fp register aliases).
set -u
cd /workspace/cores-src
OUT=/workspace/cores-src/out
mkdir -p "$OUT"
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar
X="platform=unix CC=$CC CXX=$CXX AR=$AR"
J=-j$(nproc)
ONLY=" $* "
build() { # name dir makeargs... ; resulting .so path is found afterwards
  local name=$1 dir=$2; shift 2
  [ "$ONLY" != "  " ] && [[ "$ONLY" != *" $name "* ]] && return
  echo "=== $name"
  ( cd "$dir" && make clean >/dev/null 2>&1; make $J $X "$@" ) > "$OUT/$name.log" 2>&1
  local so; so=$(find "$dir" -maxdepth 1 -name "*_libretro.so" -newer build-cores.sh | head -1)
  if [ -n "$so" ]; then
    cp "$so" "$OUT/"; aarch64-linux-gnu-strip "$OUT/$(basename "$so")"
    /workspace/sdk/check-compat.sh "$OUT/$(basename "$so")" > "$OUT/$name.compat" 2>&1 && echo "OK $(basename "$so")" || { echo "COMPAT FAIL $name"; tail -3 "$OUT/$name.compat"; }
  else
    echo "BUILD FAIL $name"; grep -m3 -E "error|Error" "$OUT/$name.log"
  fi
}
build snes9x       snes9x/libretro CXX="aarch64-linux-gnu-g++ -std=gnu++11"
build fceumm       libretro-fceumm -f Makefile.libretro
build gearcoleco   Gearcoleco/platforms/libretro
build gambatte     gambatte-libretro -f Makefile.libretro
build gpsp         gpsp CPU_ARCH=arm64 HAVE_DYNAREC=1 MMAP_JIT_CACHE=1
build pce_fast     beetle-pce-fast-libretro
build handy        libretro-handy
# Arcade. Each ships with the ROM database of the same commit (tools/make_arcade_db.py).
build fbneo        FBNeo/src/burner/libretro CXX="aarch64-linux-gnu-g++ -std=gnu++11"
build mame2003_plus mame2003-plus-libretro
ls -la "$OUT"/*.so 2>/dev/null
