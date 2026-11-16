/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_LIVE_H
#define ARM_SMMU_4K_LIVE_H
#include "ArmSmmu4KProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    ArmSmmu4KConfig config;
    uint64_t iova,bytes;              /* Bounded native VA aperture, no conversion. */
    unsigned upper_region;
    volatile uint64_t *mapping;       /* Real writable CPU mapping of owned pool. */
    size_t capacity_pages;
    /* Optional caller-owned scratch for the physical-alias checks (one entry per range of the smaller compared side), used by one
     * call at a time; absent or too small, those checks compare pairwise. Never read as state. */
    uint32_t *alias_scratch;
    size_t alias_scratch_entries;
} ArmSmmu4KLiveArena;
typedef struct {
    ArmSmmu4KResult tables;
    uint64_t generation;
    unsigned prepared,poisoned;
    /* The context bank's register offset observed during a table transaction; zero before the first observation. */
    uint64_t context_offset;
} ArmSmmu4KLiveState;
typedef struct {
    void *cookie;
    /* Clean/publish the actual mapped table page for device walks. Required
     * even for coherent walks (where the platform may use ordering alone).
     * Return zero only after visibility is established. No fake success. */
    int (*publish_table_page)(void *,volatile uint64_t *,size_t bytes);
} ArmSmmu4KLiveVisibility;
typedef struct {
    const ArmSmmu4KLiveArena *arena;
    ArmSmmu4KLiveState *state;
    const ArmSmmu4KRange *ranges;
    size_t count;
} ArmSmmu4KLiveRegion;
typedef struct {
    size_t descriptor_reads,descriptor_writes,published_pages;
    unsigned complete,translation_removed,must_retain_resources;
    unsigned whole_verification;   /* This update checked every descriptor in the region. */
    ArmSmmu4KProgramResult synchronization;
} ArmSmmu4KLiveResult;
/* Prepare an INACTIVE empty native4K arena. Preallocate only real intermediate
 * table pages; all leaf slots remain invalid. No MMIO, TLB, allocator or cache
 * operations. The explicit leaf budget bounds aperture size and traversal.
 * state must be zero initialized and never reset while a context can access
 * the pool. Capacity errors do not write tables. Caller must publish all used
 * pages before enabling a context with &state->tables. This is not activation.
 * The pool uses a fixed owned topology; no shadow/rebuilt translation exists. */
ArmSmmu4KStatus ArmSmmu4KLivePrepare(const ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *);
/* Read-only exact graph check, including reserved empty intermediate tables.
 * Use before context activation and while masters are quiesced. This neither
 * observes a context/streams nor clears poison or authorizes resource release. */
ArmSmmu4KStatus ArmSmmu4KLiveAudit(const ArmSmmu4KLiveArena *,const ArmSmmu4KLiveState *,
    const ArmSmmu4KRange *ranges,size_t count,ArmSmmu4KLiveResult *);
/* Incrementally map (map=1) an entirely invalid range, or unmap (map=0) an
 * exactly matching mapped range. Replacement requires unmap then map; valid
 * descriptors are never overwritten with different valid descriptors.
 * before describes ALL currently mapped resources, sorted by native IOVA.
 * Exact physical graph checks precede/follow changes to the actual pool.
 * No copies/reference table graphs, block mappings or table retirement.
 *
 * Requires a SINGLE enabled region rooted in this arena, an exclusive powered
 * bank/ASID/stream lease, installed fault handling, and ALL masters quiesced
 * until successful completion. Caller establishes/retains actual CPU->PA
 * ownership, attributes, object references, data-cache ordering and queue
 * serialization. Metadata, arrays, state/results and table storage are disjoint
 * and immutable during each call except the explicit outputs/table writes.
 * These conditions are platform responsibilities, not caller boolean proofs.
 * On any uncertain context/table/cache/TLB failure the state is poisoned and
 * all old/new resources and tables must remain owned until an independently
 * verified quiesce/fault/disable/reset. No automatic rollback/replay occurs.
 * Successful unmap removes translation only; it does not generate a GPU fence
 * or establish completion of previous DMA. No hardware ownership is inferred
 * from declared addresses. The software tests are not hardware qualification. */
