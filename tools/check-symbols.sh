#!/usr/bin/env bash
# Every undefined dynamic symbol of a core (or the app) must be defined by the
# cabinet's own libraries: libc, libm, libdl, libpthread, librt, libgcc_s,
# libstdc++, libz, libmali, libEGL, libGLESv2, libSDL2 and ld-linux, copied from
# the firmware (/lib, /usr/lib) into $SDK/cablibs-all/. Weak symbols are skipped.
# Usage (in the SDK image): bash check-symbols.sh <file.so|elf>... Catches what the SDK's
# version check can't: unversioned helpers like __aarch64_cas8_acq_rel.
L=/workspace/cablibs-all
for l in $L/*; do aarch64-linux-gnu-nm -D --defined-only "$l" 2>/dev/null; done | awk '{print $NF}' | sed 's/@.*//' | sort -u > /tmp/defined.txt
for so in "$@"; do
  miss=$(aarch64-linux-gnu-nm -D --undefined-only "$so" | awk '$1 != "w" {print $NF}' | sed 's/@.*//' | sort -u | comm -23 - /tmp/defined.txt | grep -v "^_ITM_\|^__gmon_start__\|^_Jv_RegisterClasses\|^__cxa_finalize")
  if [ -z "$miss" ]; then echo "OK   $(basename $so)"; else echo "MISS $(basename $so): $(echo $miss | tr '\n' ' ' | cut -c1-300)"; fi
done
