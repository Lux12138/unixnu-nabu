/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_H
#define ARM_SMMU_4K_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ArmSmmu4KSuccess = 0, ArmSmmu4KArgument, ArmSmmu4KBadRange,
    ArmSmmu4KOverlap, ArmSmmu4KCapacity, ArmSmmu4KBudget,
    ArmSmmu4KUnsupported, ArmSmmu4KStateMismatch,
    ArmSmmu4KIoError, ArmSmmu4KTimeout, ArmSmmu4KBusy, ArmSmmu4KFault
} ArmSmmu4KStatus;
typedef struct {
    unsigned input_bits;           /* 32..48, low-address TTBR0 profile. */
    unsigned output_bits;          /* 32,36,40,42,44,48. */
    uint64_t table_physical;        /* Caller-owned contiguous pool, aligned 4K. */
    size_t leaf_budget;            /* Explicit upper bound on mapping work. */
} ArmSmmu4KConfig;
enum {ArmSmmu4KRead=1,ArmSmmu4KWrite=2,ArmSmmu4KExecute=4,ArmSmmu4KPrivileged=8};
typedef struct {
    uint64_t iova, physical, bytes;
    unsigned permissions;          /* Read required; optional Write/Execute/Privileged. */
    unsigned writeback;            /* 0=normal NC, 1=normal WB/WA. */
    unsigned shareability;         /* 2=outer, 3=inner; explicit, no default. */
} ArmSmmu4KRange;
typedef struct {
    uint64_t root_physical;
    uint64_t mair;                 /* NC at index 0, WB/WA at index 1. */
    size_t table_pages, leaf_pages;
    unsigned start_level;
    unsigned upper_region;        /* 0=Build TTBR0, 1=BuildUpper TTBR1. */
} ArmSmmu4KResult;

/* Build inactive ARM LPAE stage-1 tables with ONLY 4K level-3 leaves.
 * Sorted disjoint low IOVA ranges; no sign extension or input conversion.
 * No resource may map the table pool. Conflicting cache/shareability/privilege
 * aliases refuse. Default 1/3 retains unprivileged PXN/UXN behavior. Execute
 * and Privileged explicitly describe DEVICE-stage1 accesses (e.g. firmware),
 * never CPU permissions or authorization to execute unauthenticated images.
 * All validation/capacity checks precede table writes. Failure zeros result
 * and leaves tables untouched. Success writes only result.table_pages pages.
 * Caller guarantees real accessible, disjoint storage, immutable inputs,
 * native-endian uint64_t table storage aligned to at least 8 bytes, actual
 * physical ownership and matching CPU attributes/cache maintenance.
 * NO MMIO, allocation, barriers, TLB operations or activation. This does NOT
 * prove hardware support, coherent walks or a live isolated GPU context. */
ArmSmmu4KStatus ArmSmmu4KBuild(const ArmSmmu4KConfig *config,
    const ArmSmmu4KRange *ranges, size_t range_count, uint64_t *tables,
    size_t table_capacity_pages, ArmSmmu4KResult *result);
/* Same native 4K format for TTBR1's upper, sign-extended address region.
 * IOVAs must already be in [2^64-2^input_bits,2^64); truncated low addresses
 * refuse. Uses the architectural partial-root index width. No lower-region
 * remapping, page-size conversion, MMIO or translation activation occurs. */
ArmSmmu4KStatus ArmSmmu4KBuildUpper(const ArmSmmu4KConfig *config,
    const ArmSmmu4KRange *ranges, size_t range_count, uint64_t *tables,
    size_t table_capacity_pages, ArmSmmu4KResult *result);
#ifdef __cplusplus
}
#endif
#endif
