#!/usr/bin/env bash
# Flycast (Dreamcast / NAOMI / Atomiswave) for the cabinet. Runs in the SDK image,
# with the SDK mounted at /workspace:
#   docker run --rm --platform linux/amd64 -v "$SDK:/workspace" \
#     atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/cores-src/build-flycast.sh
#
# The SDK's GCC 5.4 is too old for Flycast (C++17), so this uses Bootlin's GCC 7.2
# toolchain, which targets glibc 2.26 (the cabinet's) and whose libstdc++ is the
# cabinet's newest. One-time setup in $SDK/toolchains:
#   aarch64--glibc--bleeding-edge/  https://toolchains.bootlin.com/downloads/releases/
#                                   toolchains/aarch64/tarballs/aarch64--glibc--bleeding-edge-2017.11-1.tar.bz2
#   cmake/                          CMake >= 3.22 (e.g. cmake-3.27.9-linux-x86_64)
#   compat/filesystem               tools/compat/filesystem (C++17 <filesystem> on GCC 7)
#   compat/GLES2, GLES3, KHR, EGL   Khronos headers (OpenGL-Registry / EGL-Registry api/)
# and tools/patches/flycast-gcc7.patch applied to cores-src/flycast.
set -u
T=/workspace/toolchains/aarch64--glibc--bleeding-edge
export PATH=$T/bin:/workspace/toolchains/cmake/bin:$PATH
cd /workspace/cores-src/flycast
mkdir -p build-cab && cd build-cab
cmake .. -DLIBRETRO=ON -DUSE_GLES=ON -DUSE_VULKAN=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-g++ \
  -DCMAKE_C_FLAGS="-isystem /workspace/toolchains/compat" -DCMAKE_CXX_FLAGS="-isystem /workspace/toolchains/compat" -DCMAKE_CXX_STANDARD_LIBRARIES="-lstdc++fs" \
  -DCMAKE_FIND_ROOT_PATH=$T/aarch64-buildroot-linux-gnu/sysroot > ../../out/flycast.cmake.log 2>&1 || { echo "CMAKE FAIL"; tail -20 ../../out/flycast.cmake.log; exit 1; }
make -k -j$(nproc) > ../../out/flycast.log 2>&1 || { echo "BUILD FAIL"; grep -m8 -E "error" ../../out/flycast.log; exit 1; }
so=$(find . -maxdepth 2 -name "flycast_libretro.so" | head -1)
cp "$so" ../../out/ && $T/bin/aarch64-linux-strip ../../out/flycast_libretro.so
/workspace/sdk/check-compat.sh ../../out/flycast_libretro.so | tail -4
