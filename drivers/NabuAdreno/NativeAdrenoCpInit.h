/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_CPINIT_H
#define NATIVE_ADRENO_CPINIT_H
#include "NativeAdrenoRing.h"
struct NativeAdrenoCpInitConfig {
    uint64_t sqe_iova;             /* CP_SQE_INSTR_BASE, 4 KiB aligned, nonzero */
    uint64_t pwrup_reglist_iova;   /* Power-up register list, 4-byte aligned */
    /* Nonzero when the caller already wrote CP_SQE_INSTR_BASE before CP_SQE_CNTL, as a6xx hw_init does: the base is
     * then read back and compared instead of rewritten after the SQE started. */
    uint32_t sqe_base_programmed;
    NativeAdrenoGeneration generation=NativeAdrenoA740;
};
/* NativeAdrenoCpInit starts one command processor after GMU boot and GX power.
 * The caller supplies SQE microcode already copied to its mapped GPU address
 * and a register-list page retained for the entire boot. A642L uses the A660
 * eleven-word CP_ME_INIT payload and starts SQE through CP_SQE_CNTL. A740
 * sends CP_THREAD_CONTROL followed by its seven-word CP_ME_INIT payload.
 * A642L bootstrap waits for hardware idle before the caller authenticates ZAP
 * and exits secure mode. A740 initialization appends a timestamp fence.
 * Each object accepts one initialization attempt. */
class NativeAdrenoCpInit final {
    bool done_=false,poisoned_=false;
    IOReturn poison();
public:
    NativeAdrenoCpInit()=default;
    ~NativeAdrenoCpInit()=default;
    NativeAdrenoCpInit(const NativeAdrenoCpInit &)=delete;
    NativeAdrenoCpInit &operator=(const NativeAdrenoCpInit &)=delete;
    IOReturn run(const NativeAdrenoRingIo *,NativeAdrenoRing *,
        const NativeAdrenoCpInitConfig *,uint32_t *fence);
    IOReturn runBootstrap(const NativeAdrenoRingIo *,NativeAdrenoRing *,
        const NativeAdrenoCpInitConfig *,uint32_t *wptr);
    bool completed() const {return done_;}
    bool poisoned() const {return poisoned_;}
    static uint32_t meInitMask(NativeAdrenoGeneration generation=NativeAdrenoA740);
};
#endif
