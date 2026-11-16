/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GMU_CONFIG_H
#define NATIVE_ADRENO_GMU_CONFIG_H
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoGmuMemory.h"
#include "ShengGmu.h"

/* Assemble the exact ChengGmuConfig the qualified GMU cold start consumes from
 * a validated firmware view and the allocated payload layout. It enforces the
 * qualified profile: firmware length must equal the payload's own image, the
 * five payload sizes must be the startup sizes, and every payload must be
 * allocated and IOVA-assigned. MMIO and time services stay caller-provided;
 * this function proves nothing about the hardware and performs no access. */
IOReturn NativeAdrenoAssembleGmuConfig(const NativeAdrenoFirmwareView *firmware,
    const NativeAdrenoGmuAllocator *allocator,uint32_t chip_id,unsigned poll_budget,
    ShengGmuConfig *config);
#endif
