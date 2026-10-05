/* GCC 10+ "outline atomics" helpers (normally in libgcc), for prebuilt objects
 * compiled with -moutline-atomics (PPSSPP's bundled FFmpeg) linked by the GCC 7
 * toolchain, whose libgcc lacks them. Same contract as libgcc's lse.S: the
 * return value is the old contents of *ptr. Built with GCC 7's own atomics. */
#include <stdint.h>

__attribute__((visibility("hidden"))) uint64_t __aarch64_cas8_acq_rel(uint64_t expected, uint64_t desired, uint64_t* ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

__attribute__((visibility("hidden"))) uint32_t __aarch64_ldadd4_acq_rel(uint32_t value, uint32_t* ptr) {
    return __atomic_fetch_add(ptr, value, __ATOMIC_ACQ_REL);
}
