/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GMU_PLATFORM_H
#define NATIVE_ADRENO_GMU_PLATFORM_H
#include <IOKit/IOTypes.h>
#include "NativeAdrenoSha256.h"
#include "ShengGmu.h"

/* Bounds-checked MMIO adapter and time/hash services for the qualified GMU
 * cold start. Windows are exactly the five the startup uses, already mapped
 * and powered by the caller: GMU core, RSCC, PDC, the GPU register window and
 * CX misc. Every access is 4-byte aligned and inside its window; a violation
 * returns failure instead of touching another register. The adapter performs
 * no mapping, clock, reset or power action, and the services object performs
 * no I/O of its own beyond the caller-provided clock callbacks. */
struct NativeAdrenoGmuWindow {
    volatile uint8_t *base;
    size_t bytes;
};
struct NativeAdrenoGmuTime {
    void *cookie;
    int (*now_us)(void *,uint64_t *);
    int (*wait_us)(void *,unsigned);
};
class NativeAdrenoGmuPlatform final {
    NativeAdrenoGmuWindow windows_[ShengGmuWindows]{};
    static int read32(void *,unsigned,size_t,uint32_t *);
    static int write32(void *,unsigned,size_t,uint32_t);
    static int barrier(void *);
public:
    IOReturn bind(const NativeAdrenoGmuWindow windows[ShengGmuWindows],ShengGmuIo *io);
    /* sha256 uses the kernel implementation; now/wait must be real. */
    static IOReturn services(const NativeAdrenoGmuTime *,ShengGmuServices *);
};
#endif
