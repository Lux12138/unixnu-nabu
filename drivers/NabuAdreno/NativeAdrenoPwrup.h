/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PWRUP_H
#define NATIVE_ADRENO_PWRUP_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Pack the A7xx power-up register list consumed by CP_ME_INIT. The layout is
 * the pinned `struct cpu_gpu_lock`: three u32 lock words, a union of either
 * {u8 ifpc_len, u8 preemption_len, u16 dynamic_len} (A7xx) or two u16 fields
 * (A6xx), then 64-bit register slots holding {reg, current value} pairs and
 * 12-byte dynamic triplets {aperture, offset, value}. Only the A7xx form is
 * packed here. Values must come from actual register reads at bring-up; this
 * function never invents one, never reads MMIO and never clears the lock words
 * silently. Capacity and count limits are checked before any write. */
struct NativeAdrenoPwrupCounts {
    uint32_t ifpc;
    uint32_t preemption;
    uint32_t dynamic;
};
class NativeAdrenoPwrupList final {
    NativeAdrenoPwrupList()=delete;
public:
    static size_t requiredBytes(const NativeAdrenoPwrupCounts &);
    /* regs/values are parallel arrays; dynamic triplets are {aperture,offset,
     * value} in that order. Output is the exact byte image; *written receives
     * the length on success. */
    /* Read the sixteen A740 preemption registers through the callback (raw
     * 32-bit GPU register reads) and pack them with zero IFPC and dynamic
     * counts. A read failure refuses and writes nothing. */
    static IOReturn buildA740(int (*read)(void *,uint32_t,uint32_t *),void *cookie,
        uint8_t *out,size_t capacity,size_t *written);
    static IOReturn pack(const NativeAdrenoPwrupCounts &,
        const uint32_t *ifpc_regs,const uint32_t *ifpc_values,
        const uint32_t *preemption_regs,const uint32_t *preemption_values,
        const uint32_t *dynamic_triplets,uint8_t *out,size_t capacity,size_t *written);
};
#endif
