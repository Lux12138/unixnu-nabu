/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_PROGRAM_H
#define ARM_SMMU_4K_PROGRAM_H
#include "ArmSmmu4KContext.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Byte offsets are relative to one real, owned SMMUv2 MMIO mapping. All I/O
 * callbacks return 0 on success. read/write64 must perform actual aligned
 * 64-bit TTBR accesses; barrier orders table visibility and device writes.
 * A barrier is NOT cache cleaning. No callback may silently fake success. */
typedef struct {
    void *cookie;
    size_t register_bytes;
    int (*read32)(void *,size_t,uint32_t *);
    int (*read64)(void *,size_t,uint64_t *);
    int (*write32)(void *,size_t,uint32_t);
    int (*write64)(void *,size_t,uint64_t);
    int (*barrier)(void *);
} ArmSmmu4KIo;
typedef struct {
    uint64_t context_offset;
    uint32_t id0,id1,id2,phase,writes,poll_reads,fault_status;
    uint32_t readback_valid,enabled,disabled_verified,tlb_synchronized,must_retain_tables;
    ArmSmmu4KContextImage readback;
} ArmSmmu4KProgramResult;

/* REAL register programming primitive, not a device-discovery/ownership layer.
 * Caller MUST hold an exclusive bank/ASID/stream lease, keep the device powered
 * and clocked, quiesce/fault its streams, install its fault interrupt handler,
 * establish platform ACTLR/security requirements, and clean/publish real owned
 * native4K tables before calling. Every argument/result must be disjoint.
 * Reads actual ID0/ID1/ID2 through I/O and overrides config's supplied IDs.
 * Refuses enabled banks before any write. Enables SCTLR only after register
 * readback and bounded ASID invalidation/synchronization. No global reset,
 * stream reassignment or other bank is touched. On failure attempts disable
 * and sync; must_retain_tables stays set unless both are confirmed. Success
 * also retains table ownership while the installed context is active.
 * This does not walk tables or establish actual GPU execution/correctness. */
ArmSmmu4KStatus ArmSmmu4KProgramDisabledContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,unsigned poll_budget,ArmSmmu4KProgramResult *);
/* Read-only observation of actual IDs and all owned context registers. Requires
 * exact enabled native4K image and no pending context fault. Does not walk the
 * physical tables or establish ownership. Readback is a momentary observation;
 * callers retain resources, quiescence/exclusive lease and fault handling. */
ArmSmmu4KStatus ArmSmmu4KObserveOwnedContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,ArmSmmu4KProgramResult *);
/* Publish already cleaned descriptor changes with a real ASID invalidation
 * and bounded context TLBSYNC. Observe the complete enabled native4K image and
 * faults before and after. Caller exclusively owns this bank/ASID, serializes
 * all page-table updates and keeps every master quiesced. A barrier is not a
 * cache clean. No SCTLR/TTBR/stream writes or recovery/reset are performed.
 * Any failure retains all mappings/resources; success is not a GPU fence. */
ArmSmmu4KStatus ArmSmmu4KSynchronizeOwnedContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,unsigned poll_budget,ArmSmmu4KProgramResult *);
/* TLBIASID (or, with all set, TLBIALL) and the bounded TLBSYNC of the context bank at context_offset alone: no image or fault
 * observation. For a caller that observed this owned bank's image at that offset already and keeps it owned (the live updater
 * between its whole-verification updates). */
ArmSmmu4KStatus ArmSmmu4KSyncContextTlb(const ArmSmmu4KIo *,uint64_t context_offset,unsigned asid,unsigned all,
    unsigned poll_budget,ArmSmmu4KProgramResult *);
/* As ArmSmmu4KSynchronizeOwnedContext, but with TLBIALL for the bank instead of TLBIASID: every cached translation of the bank goes,
 * whatever ASID it was tagged with. */
ArmSmmu4KStatus ArmSmmu4KInvalidateAllOwnedContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,unsigned poll_budget,ArmSmmu4KProgramResult *);
/* Disable only if actual stage/format/roots/ASID/attributes still match the
 * expected owned context. Retain bank, mappings and tables on any uncertainty.
 * Does not release the caller's lease or power/stream/interrupt resources. */
ArmSmmu4KStatus ArmSmmu4KDisableOwnedContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,unsigned poll_budget,ArmSmmu4KProgramResult *);
#ifdef __cplusplus
}
#endif
#endif
