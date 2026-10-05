#!/usr/bin/env bash
# N64 (Mupen64Plus-Next) and Saturn (YabaSanshiro) for the cabinet: GLES 3 builds with the
# Bootlin GCC 7.2 / glibc 2.26 toolchain, linked against the cabinet's own libGLESv2/libEGL
# (toolchains/cablibs). Same setup as build-flycast.sh / build-ppsspp.sh.
set -u
T=/workspace/toolchains/aarch64--glibc--bleeding-edge
export PATH=$T/bin:/workspace/toolchains/python/bin:$PATH
export CC=aarch64-linux-gcc CXX=aarch64-linux-g++ AR=aarch64-linux-ar
export CFLAGS="-isystem /workspace/toolchains/compat" CXXFLAGS="-isystem /workspace/toolchains/compat"
export LDFLAGS="-L/workspace/toolchains/cablibs -Wl,-rpath-link,/workspace/toolchains/cablibs -Wl,--no-as-needed -lmali -lEGL"
OUT=/workspace/cores-src/out
J=-j$(nproc)
one() { # name dir so makeargs...
  local name=$1 dir=$2 so=$3; shift 3
  echo "=== $name"
  ( cd "$dir" && { [ -n "${NOCLEAN:-}" ] || make clean >/dev/null 2>&1; }; make -k $J CC=$CC CXX=$CXX AR=$AR "$@" ) > "$OUT/$name.log" 2>&1
  if [ -f "$dir/$so" ]; then
    cp "$dir/$so" "$OUT/" && $T/bin/aarch64-linux-strip "$OUT/$so"
    /workspace/sdk/check-compat.sh "$OUT/$so" | tail -1
  else
    echo "BUILD FAIL $name"; grep -E "error" "$OUT/$name.log" | sort | uniq -c | sort -rn | head -12
  fi
}
cd /workspace/cores-src
[ "${1:-all}" = all ] || [ "$1" = mupen ] && one mupen64plus_next mupen64plus-libretro-nx mupen64plus_next_libretro.so platform=arm64_cortex_a53_gles3
[ "${1:-all}" = all ] || [ "$1" = yaba ] && one yabasanshiro yabasanshiro/yabause/src/libretro yabasanshiro_libretro.so platform=arm64_cortex_a53_gles3
