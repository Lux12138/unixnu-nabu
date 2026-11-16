/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_CONTEXT_H
#define ARM_SMMU_4K_CONTEXT_H
#include "ArmSmmu4K.h"
#ifdef __cplusplus
extern "C" {
#endif
/* ArmSmmu4KShareability selects the SMMUv2 SHCFG transaction override. */
typedef enum {
    ArmSmmu4KShareabilityFromTransaction = 0,
    ArmSmmu4KShareabilityNonShareable = 3
} ArmSmmu4KShareability;

typedef struct {
    /* A config/result pair from Build, BuildUpper or LivePrepare; NULL pair disables it.
     * A live arena may have zero mapped leaves while retaining its table topology.
     * Both enabled regions need distinct physical pools and equal OAS/MAIR.
     * The trusted caller retains actual pool ownership and immutable results. */
    const ArmSmmu4KConfig *lower, *upper;
    const ArmSmmu4KResult *lower_tables, *upper_tables;
    uint32_t id0, id2;       /* Raw SMMUv2 identification register values. */
    unsigned asid;          /* 0..65535, ownership managed by the caller. */
    unsigned coherent_walk; /* Outer-shareable table walks: 1=WBWA, 0=NC. */
    unsigned stall_on_fault, continue_after_fault; /* Each 0 or 1. */
    ArmSmmu4KShareability stream_shareability; /* S2CR.SHCFG for each owned stream. */
    ArmSmmu4KShareability bypass_shareability; /* SCTLR.SHCFG for bypass transactions. */
} ArmSmmu4KContextConfig;
typedef struct {
    uint32_t cbar, cba2r, tcr, tcr2, mair0, mair1, sctlr, reserved_zero;
    uint64_t ttbr0, ttbr1;
} ArmSmmu4KContextImage;

/* Encode AArch64 stage1, stage2-bypass native4K SMMUv2 context registers.
 * Capability/width/result/refusal checks precede publishing an image. Failure
 * zeros it. Both TG0/TG1 select 4K even for a disabled region. No MMIO, power,
 * stream/context assignment, cache barriers, TLB operations or activation.
 * ID inputs are not proof that the caller read actual hardware. Inputs,
 * their pointed-to configs/results, and output must be disjoint and immutable
 * throughout the call; this is a trusted kernel integration boundary. */
ArmSmmu4KStatus ArmSmmu4KEncodeContext(const ArmSmmu4KContextConfig *,
    ArmSmmu4KContextImage *);
/* Compare all supplied readback fields with the freshly encoded expected
 * image. Caller must read real hardware and independently walk its actual
 * TTBR tables, verify ownership/coherency and handle faults/TLB lifetime.
 * Matching caller-supplied values alone never qualify an active GPU mapping. */
ArmSmmu4KStatus ArmSmmu4KValidateContextReadback(const ArmSmmu4KContextConfig *,
    const ArmSmmu4KContextImage *);
#ifdef __cplusplus
}
#endif
#endif
