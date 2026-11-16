/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_KERNEL_TIME_H
#define NATIVE_ADRENO_KERNEL_TIME_H
#include <stdint.h>

/* Real kernel time for the GMU startup: monotonic microseconds from
 * mach_absolute_time and a bounded busy wait. This is the only production
 * implementation of the time callbacks; tests inject their own. */
struct NativeAdrenoKernelTime {
    static int now_us(void *,uint64_t *);
    static int wait_us(void *,unsigned);
};
#endif
