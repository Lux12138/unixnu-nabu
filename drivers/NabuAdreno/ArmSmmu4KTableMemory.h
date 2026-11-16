/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_TABLE_MEMORY_H
#define ARM_SMMU_4K_TABLE_MEMORY_H
#include "ArmSmmu4KAudit.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const volatile uint64_t *mapping;
    uint64_t physical;
    size_t pages;
} ArmSmmu4KTableMapping;
typedef struct {
    unsigned count;
    ArmSmmu4KTableMapping pools[2];
} ArmSmmu4KTableMemory;
/* Concrete aligned volatile 64-bit descriptor reads through already existing
 * CPU mappings of one/two physically contiguous owned table pools. Validates
 * CPU/physical bounds and aliasing; it cannot create or establish the claimed
 * VA->PA relation, ownership, cache attributes or visibility. Platform must
 * obtain and retain those mappings from its real allocator, clean tables for
 * device visibility and order publication before activation/audit. Caller
 * retains immutable memory metadata and tables under an exclusive lease.
 * No fallback tables, copying/rebuilding, MMIO, cache clean, allocation or
 * hardware probe. A matching software buffer alone is not hardware evidence.
 * On bind failure io is cleared. Inputs/output/storage must be disjoint. */
ArmSmmu4KStatus ArmSmmu4KBindTableMemory(const ArmSmmu4KTableMemory *,ArmSmmu4KTableIo *);
#ifdef __cplusplus
}
#endif
#endif
