/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_AUDIT_H
#define ARM_SMMU_4K_AUDIT_H
#include "ArmSmmu4KProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *cookie;
    /* Read the actual descriptor at an owned physical address. No generated,
     * cached reference-table or translated/shadow-table substitute. Return 0
     * on success. Platform supplies a real coherent CPU mapping and ownership. */
    int (*read_descriptor)(void *,uint64_t physical,uint64_t *);
} ArmSmmu4KTableIo;
typedef struct {
    const ArmSmmu4KRange *ranges;
    size_t range_count,capacity_pages,read_budget;
    /* Scratch: one byte per pool page, reset by each audit. Disjoint from all
     * inputs, outputs, mapped resources, physical tables and other scratch. */
    unsigned char *visited;
    size_t visited_bytes;
} ArmSmmu4KAuditRegion;
typedef struct {
    ArmSmmu4KTableIo io;
    const ArmSmmu4KAuditRegion *lower,*upper; /* exactly the enabled regions */
} ArmSmmu4KAuditConfig;
typedef struct {
    size_t descriptor_reads,table_pages,leaf_pages;
    uint64_t descriptor_physical,descriptor,iova,expected_descriptor;
    unsigned level,complete;
} ArmSmmu4KTableAuditResult;
typedef struct {
    ArmSmmu4KTableAuditResult lower,upper;
    ArmSmmu4KProgramResult before,after;
    unsigned phase,complete;
} ArmSmmu4KAuditResult;
/* Walk the physical graph, including every unused slot, with a strict read
 * budget and bounded depth (four levels). Reject blocks, cycles, aliases of
 * table pages, extra/missing mappings, permissions/attributes differing from
 * declared resources, foreign PAs and nonzero reserved descriptor bits.
 * No allocation, descriptor writes, rebuilding, conversion or MMIO. Each
 * expected resource must appear exactly once, in ascending native IOVA order.
 * Caller holds immutable tables/ranges/results and exclusive leases throughout;
 * all masters are quiesced. Scratch and physical callback are trusted kernel
 * boundaries. Declared bounds do NOT establish physical ownership/provenance. */
ArmSmmu4KStatus ArmSmmu4KAuditTables(const ArmSmmu4KTableIo *,
    const ArmSmmu4KConfig *,const ArmSmmu4KResult *,const ArmSmmu4KAuditRegion *,
    uint64_t root_physical,ArmSmmu4KTableAuditResult *);
/* Pre-activation audit of BOTH declared physical regions; also rejects pool
 * overlap and resource access to either entire pool, including spare pages.
 * This is not an observation of an active context. */
ArmSmmu4KStatus ArmSmmu4KAuditDeclaredContext(const ArmSmmu4KContextConfig *,
    const ArmSmmu4KAuditConfig *,ArmSmmu4KAuditResult *);
/* Read actual context before AND after the physical walks; use its observed
 * TTBR addresses as traversal roots. Require exact enabled 4K state and no
 * faults at both observations. No writes, activation or hardware timing proof.
 * Exclusive ownership/quiescence and coherent immutable tables are mandatory;
 * two observations cannot detect an unowned writer changing and restoring state. */
ArmSmmu4KStatus ArmSmmu4KObserveAuditedContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KAuditConfig *,ArmSmmu4KAuditResult *);
#ifdef __cplusplus
}
#endif
#endif
