/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_RING_H
#define NATIVE_ADRENO_RING_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoGeneration.h"

/* NativeAdrenoRing owns a command processor ring and its timestamp fence.
 * Register indices are dword offsets; the platform adds the MMIO base and
 * multiplies by four. Caller must already own an exclusive, powered GPU with
 * SQE microcode loaded (CP_SQE_INSTR_BASE), global reset released and all
 * masters quiesced while attach programs the ring. Ring and fence CPU
 * addresses must be actual writable mappings of the same IOVAs that were
 * mapped into the active native-4K GPU context. No MMIO base, IRQ, power,
 * GMU, context switching or per-client validation is implemented here.
 * Every submit publishes ring memory before advancing WPTR and completes
 * only when the CP timestamp value is observed at the fence address.
 * A failed WPTR write or cache publication is uncertain: the ring is poisoned
 * and every resource must be retained until an independently verified reset. */
struct NativeAdrenoRingIo {
    void *cookie;
    /* Return zero only on a completed access. */
    int (*write32)(void *,uint32_t reg,uint32_t value);
    int (*read32)(void *,uint32_t reg,uint32_t *value);
    int (*barrier)(void *);
    /* The A642L platform retries a dropped WPTR write through the GMU fence. */
    int (*writeWptr)(void *,uint32_t value);
    /* The GMU AO status uses bit 23 for GPU busy excluding AHB accesses. */
    int (*readGpuBusy)(void *,uint32_t *status);
    /* CPU timestamp reads use the GMU AO registers on GMU-equipped GPUs. */
    int (*readAlwaysOn)(void *, uint64_t *ticks);
};
struct NativeAdrenoRingCache {
    void *cookie;
    /* Clean owned ring bytes to PoC before the device reads them. */
    int (*publish)(void *,const volatile uint32_t *words,size_t bytes);
    /* Invalidate the fence page before the CPU reads CP-written values. */
    int (*acquire)(void *,volatile uint32_t *words,size_t bytes);
};
struct NativeAdrenoRingClock {
    void *cookie;
    uint64_t (*now_us)(void *);
    /* Optional; may sleep. Never used before a successful submit. */
    int (*idle)(void *);
};
enum NativeAdrenoRingFence {
    NativeAdrenoRingFenceComplete = 0,
    NativeAdrenoRingFenceTimeout,
    NativeAdrenoRingFencePoisoned,
    NativeAdrenoRingFenceArgument
};
class NativeAdrenoRing final {
    NativeAdrenoRingIo io_{};
    NativeAdrenoRingCache cache_{};
    volatile uint32_t *ring_=nullptr,*fence_=nullptr;
    size_t ring_bytes_=0,fence_bytes_=0;
    uint64_t ring_iova_=0,fence_iova_=0;
    uint32_t next_=0,rptr_shadow_=0,seqno_=0,completed_=0,submissions_=0;
    bool attached_=false,poisoned_=false,reset_pending_=false;
    bool bootstrap_pending_=false;
    uint32_t profileWindows_[16]{};
    uint32_t profileFences_[16]{};
    uint32_t profileIssueTicks_[16]{};
    bool profileIssueValid_[16]{};
    NativeAdrenoGeneration generation_=NativeAdrenoA740;
    IOReturn poison();
    IOReturn readRptr(uint32_t *);
    IOReturn reserveA6xx(uint32_t words,uint32_t *at);
    IOReturn commit(uint32_t end,uint32_t serial);
    void logBootstrap(uint32_t target,uint32_t rptr,uint32_t busy,const char *result);
    IOReturn emitPrologue(uint32_t at,bool reset,uint32_t *end);
    IOReturn emitFence(uint32_t at,uint32_t seqno,uint32_t *end);
    IOReturn emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end);
    IOReturn submitWords(const uint32_t *words,size_t word_count,uint32_t *fence,bool framed);
public:
    NativeAdrenoRing()=default;
    ~NativeAdrenoRing()=default;
    NativeAdrenoRing(const NativeAdrenoRing &)=delete;
    NativeAdrenoRing &operator=(const NativeAdrenoRing &)=delete;
    /* ring_bytes is a power of two in [8KiB,1MiB]; ring_iova must be aligned
     * to ring_bytes. fence_bytes >=64 and fence_iova 8-aligned. The fence page
     * layout is: +0 CP read-pointer shadow, +4 completed timestamp, +8 spare.
     * All outputs are prevalidated before any register write. */
    IOReturn attach(const NativeAdrenoRingIo *,const NativeAdrenoRingCache *,
        void *ring,size_t ring_bytes,uint64_t ring_iova,
        void *fence,size_t fence_bytes,uint64_t fence_iova,
        NativeAdrenoGeneration generation=NativeAdrenoA740);
    /* Programs nothing. Refuses while submissions are unobserved or poisoned,
     * because only the caller can establish real GPU quiescence and unmap. */
    IOReturn detach();
    /* Emit an entire command stream followed by the timestamp fence and commit
     * it with one WPTR update. Returns a nonzero fence value on success. The
     * words are copied; the caller's buffer is never retained. */
    IOReturn submit(const uint32_t *words,size_t word_count,uint32_t *fence);
    /* A642L bootstrap commands run before timestamp memory is usable. The
     * returned WPTR is completed only by waitBootstrap's hardware-idle check. */
    IOReturn submitBootstrap(const uint32_t *words,size_t word_count,uint32_t *wptr);
    NativeAdrenoRingFence waitBootstrap(uint32_t wptr,const NativeAdrenoRingClock *,
        uint32_t timeout_us,uint32_t *observed);
    /* A642L user buffers receive A6xx IB-list markers and CACHE_FLUSH_TS.
     * A740 user buffers receive its BR/BV thread synchronization packets. */
    IOReturn submitFramed(const uint32_t *words,size_t word_count,uint32_t *fence);
    /* The submission owner calls this after a normal fence observation, or
     * with complete=false after stopping DMA for a failed job. */
    void collectGpuProfile(uint32_t fence, uint32_t queue, bool complete);
    /* The next user submission waits for idle on A642L or clears the A740
     * thread counters, timestamps and resource table between thread syncs. */
    void requestContextReset() {reset_pending_=true;}
    /* Polls the CP-written timestamp after invalidating the fence page. Never
     * writes registers and never advances the ring. Timeout is not poison. */
    NativeAdrenoRingFence wait(uint32_t fence,const NativeAdrenoRingClock *,
        uint32_t timeout_us,uint32_t *observed);
    /* Observe a previously issued fence without retiring or accounting for it.
     * Used before a new submission waits on an earlier batch; only fence values
     * this ring issued are accepted. Never writes a register. */
    NativeAdrenoRingFence observeFence(uint32_t fence,const NativeAdrenoRingClock *,
        uint32_t timeout_us,uint32_t *observed);
    bool poisoned() const {return poisoned_;}
    /* A fatal GPU fault prevents further WPTR updates and retains live DMA. */
    void recordFault() {poisoned_=true;}
    uint32_t outstanding() const {return seqno_-completed_+(bootstrap_pending_?1:0);}
    uint32_t lastSubmitted() const {return seqno_;}      /* the newest fence number handed out */
    uint32_t completedFence() const {return completed_;}
    uint32_t submissions() const {return submissions_;}
    uint32_t readPointer() const {return rptr_shadow_;}
};
#endif
