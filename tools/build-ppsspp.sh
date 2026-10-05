#!/usr/bin/env bash
# PPSSPP (PSP) libretro core for the cabinet, with the Bootlin GCC 7.2 / glibc 2.26 toolchain.
set -u
T=/workspace/toolchains/aarch64--glibc--bleeding-edge
export PATH=$T/bin:/workspace/toolchains/cmake/bin:/workspace/toolchains/python/bin:$PATH
cd /workspace/cores-src/ppsspp
mkdir -p build-cab && cd build-cab
cmake .. -DPYTHON_EXECUTABLE=/workspace/toolchains/python/bin/python3 -DLIBRETRO=ON -DUSING_GLES2=ON -DUSING_EGL=OFF -DUSE_FFMPEG=ON -DUSE_SYSTEM_FFMPEG=OFF -DUSE_DISCORD=OFF \
  -DUSE_MINIUPNPC=OFF -DUSING_X11_VULKAN=OFF -DVULKAN=OFF -DHEADLESS=OFF -DUNITTEST=OFF -DSIMULATOR=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-g++ \
  -DCMAKE_C_FLAGS="-isystem /workspace/toolchains/compat" -DCMAKE_CXX_FLAGS="-isystem /workspace/toolchains/compat" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lstdc++fs" \
  -DCMAKE_SHARED_LINKER_FLAGS="-L/workspace/toolchains/cablibs -Wl,-rpath-link,/workspace/toolchains/cablibs /workspace/toolchains/compat/aarch64_atomics.o" \
  -DCMAKE_FIND_ROOT_PATH=$T/aarch64-buildroot-linux-gnu/sysroot > ../../out/ppsspp.cmake.log 2>&1 || { echo "CMAKE FAIL"; tail -25 ../../out/ppsspp.cmake.log; exit 1; }
make -k -j$(nproc) ppsspp_libretro > ../../out/ppsspp.log 2>&1 || { echo "BUILD FAIL"; grep -E "error:" ../../out/ppsspp.log | sort | uniq -c | sort -rn | head -25; exit 1; }
so=$(find . -name "ppsspp_libretro.so" | head -1)
cp "$so" ../../out/ && $T/bin/aarch64-linux-strip ../../out/ppsspp_libretro.so
/workspace/sdk/check-compat.sh ../../out/ppsspp_libretro.so | tail -4
