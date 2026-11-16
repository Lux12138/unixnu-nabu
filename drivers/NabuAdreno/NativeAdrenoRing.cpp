/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoRing.h"
#include "NativeAdrenoGpuProfile.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* Values are taken from the pinned Linux source, not guessed:
 * drivers/gpu/drm/msm/registers/adreno/a6xx.xml (register offsets), and
 * registers/adreno/adreno_pm4.xml (opcode 0x46 CP_EVENT_WRITE7, event 0x31
 * CACHE_CLEAN, WRITE_SRC bits 20..22, WRITE_DST bit 24, WRITE_ENABLED bit 27,
 * IRQ bit 31). CP_RB_CNTL BUFSZ bits 0..5 and BLKSZ bits 8..13 are the same
 * fields MSM_GPU_RB_CNTL_DEFAULT programs. All are dword register indices. */
enum : uint32_t {
    RingBase = 0x0800,        /* 64-bit CP_RB_BASE */
    RingControl = 0x0802,     /* CP_RB_CNTL */
    RingRptrAddress = 0x0804, /* 64-bit CP_RB_RPTR_ADDR */
    RingRptr = 0x0806,
    RingWptr = 0x0807,
    RingNop = 0x10,
    RingScratch = 0x0885,     /* CP_SCRATCH(2) for A6XX..A7XX */
    RingFenceOpcode = 0x46,   /* CP_EVENT_WRITE7 on A7XX */
    RingFenceEvent = 0x31,    /* CACHE_CLEAN */
    RingFenceEnable = 1u << 27,
    RingFenceIrq = 1u << 31,
    RingBlockBytes = 32,
    /* A7XX framing, as a7xx_submit() in the pinned a6xx_gpu.c emits it: CP_THREAD_CONTROL (0x17; THREAD bits 0..1,
     * BR 1, BV 2, BOTH 3; SYNC_THREADS bit 31), CP_SET_MARKER (0x65; 0x101 IFPC disable, 0x100 enable), CCU_INVALIDATE_DEPTH
     * 0x18 / CCU_INVALIDATE_COLOR 0x19 and CP_WAIT_TIMESTAMP (0x14) from adreno_pm4.xml. User command streams synchronize
     * the BR and BV threads themselves (Turnip waits on BV timestamps), so the IBs must run with both threads enabled. */
    RingThreadControl = 0x17,
    RingThreadBr = 1,
    RingThreadBv = 2,
    RingThreadBoth = 3,
    RingSyncThreads = 1u << 31,
    RingSetMarker = 0x65,
    RingWaitTimestamp = 0x14,
    RingCcuInvalidateDepth = 0x18,
    RingCcuInvalidateColor = 0x19,
    /* What the SQE's CP_SMMU_TABLE_UPDATE handler does before it switches tables (a740_sqe.fw, qrisc-disasm): CACHE_FLUSH7 (0x32)
     * and CACHE_INVALIDATE7 (0x33) with a wait for idle (CP_WAIT_FOR_IDLE 0x26). Linux gets these from that packet on every context
     * switch; this ring never sends it, and event 0x31 is only CACHE_CLEAN on A7XX, so nothing else ever invalidated UCHE. */
    RingCacheFlush7 = 0x32,
    RingCacheInvalidate7 = 0x33,
    RingWaitForIdle = 0x26,
    RingPrologueWords = 11,
    /* a6xx_set_pagetable()'s A7XX context entry: CP_RESET_CONTEXT_STATE (0x1f; CLEAR_ON_CHIP_TS, CLEAR_RESOURCE_TABLE,
     * CLEAR_BV_BR_COUNTER, RESET_GLOBAL_LOCAL_TS = bits 0..3) between thread syncs, LRZ_FLUSH_INVALIDATE (0x26), the
     * RBBM_PERFCTR_SRAM_INIT_CMD (0x050e) write and CACHE_INVALIDATE (0x31), after a CP_WAIT_TIMESTAMP on the last fence, then the
     * CP_WAIT_REG_MEM (0x3c, WRITE_EQ) on RBBM_PERFCTR_SRAM_INIT_STATUS (0x050f) bit 0 that Linux always emits after the SRAM init
     * write. Without that wait the client's first IB started while the SRAM was still clearing; on sheng (int-064..067) the CP then
     * stopped on the IB's first CP_THREAD_CONTROL, while the same IB retired under the Linux driver. */
    RingResetContextState = 0x1f,
    RingResetAll = 0xf,
    RingLrzFlushInvalidate = 0x26,
    RingCacheInvalidate = 0x31,
    RingPerfctrSramInit = 0x050e,
    RingPerfctrSramInitStatus = 0x050f,
    RingWaitRegMem = 0x3c,
    RingWaitWriteEq = 3,
    RingResetWords = 26,
    RingFenceWords = 7,       /* kernel bring-up submissions: TYPE4 scratch + TYPE7 event */
    RingFramedFenceWords = 31,/* user submissions: scratch, BV timestamp, BR wait, ring timestamp */
    RingBvFenceOffset = 12,   /* msm_rbmemptrs.bv_fence; +8 is bv_rptr, the BV RPTR shadow hw init programs */
    RingProfileOffset = 64,
    RingProfileSlotBytes = 32,
    RingProfileSlots = 16,
    RingProfileWords = 9
};
static uint32_t ilog2(size_t value)
{
    uint32_t result=0;while(value>1) {value>>=1;++result;}return result;
}
/* The last submissions as the CP will read them: [slot] fence, start index, end index, word count, then up to 112 ring words from the start. The board log
 * prints them, so a fence the GPU never wrote can be compared with the packets that should have written it. */
