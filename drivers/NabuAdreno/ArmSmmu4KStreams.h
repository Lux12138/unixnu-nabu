/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_STREAMS_H
#define ARM_SMMU_4K_STREAMS_H
#include "ArmSmmu4KProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {unsigned slot;uint16_t sid;} ArmSmmu4KStream;
typedef struct {
    unsigned attempted,attached,poisoned,count,bank;
    uint32_t id0,scr0;
    ArmSmmu4KStream streams[8];
    ArmSmmu4KShareability stream_shareability; /* Exact S2CR.SHCFG retained through faulting. */
} ArmSmmu4KStreamState;
typedef struct {
    unsigned phase,reads,writes,streams_verified,streams_faulted,must_retain_resources;
    uint32_t id0,id1,scr0,global_fault;   /* NABU-A640-NO-STAGE2: IDR1.S2 counts stage-2 banks */
    ArmSmmu4KProgramResult context;
} ArmSmmu4KStreamResult;
/* Read-only preflight of global fault policy, target faulting slots and all
 * conflicting live routes, including unrelated SIDs using the selected bank.
 * The supplied shareability must match the faulting S2CRs exactly. Intended
 * before programming a disabled context under the same global lease. */
ArmSmmu4KStatus ArmSmmu4KCheckFaultedStreams(const ArmSmmu4KIo *,const ArmSmmu4KStream *,
    unsigned count,unsigned bank,ArmSmmu4KShareability,ArmSmmu4KStreamResult *);
/* Native SMMUv2 exact stream matching (SMS), both legacy and EXIDS layouts.
 * Caller owns the listed slots, bank/ASID, mappings and a global configuration
 * lock. All client masters MUST remain quiesced until successful attachment.
 * Requires live powered MMIO and installed context/global fault handlers,
 * active native4K context/tables with verified physical ownership/coherence,
 * and global fault-on-unmatched (no bypass). Global setup/quirks and context
 * creation belong to the platform; no global reset or mask probing occurs.
 * All target S2CRs must already fault; valid target SMRs must be exact matches.
 * Every other active SMR is checked for overlapping requested SIDs. No masked
 * attachment, bypass, privilege override, implicit slot selection or context
 * reassignment is supported. Reads actual ID0 and enabled context registers.
 * Inputs/state/result must be disjoint; state is zero-initialized and remains
 * exclusively owned. Configuration/tables remain immutable while attached.
 * Writes S2CR before publishing SMR; EXIDS keeps EXIDVALID clear until the
 * exact SMR has been read back. Success rereads actual context/stream state.
 * Any failure after a mutation attempt faults only the leased touched slots,
 * poisons state and forbids replay. Resources are retained on EVERY outcome.
 * Successful software I/O fixtures are not proof of active hardware tables. */
ArmSmmu4KStatus ArmSmmu4KAttachStreams(const ArmSmmu4KIo *,ArmSmmu4KStreamState *,
    const ArmSmmu4KStream *,unsigned count,unsigned bank,const ArmSmmu4KContextConfig *,ArmSmmu4KStreamResult *);
/* Read-only, checks all routes/conflicts/global fault policy and complete
 * enabled native4K context image. No fault clear or recovery is synthesized. */
ArmSmmu4KStatus ArmSmmu4KObserveStreams(const ArmSmmu4KIo *,const ArmSmmu4KStreamState *,
    const ArmSmmu4KContextConfig *,ArmSmmu4KStreamResult *);
/* Read-only, the global fault policy, the complete enabled native4K context
 * image and the exact owned routes, without the scan of every other slot for
 * conflicting routes: a fixed read count however many slots the SMMU has. A
 * route another agent added later is only seen by ArmSmmu4KObserveStreams,
 * which a caller observing this way must still run at a bounded interval. */
ArmSmmu4KStatus ArmSmmu4KObserveOwnedStreams(const ArmSmmu4KIo *,const ArmSmmu4KStreamState *,
    const ArmSmmu4KContextConfig *,ArmSmmu4KStreamResult *);
/* Require exact owned active routes before changing them to FAULT. SMRs stay
 * valid, so those SIDs remain blocked. Does not drain transactions/TLBs, stop
 * devices, disable context, release ownership or authorize freeing tables.
 * Failure poisons state; external real reset/recovery is required. */
ArmSmmu4KStatus ArmSmmu4KFaultStreams(const ArmSmmu4KIo *,ArmSmmu4KStreamState *,ArmSmmu4KStreamResult *);
#ifdef __cplusplus
}
#endif
#endif