ArmSmmu4KStatus ArmSmmu4KLiveUpdate(const ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *,
    const ArmSmmu4KIo *,unsigned bank,const ArmSmmu4KContextConfig *,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *before,size_t count,
    const ArmSmmu4KRange *change,unsigned map,unsigned poll_budget,ArmSmmu4KLiveResult *);
/* Full enabled-context variant: supply exactly its enabled lower/upper regions.
 * Both actual pool graphs are audited; cross-pool resource access, pool CPU/PA
 * aliases and inconsistent cross-region physical aliases refuse before writes.
 * Context roots/configs must match the actual supplied arena/state pairs.
 * AuditContext is read-only and does not observe hardware or release anything.
 * UpdateContext edits only the selected region, publishes its changed pages,
 * synchronizes the actual shared ASID and rechecks BOTH graphs. Any uncertainty
 * poisons both states and retains all resources. All ownership/quiescence/cache
 * preconditions of LiveUpdate apply to the entire context. No context switch,
 * TTBR change, copied/converted/shadow mapping or GPU fence is implemented. */
ArmSmmu4KStatus ArmSmmu4KLiveAuditContext(const ArmSmmu4KContextConfig *,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper,ArmSmmu4KLiveResult *);
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *change,unsigned map,
    unsigned poll_budget,ArmSmmu4KLiveResult *);
/* UpdateContext for several ranges of one region in one transaction: changes are
 * sorted by native IOVA and disjoint; map wants each entirely invalid, unmap
 * each exactly mapped. Every check of UpdateContext covers all of them before
 * the first store; their leaf pages are published once, the ASID synchronized
 * once and both graphs rechecked once. A buffer of scattered physical pages is
 * one change per page, so its mapping costs one transaction, not one per page.
 * The changes may lie inside the target region's ranges (an unmap). */
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContextBatch(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *changes,size_t change_count,
    unsigned map,unsigned poll_budget,ArmSmmu4KLiveResult *);

/* UpdateContextRanges receives the physical ranges of one map or unmap. The
 * caller first audits the complete context with ArmSmmu4KLiveAuditContext and
 * then changes its tables only through the live update functions. The caller
 * checks each map's IOVA and physical cache/shareability/privilege attributes
 * against every existing object, serializes updates, and retains the context
 * bank, table allocations and resource ownership required by LiveUpdate.
 *
 * lower and upper identify the enabled arenas and states; their ranges/count
 * fields may be null/zero. The kit checks the arena bindings, retained leaf
 * budget, sorted changes, physical table-pool overlap and aliases within the
 * changes. It checks each changed leaf before writing it, publishes each
 * changed table page, synchronizes the ASID on unmap and checks the new leaves.
 * The first update also observes the context registers and saves their offset.
 * Existing objects remain in the caller's mapping records. A failed table,
 * context, publication or TLB operation poisons the affected states and keeps
 * must_retain_resources set until the caller establishes hardware quiescence. */
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContextRanges(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *changes,size_t change_count,
    unsigned map,unsigned poll_budget,ArmSmmu4KLiveResult *);

/* Optional monotonic microsecond clock for UpdateContextRanges. The caller
 * serializes timed updates and snapshots of the totals with its context lock.
 * Each successful call adds one complete row; failures leave totals unchanged. */
extern uint64_t (*arm_smmu_4k_live_now_us)(void);
/* Rows: map, unmap. Columns: calls; validation and initial context observation;
 * old leaf checks and context reread; writes and publication; TLB synchronization;
 * new leaf checks and context observation; maximum call time; changed pages. */
extern volatile uint64_t arm_smmu_4k_live_phase_us[2][8];
#ifdef __cplusplus
}
#endif
#endif
