/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_MMIO_H
#define ARM_SMMU_4K_MMIO_H
#include "ArmSmmu4KProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {volatile unsigned char *mapping;size_t bytes;} ArmSmmu4KMmio;
/* AArch64 little-endian direct volatile MMIO adapter, with dsb sy barriers.
 * Caller supplies an owned live Device mapping. This allocates/maps nothing,
 * powers nothing and cannot recover a synchronous external bus abort. All
 * inputs/output must be disjoint and retained for the I/O object's lifetime.
 * Never bind arbitrary physical/virtual addresses or ordinary user buffers
 * as a hardware mapping. Software tests are explicitly separate fixtures. */
ArmSmmu4KStatus ArmSmmu4KBindMmio(ArmSmmu4KMmio *,ArmSmmu4KIo *);
#ifdef __cplusplus
}
#endif
#endif
