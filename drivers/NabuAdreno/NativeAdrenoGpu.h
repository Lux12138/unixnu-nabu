/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GPU_H
#define NATIVE_ADRENO_GPU_H
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoTimer.h"

/* One GPU lifecycle attempt: board power/GMU/SMMU actions are injected, while
 * ring programming, CP start and interrupt masking are the real components in
 * this repository. The board operations own every resource that must exist
 * before MMIO is touched; this object never powers, clocks or maps anything.
 * Order is fixed: reserve, boot GMU, program the GPU SMMU context, attach the
 * ring, start the CP and program the interrupt mask. A failure after a
 * successful reserve calls quiesce exactly once, never retries and never
 * reports readiness; components keep their own quarantine state. On success
 * the ring, timer and IRQ objects are attached and owned here and may be handed
 * to user clients. This is not a Metal device and does not prove hardware. */
struct NativeAdrenoGpuBoard {
    void *cookie;
    IOReturn (*reserve)(void *);       /* exclusive device, clocks, mappings */
    IOReturn (*bootGmu)(void *);       /* GMU firmware/HFI startup */
    IOReturn (*programSmmu)(void *);   /* streams + native4K context enabled */
    IOReturn (*quiesce)(void *);       /* stop/fault masters after a failure */
};
struct NativeAdrenoGpuConfig {
    NativeAdrenoRingIo mmio;           /* powered GPU register window */
    NativeAdrenoRingCache cache;       /* PoC publication for ring/fence */
    NativeAdrenoTimeIo time;           /* optional GPU counter window */
    NativeAdrenoRingClock clock;       /* required: bounds the CP start wait */
    uint32_t start_timeout_us;         /* required: CP start fence timeout */
    void *ring;
    size_t ring_bytes;
    uint64_t ring_iova;
    void *fence;
    size_t fence_bytes;
    uint64_t fence_iova;
    uint64_t sqe_iova;                 /* verified SQE microcode IOVA */
    uint64_t pwrup_reglist_iova;
};
enum { NativeAdrenoGpuSteps = 6 };
class NativeAdrenoGpu final {
    NativeAdrenoGpuBoard board_{};
    NativeAdrenoRing ring_;
    NativeAdrenoCpInit cp_;
    NativeAdrenoIrq irq_;
    NativeAdrenoTimer timer_;
    uint32_t steps_=0,fence_=0;
    bool reserved_=false,ready_=false,failed_=false,quiesced_=false,started_=false;
    IOReturn fail(IOReturn status);
public:
    NativeAdrenoGpu()=default;
    ~NativeAdrenoGpu()=default;
    NativeAdrenoGpu(const NativeAdrenoGpu &)=delete;
    NativeAdrenoGpu &operator=(const NativeAdrenoGpu &)=delete;
    /* One attempt. Returns the failing step's error; readiness is only true
     * when every step succeeded. Ring ownership transfers to this object. */
    IOReturn start(const NativeAdrenoGpuBoard *,const NativeAdrenoGpuConfig *);
    /* Only a ready GPU may be stopped; requires every submission observed. */
    IOReturn stop();
    bool ready() const {return ready_;}
    bool failed() const {return failed_;}
    bool quiesced() const {return quiesced_;}
    uint32_t stepsCompleted() const {return steps_;}
    uint32_t startFence() const {return fence_;}
    NativeAdrenoRing *ring() {return ready_?&ring_:nullptr;}
    NativeAdrenoTimer *timer() {return ready_&&timerTimeOk_?&timer_:nullptr;}
    NativeAdrenoIrq *interrupts() {return ready_?&irq_:nullptr;}
private:
    bool timerTimeOk_=false;
};
#endif