extern "C" {volatile uint32_t native_adreno_ring_log[4][4+112];volatile uint32_t native_adreno_ring_log_count;}
IOReturn NativeAdrenoRing::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
IOReturn NativeAdrenoRing::readRptr(uint32_t *value)
{
    uint32_t rptr=0;
    if(io_.read32(io_.cookie,RingRptr,&rptr))return poison();
    if(rptr>=ring_bytes_/4)return poison(); /* unusable read pointer */
    *value=rptr;rptr_shadow_=rptr;return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::reserveA6xx(uint32_t words,uint32_t *at)
{
    const uint32_t size=uint32_t(ring_bytes_/4);
    if(!words || words>size-3 || !at) {
        return kIOReturnBadArgument;
    }
    uint32_t rptr=0;
    IOReturn status=readRptr(&rptr);
    if(status) {
        return status;
    }
    if(next_>=rptr) {
        if(words<=size-2-next_) {
            *at=next_;
            return kIOReturnSuccess;
        }
        if(words>=rptr) {
            return kIOReturnNoResources;
        }
        const uint32_t remaining=size-next_;
        uint32_t header=0;
        if(remaining<2 || AdrenoPM4Type7(RingNop,remaining-1,&header)!=AdrenoPM4Success) {
            return poison();
        }
        ring_[next_]=header;
        for(uint32_t i=next_+1;i<size;++i) {
            ring_[i]=0;
        }
        *at=0;
        return kIOReturnSuccess;
    }
    if(words>=rptr-next_) {
        return kIOReturnNoResources;
    }
    *at=next_;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::commit(uint32_t end,uint32_t serial)
{
    const uint32_t mask=uint32_t(ring_bytes_/4-1);
    const uint32_t slot=native_adreno_ring_log_count&3;
    const uint32_t total=(end-next_)&mask;
    volatile uint32_t *record=native_adreno_ring_log[slot];
    record[0]=serial;
    record[1]=next_;
    record[2]=end;
    record[3]=total;
    for(uint32_t i=0;i<112;++i) {
        record[4+i]=i<total?ring_[(next_+i)&mask]:0;
    }
    native_adreno_ring_log_count=native_adreno_ring_log_count+1;
    if(cache_.publish(cache_.cookie,ring_,ring_bytes_) || io_.barrier(io_.cookie)) {
        return poison();
    }
    const uint32_t profileSlot = serial % RingProfileSlots;
    if (profileWindows_[profileSlot] && profileFences_[profileSlot] == serial) {
        uint64_t ticks = 0;
        profileIssueValid_[profileSlot] = io_.readAlwaysOn &&
            io_.readAlwaysOn(io_.cookie, &ticks) == 0;
        profileIssueTicks_[profileSlot] = uint32_t(ticks);
    }

    if(nativeAdrenoA6xx(generation_)) {
        if(!io_.writeWptr || io_.writeWptr(io_.cookie,end)) {
            return poison();
        }
    } else if(io_.write32(io_.cookie,RingWptr,end)) {
        return poison();
    }
    next_=end;
    return kIOReturnSuccess;
}
/* Packs one TYPE7 packet into words; returns the next free index or 0 when the header cannot be built. */
static size_t packet(uint32_t *words,size_t at,uint32_t opcode,const uint32_t *payload,uint32_t count)
{
    if(AdrenoPM4Type7(opcode,count,&words[at])!=AdrenoPM4Success)return 0;
    for(uint32_t i=0;i<count;++i)words[at+1+i]=payload[i];
    return at+1+count;
}
/* Thread selection and IFPC disable before the user buffers, as a7xx_submit() does; the first submission after
 * requestContextReset() also carries the context entry a6xx_set_pagetable() emits when a new context first submits.
 * Every prologue ends its BR-only part with the GPU cache flush and invalidate described at RingCacheFlush7. */
IOReturn NativeAdrenoRing::emitPrologue(uint32_t at,bool reset,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    if(nativeAdrenoA6xx(generation_)) {
        uint32_t words[7]={0};
        const uint32_t disable=0x101,start=0xd,invalidate=RingCacheInvalidate;
        size_t count=packet(words,0,RingSetMarker,&disable,1);
        if(reset) {
            count=packet(words,count,RingWaitForIdle,nullptr,0);
            /* Context entry must invalidate UCHE before the new user's IBs,
             * as a6xx_set_pagetable() and KGSL a6xx_rb_context_switch() do. */
            count=packet(words,count,RingFenceOpcode,&invalidate,1);
        }
        count=packet(words,count,RingSetMarker,&start,1);
        if(count!=(reset?7u:4u))return poison();
        for(size_t i=0;i<count;++i) {
            ring_[(at+i)&mask]=words[i];
        }
        *end=(at+uint32_t(count))&mask;
        return kIOReturnSuccess;
    }
    uint32_t words[RingPrologueWords+RingResetWords]={0};
    const uint32_t br=RingSyncThreads|RingThreadBr,syncBoth=RingSyncThreads|RingThreadBoth,both=RingThreadBoth,marker=0x101;
    const uint32_t count=RingPrologueWords+(reset?RingResetWords:0);
    size_t n=packet(words,0,RingThreadControl,&br,1);
    if(reset) {
        const uint64_t at4=fence_iova_+4;
        const uint32_t wait[4]={0,(uint32_t)at4,(uint32_t)(at4>>32),seqno_},all=RingResetAll,lrz=RingLrzFlushInvalidate,
            invalidate=RingCacheInvalidate;
        if(n)n=packet(words,n,RingWaitTimestamp,wait,4);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingResetContextState,&all,1);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingFenceOpcode,&lrz,1);
        if(n)n=packet(words,n,RingThreadControl,&br,1);
        if(n && AdrenoPM4Type4(RingPerfctrSramInit,1,&words[n])==AdrenoPM4Success) {words[n+1]=1;n+=2;} else n=0;
        if(n)n=packet(words,n,RingFenceOpcode,&invalidate,1);
        const uint32_t sram[6]={RingWaitWriteEq,RingPerfctrSramInitStatus,0,1,1,0};
        if(n)n=packet(words,n,RingWaitRegMem,sram,6);
    }
    const uint32_t flush=RingCacheFlush7,invalidate7=RingCacheInvalidate7;
    if(n)n=packet(words,n,RingFenceOpcode,&flush,1);
    if(n)n=packet(words,n,RingFenceOpcode,&invalidate7,1);
    if(n)n=packet(words,n,RingWaitForIdle,nullptr,0);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    if(n!=count)return poison();
    for(uint32_t i=0;i<count;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+count)&mask;return kIOReturnSuccess;
}
/* Writes the timestamp fence that makes a submission observable. */
IOReturn NativeAdrenoRing::emitFence(uint32_t at,uint32_t seqno,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[RingFenceWords]={0};
    if(AdrenoPM4Type4(RingScratch,1,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(RingFenceOpcode,4,&words[2])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    words[3]=nativeAdrenoA6xx(generation_)?
        0x04u|RingFenceIrq:RingFenceEvent|RingFenceEnable|RingFenceIrq;
    /* The timestamp goes to +4 (msm_rbmemptrs.fence), where wait() reads it; +0 is the CP's RPTR shadow. */
    const uint64_t at4=fence_iova_+4;
    words[4]=(uint32_t)at4;words[5]=(uint32_t)(at4>>32);words[6]=seqno;
    for(uint32_t i=0;i<RingFenceWords;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+RingFenceWords)&mask;return kIOReturnSuccess;
}
/* The fence of a framed submission: CP_SCRATCH(2), the BV thread's timestamp, a BR wait for it (so BR cannot report the
 * submission while BV still works on it), then the ring timestamp with an interrupt, both threads and IFPC back on. */
IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end)
{
    if(nativeAdrenoA6xx(generation_)) {
        const uint32_t mask=uint32_t(ring_bytes_/4-1);
        uint32_t words[6]={0};
        const uint32_t finish=0xe;
        const uint32_t depth=RingCcuInvalidateDepth,color=RingCcuInvalidateColor;
        size_t count=packet(words,0,RingSetMarker,&finish,1);
        count=packet(words,count,RingFenceOpcode,&depth,1);
        count=packet(words,count,RingFenceOpcode,&color,1);
        for(size_t i=0;i<count;++i) {
            ring_[(at+i)&mask]=words[i];
        }
        IOReturn status=emitFence((at+uint32_t(count))&mask,seqno,end);
        if(status) {
            return status;
        }
        const uint32_t enable=0x100;
        packet(words,0,RingSetMarker,&enable,1);
        ring_[*end]=words[0];
        ring_[(*end+1)&mask]=words[1];
        *end=(*end+2)&mask;
        return kIOReturnSuccess;
    }
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[RingFramedFenceWords]={0};
    if(AdrenoPM4Type4(RingScratch,1,&words[0])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    /* The timestamp goes to +4 (msm_rbmemptrs.fence), where wait() reads it; +0 is the CP's RPTR shadow. */
    const uint64_t at4=fence_iova_+4,bv=fence_iova_+RingBvFenceOffset;
    const uint32_t br=RingThreadBr,bvThread=RingThreadBv,both=RingThreadBoth,depth=RingCcuInvalidateDepth,
        color=RingCcuInvalidateColor,marker=0x100;
    const uint32_t bvWrite[4]={RingFenceEvent|RingFenceEnable,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t bvWait[4]={0,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t fence[4]={RingFenceEvent|RingFenceEnable|RingFenceIrq,(uint32_t)at4,(uint32_t)(at4>>32),seqno};
    size_t n=packet(words,2,RingThreadControl,&br,1);
    if(n)n=packet(words,n,RingFenceOpcode,&depth,1);
    if(n)n=packet(words,n,RingFenceOpcode,&color,1);
    if(n)n=packet(words,n,RingThreadControl,&bvThread,1);
    if(n)n=packet(words,n,RingFenceOpcode,bvWrite,4);
    if(n)n=packet(words,n,RingThreadControl,&br,1);
    if(n)n=packet(words,n,RingWaitTimestamp,bvWait,4);
    if(n)n=packet(words,n,RingFenceOpcode,fence,4);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    if(n!=RingFramedFenceWords)return poison();
    for(uint32_t i=0;i<RingFramedFenceWords;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+RingFramedFenceWords)&mask;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *io,const NativeAdrenoRingCache *cache,
    void *ring,size_t ring_bytes,uint64_t ring_iova,void *fence,size_t fence_bytes,uint64_t fence_iova,
    NativeAdrenoGeneration generation)
{
    if(attached_ || poisoned_ || ring_ || fence_)return kIOReturnBusy;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !cache ||
        !cache->publish || !cache->acquire || !ring || !fence ||
        ring_bytes<8192 || ring_bytes>(1u<<20) || (ring_bytes&(ring_bytes-1)) ||
        ((ring_iova|ring_bytes)&(ring_bytes-1)) || (ring_iova&4095) ||
        fence_bytes<64 || (fence_iova&7) || fence_bytes>=(1u<<24) ||
        (nativeAdrenoA6xx(generation) && !io->writeWptr))return kIOReturnBadArgument;
    /* No register may be touched before every argument is acceptable. */
    generation_=generation;
    io_=*io;cache_=*cache;ring_=static_cast<volatile uint32_t *>(ring);
    ring_bytes_=ring_bytes;ring_iova_=ring_iova;
    fence_=static_cast<volatile uint32_t *>(fence);fence_bytes_=fence_bytes;fence_iova_=fence_iova;
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    if(nativeAdrenoA6xx(generation_)) {
        for(size_t i=0;i<ring_bytes_/4;++i) {
            ring_[i]=0xaaaaaaaa;
        }
    }
    const uint32_t control=(uint32_t)(ilog2(ring_bytes/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8)|
        (generation_==NativeAdrenoA640?(1u<<27):0u);
    const uint32_t base_lo=(uint32_t)ring_iova_,base_hi=(uint32_t)(ring_iova_>>32);
    const uint32_t rptr_lo=(uint32_t)fence_iova_,rptr_hi=(uint32_t)(fence_iova_>>32);
    if(nativeAdrenoA6xx(generation_)) {
        const uint32_t writes[]={RingRptrAddress,rptr_lo,RingRptrAddress+1,rptr_hi,
            RingControl,control,RingBase,base_lo,RingBase+1,base_hi};
        for(size_t i=0;i<sizeof(writes)/sizeof(writes[0]);i+=2) {
            if(io_.write32(io_.cookie,writes[i],writes[i+1])) {
                return poison();
            }
        }
        next_=0;
        rptr_shadow_=0;
    } else {
        const uint32_t writes[]={RingBase,base_lo,RingBase+1,base_hi,RingControl,control,
            RingRptrAddress,rptr_lo,RingRptrAddress+1,rptr_hi,RingWptr,0,RingScratch,0};
        for(size_t i=0;i<sizeof(writes)/sizeof(writes[0]);i+=2) {
            if(io_.write32(io_.cookie,writes[i],writes[i+1])) {
                return poison();
            }
        }
        uint32_t rptr=0;
        const IOReturn status=readRptr(&rptr);
        if(status) {
            return status;
        }
        next_=rptr;
    }
    seqno_=0;completed_=0;submissions_=0;bootstrap_pending_=false;
    attached_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::detach()
{
    if(poisoned_)return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    if(outstanding())return kIOReturnBusy;
    attached_=false;ring_=nullptr;fence_=nullptr;ring_bytes_=fence_bytes_=0;
    ring_iova_=fence_iova_=0;next_=rptr_shadow_=seqno_=completed_=submissions_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::submit(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    return submitWords(words,word_count,fence,false);
}

IOReturn NativeAdrenoRing::submitBootstrap(const uint32_t *words,size_t word_count,uint32_t *wptr)
{
    if(!wptr) {
        return kIOReturnBadArgument;
    }
    *wptr=0;
    if(!attached_ || poisoned_ || bootstrap_pending_ || seqno_ ||
        !nativeAdrenoA6xx(generation_)) {
        return kIOReturnNotReady;
    }
    if(!words || !word_count || word_count>ring_bytes_/4-3) {
        return kIOReturnBadArgument;
    }
    uint32_t at=0;
    IOReturn status=reserveA6xx(uint32_t(word_count),&at);
    if(status) {
        return status;
    }
    for(size_t i=0;i<word_count;++i) {
        ring_[at+i]=words[i];
    }
    const uint32_t end=at+uint32_t(word_count);
    status=commit(end,0);
    if(status) {
        return status;
    }
    bootstrap_pending_=true;
    *wptr=end;
    IOLog("NativeAdrenoRing: bootstrap publication completed wptr=%u\n",end);
    return kIOReturnSuccess;
}

void NativeAdrenoRing::logBootstrap(uint32_t target,uint32_t rptr,uint32_t busy,const char *result)
{
    const uint32_t regs[]={RingWptr,0x210,0x213,0x201,0x821,0x823,0x824};
    uint32_t values[7]={0};
    uint32_t failed=0;
    for(size_t i=0;i<sizeof(regs)/sizeof(regs[0]);++i) {
        if(io_.read32(io_.cookie,regs[i],&values[i])) {
            failed|=1u<<i;
        }
    }
    IOLog("NativeAdrenoRing: bootstrap %s target=%u rptr=%u wptr=%u busy=%08x rbbm=%08x/%08x int=%08x\n",
        result,target,rptr,values[0],busy,values[1],values[2],values[3]);
    IOLog("NativeAdrenoRing: bootstrap fault=%08x cp=%08x protect=%08x failed_reads=%x\n",
        values[4],values[5],values[6],failed);
}

NativeAdrenoRingFence NativeAdrenoRing::waitBootstrap(uint32_t wptr,
    const NativeAdrenoRingClock *clock,uint32_t timeout_us,uint32_t *observed)
{
    if(observed) {
        *observed=0;
    }
    if(!attached_ || poisoned_) {
        return NativeAdrenoRingFencePoisoned;
    }
    if(!nativeAdrenoA6xx(generation_) || !bootstrap_pending_ || wptr!=next_ ||
        !clock || !clock->now_us || !timeout_us) {
        return NativeAdrenoRingFenceArgument;
    }
    const uint32_t fatalInterrupts=(1u<<1)|(1u<<6)|(1u<<7)|(1u<<9)|
        (1u<<22)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<28);
    const uint64_t start=clock->now_us(clock->cookie);
    uint32_t rptr=0,busy=0;
    for(;;) {
        uint32_t interrupts=0,cp=0;
        if(readRptr(&rptr) || io_.read32(io_.cookie,0x201,&interrupts) ||
            io_.read32(io_.cookie,0x823,&cp)) {
            logBootstrap(wptr,rptr,busy,"read failed");
            poison();
            return NativeAdrenoRingFencePoisoned;
        }
        if(observed) {
            *observed=rptr;
        }
        bool idle=false;
        if(io_.readGpuBusy) {
            if(io_.readGpuBusy(io_.cookie,&busy)) {
                logBootstrap(wptr,rptr,busy,"GMU read failed");
                poison();
                return NativeAdrenoRingFencePoisoned;
            }
            idle=(busy&(1u<<23))==0;
        } else {
            if(io_.read32(io_.cookie,0x210,&busy)) {
                logBootstrap(wptr,rptr,busy,"RBBM read failed");
                poison();
                return NativeAdrenoRingFencePoisoned;
            }
            idle=(busy&0xfffffffeu)==0;
        }
        if((interrupts&fatalInterrupts) || (cp&0xf7u)) {
            logBootstrap(wptr,rptr,busy,"hardware fault");
            poison();
            return NativeAdrenoRingFencePoisoned;
        }
        if(rptr==wptr && idle) {
            bootstrap_pending_=false;
            logBootstrap(wptr,rptr,busy,"idle");
            return NativeAdrenoRingFenceComplete;
        }
        if(clock->now_us(clock->cookie)-start>=timeout_us) {
            logBootstrap(wptr,rptr,busy,"timeout");
            return NativeAdrenoRingFenceTimeout;
        }
        if(clock->idle && clock->idle(clock->cookie)) {
            logBootstrap(wptr,rptr,busy,"wait interrupted");
            return NativeAdrenoRingFenceTimeout;
        }
    }
}

IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    return submitWords(words,word_count,fence,true);
}
IOReturn NativeAdrenoRing::submitWords(const uint32_t *words, size_t word_count, uint32_t *fence, bool framed)
{
    if (!fence) {
        return kIOReturnBadArgument;
    }

    *fence = 0;
    if (!attached_ || poisoned_ || bootstrap_pending_) {
        return kIOReturnNotReady;
    }
    if ((word_count && !words) || (!word_count && !framed) ||
        word_count > SIZE_MAX / sizeof(uint32_t)) {
        return kIOReturnBadArgument;
    }

    const uint32_t size_words = (uint32_t)(ring_bytes_ / 4);
    const uint32_t mask = size_words - 1;
    const bool reset = framed && reset_pending_;
    const uint32_t serial = seqno_ + 1;
    const uint32_t profileSlot = serial % RingProfileSlots;
    const bool profiling = framed && generation_ == NativeAdrenoA642L &&
        fence_bytes_ >= RingProfileOffset + RingProfileSlotBytes * RingProfileSlots &&
        !profileWindows_[profileSlot] && NativeAdrenoGpuProfile::active();
    const uint32_t frameExtra = nativeAdrenoA6xx(generation_) ?
        (framed ? 4 + (reset ? 3 : 0) + RingFenceWords + 8 : RingFenceWords) :
        (framed ? RingPrologueWords + (reset ? RingResetWords : 0) + RingFramedFenceWords : RingFenceWords);
    const uint32_t extra = frameExtra + (profiling ? RingProfileWords : 0);
    if (word_count > size_words - extra - 8) {
        return kIOReturnBadArgument;
    }

    uint32_t at = next_;
    IOReturn status = kIOReturnSuccess;
    if (nativeAdrenoA6xx(generation_)) {
        status = reserveA6xx(uint32_t(word_count) + extra, &at);
        if (status) {
            return status;
        }
    } else {
        uint32_t rptr = 0;
        status = readRptr(&rptr);
        if (status) {
            return status;
        }

        const uint32_t used = (next_ - rptr) & mask;
        if (word_count + extra + 8 > size_words - used) {
            return kIOReturnNoResources;
        }
    }

    if (framed) {
        status = emitPrologue(at, reset, &at);
        if (status) {
            return status;
        }
    }

    const uint32_t profileWindow = profiling ? NativeAdrenoGpuProfile::beginJob() : 0;
    const uint32_t profileOffset = RingProfileOffset + profileSlot * RingProfileSlotBytes;
    if (profileWindow) {
        profileWindows_[profileSlot] = profileWindow;
        profileFences_[profileSlot] = serial;
        profileIssueValid_[profileSlot] = false;

        volatile uint32_t *destination = fence_ + profileOffset / 4;
        destination[0] = 0;
        destination[1] = 0;
        destination[2] = 0;
        destination[3] = 0;

        // REG_TO_MEM reads CP's clock when CP reaches this IB list.
        uint32_t header = 0;
        AdrenoPM4Type7(0x3e, 3, &header);

        const uint64_t address = fence_iova_ + profileOffset;
        ring_[at] = header;
        ring_[(at + 1) & mask] = 0x980u | (2u << 18) | (1u << 30);
        ring_[(at + 2) & mask] = uint32_t(address);
        ring_[(at + 3) & mask] = uint32_t(address >> 32);
        at = (at + 4) & mask;
    }

    for (size_t i = 0; i < word_count; ++i) {
        ring_[(at + i) & mask] = words[i];
    }
    at = (at + (uint32_t)word_count) & mask;

    if (profileWindow) {
        // Turnip's A6xx u_trace uses RB_DONE_TS with bit 30 for the GPU clock.
        // Its low 32 bits complete before the existing CACHE_FLUSH_TS fence.
        uint32_t header = 0;
        AdrenoPM4Type7(0x46, 4, &header);

        const uint64_t address = fence_iova_ + profileOffset + 8;
        ring_[at] = header;
        ring_[(at + 1) & mask] = 0x16u | (1u << 30);
        ring_[(at + 2) & mask] = uint32_t(address);
        ring_[(at + 3) & mask] = uint32_t(address >> 32);
        ring_[(at + 4) & mask] = 0;
        at = (at + 5) & mask;
    }

    uint32_t end = 0;
    status = framed ? emitFramedFence(at, seqno_ + 1, &end) : emitFence(at, seqno_ + 1, &end);
    if (status) {
        collectGpuProfile(serial, 0, false);
        return status;
    }
    if (commit(end, seqno_ + 1)) {
        collectGpuProfile(serial, 0, false);
        return poison();
    }

    *fence = ++seqno_;
    ++submissions_;
    if (reset) {
        reset_pending_ = false;
    }

    return kIOReturnSuccess;
}

void NativeAdrenoRing::collectGpuProfile(uint32_t fence, uint32_t queue, bool complete)
{
    const uint32_t slot = fence % RingProfileSlots;
    const uint32_t capturedWindow = profileWindows_[slot];
    if (!capturedWindow || profileFences_[slot] != fence) {
        return;
    }

    uint32_t cp = 0;
    uint32_t rb = 0;
    if (complete) {
        __asm__ volatile("dsb ld" ::: "memory");

        const volatile uint32_t *source = fence_ + (RingProfileOffset + slot * RingProfileSlotBytes) / 4;
        cp = source[0];
        rb = source[2];
    }

    NativeAdrenoGpuProfile::finishJob(capturedWindow, fence, queue, profileIssueTicks_[slot],
        cp, rb, complete && (cp || rb), profileIssueValid_[slot]);
    profileWindows_[slot] = 0;
    profileFences_[slot] = 0;
}

NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t fence,
    const NativeAdrenoRingClock *clock,uint32_t timeout_us,uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || fence>seqno_)return NativeAdrenoRingFenceArgument;
    uint64_t start=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        if(cache_.acquire(cache_.cookie,fence_,fence_bytes_)) {poison();return NativeAdrenoRingFencePoisoned;}
        const uint32_t value=fence_[1];
        if(observed)*observed=value;
        if(value>seqno_) {poison();return NativeAdrenoRingFencePoisoned;}
        if(value>=fence)return NativeAdrenoRingFenceComplete;
        if(!clock || !clock->now_us)return NativeAdrenoRingFenceTimeout;
        if(clock->now_us(clock->cookie)-start>=timeout_us)return NativeAdrenoRingFenceTimeout;
        if(clock->idle && clock->idle(clock->cookie))return NativeAdrenoRingFenceTimeout;
    }
}
NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us,uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || fence>seqno_)return NativeAdrenoRingFenceArgument;
    uint64_t start=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        if(cache_.acquire(cache_.cookie,fence_,fence_bytes_)) {poison();return NativeAdrenoRingFencePoisoned;}
        const uint32_t value=fence_[1];
        if(observed)*observed=value;
        if(value>=fence) {if(value>seqno_) {poison();return NativeAdrenoRingFencePoisoned;}completed_=value;
            return NativeAdrenoRingFenceComplete;}
        if(!clock || !clock->now_us)return NativeAdrenoRingFenceTimeout;
        if(clock->now_us(clock->cookie)-start>=timeout_us)return NativeAdrenoRingFenceTimeout;
        if(clock->idle && clock->idle(clock->cookie))return NativeAdrenoRingFenceTimeout;
    }
}
