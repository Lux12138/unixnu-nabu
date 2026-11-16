/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include "NabuGpuCopyPlan.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>

extern "C" uint64_t gPhysBase,gPhysSize;

/* The board diagnostic layout keeps allocation, prepare, spans, zero, four
 * reserved rows, complete and descriptor release. Each row records calls,
 * total us, maximum us and bytes. */
extern "C" {
extern uint64_t (*native_adreno_vm_now_us)(void);
extern unsigned int memorystatus_available_pages;
void proc_selfname(char *,int);
volatile uint64_t native_adreno_memory_stage_stats[10][4];
/* hits, misses, insertions, evictions, rejected insertions, cached objects,
 * cached bytes, peak cached bytes, pressure drains, allocation-failure drains,
 * pages restored to WC on reuse, quarantined cache entries. */
volatile uint64_t native_adreno_backing_cache_stats[12];
}

namespace {
enum MemoryStage : unsigned {
    MemoryAllocate,
    MemoryPrepare,
    MemorySpans,
    MemoryZero,
    MemoryComplete=8,
    MemoryDescriptorRelease
};

/* MemoryStageTime accumulates one operation's duration in the bounded stage
 * table. The caller supplies the number of bytes handled by that operation. */
class MemoryStageTime final {
    unsigned stage_;
    size_t bytes_;
    uint64_t began_;
public:
    MemoryStageTime(MemoryStage stage,size_t bytes):stage_(stage),bytes_(bytes),
        began_(native_adreno_vm_now_us?native_adreno_vm_now_us():0)
    {
    }

    ~MemoryStageTime()
    {
        const uint64_t ended=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
        const uint64_t elapsed=began_ && ended>=began_?ended-began_:0;
        volatile uint64_t *row=native_adreno_memory_stage_stats[stage_];
        row[0]=row[0]+1;
        row[1]=row[1]+elapsed;
        if(elapsed>row[2]) {
            row[2]=elapsed;
        }
        row[3]=row[3]+bytes_;
    }
};

enum : size_t {
    BackingCacheEntries=64,
    BackingCacheBytes=64u*1024u*1024u,
    BackingCacheObjectBytes=16u*1024u*1024u,
    BackingCacheLowPages=32768
};

/* CachedBacking owns storage after all user and GPU mappings are removed.
 * sequence orders successful insertions so eviction releases the oldest one. */
struct CachedBacking {
    NativeAdrenoBackingStorage storage;
    uint64_t sequence=0;
};

CachedBacking backing_cache[BackingCacheEntries];
size_t backing_cache_bytes=0,backing_cache_count=0;
uint64_t backing_cache_sequence=0;
NativeAdrenoMemoryDiagnostics memory_diagnostics;

enum BackingMissReason : unsigned {
    BackingMissEmpty,
    BackingMissSize,
    BackingMissAttributes,
    BackingMissQuarantined
};

enum BackingRejectReason : unsigned {
    BackingRejectNotOwned,
    BackingRejectNotPrepared,
    BackingRejectNotWriteCombined,
    BackingRejectQuarantined,
    BackingRejectNoSpans,
    BackingRejectZeroSize,
    BackingRejectOversized,
    BackingRejectNoEvictableEntry,
    BackingRejectNoSlot,
    BackingRejectImported,
    BackingRejectPooled,
    BackingRejectPressure,
    BackingRejectBusy
};

unsigned backingSizeBucket(size_t bytes)
{
    uint64_t upper=4096;
    for(unsigned index=0;index+1<NativeAdrenoMemorySizeBucketCount;++index) {
        if(bytes<=upper) {
            return index;
        }
        upper*=2;
    }
    return NativeAdrenoMemorySizeBucketCount-1;
}

void noteBackingRequest(size_t bytes,bool reuse)
{
    NativeAdrenoMemoryBucketStats &bucket=memory_diagnostics.buckets[backingSizeBucket(bytes)];
    ++bucket.requests;
    bucket.bytes+=bytes;
    if(reuse) {
        ++bucket.lookups;
    } else {
        ++bucket.bypassed;
    }
}

void noteBackingReject(BackingRejectReason reason)
{
    ++memory_diagnostics.insert_reject[reason];
    native_adreno_backing_cache_stats[4]=native_adreno_backing_cache_stats[4]+1;
}

/* MemorySlowTime retains the storage attributes needed after a descriptor is
 * released. Allocation and import refresh those attributes when they finish,
 * so their records include the physical spans found during the operation. */
class MemorySlowTime final {
    const NativeAdrenoBackingStorage *storage_;
    const bool *pooled_;
    NativeAdrenoMemorySlowEvent event_;
    uint64_t began_;
    bool refresh_;

    void captureStorage()
    {
        event_.spans=storage_->span_count;
        event_.source=storage_->diagnostic_source;
        event_.cache_result=storage_->diagnostic_cache_result;
        if(storage_->contiguous) {
            event_.flags|=NativeAdrenoMemoryDiagnosticContiguous;
        }
        if(storage_->write_combined) {
            event_.flags|=NativeAdrenoMemoryDiagnosticWriteCombined;
        }
        if(storage_->quarantined) {
            event_.flags|=NativeAdrenoMemoryDiagnosticQuarantined;
        }
        if(storage_->diagnostic_source==NativeAdrenoMemorySource::Imported) {
            event_.flags|=NativeAdrenoMemoryDiagnosticImported;
        }
        if(pooled_ && *pooled_) {
            event_.flags|=NativeAdrenoMemoryDiagnosticPooled;
        }
    }

public:
    MemorySlowTime(NativeAdrenoMemorySlowOperation operation,const NativeAdrenoBackingStorage &storage,
        size_t bytes,unsigned bits,uint32_t flags=0,bool refresh=false,const bool *pooled=nullptr):
        storage_(&storage),pooled_(pooled),began_(native_adreno_vm_now_us?native_adreno_vm_now_us():0),refresh_(refresh)
    {
        event_.operation=operation;
        event_.bytes=bytes;
        event_.output_bits=bits;
        event_.flags=flags;
        captureStorage();
    }

    ~MemorySlowTime()
    {
        const uint64_t ended=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
        if(!began_ || ended<began_ || ended-began_<8000) {
            return;
        }
        if(refresh_ && storage_->bytes) {
            captureStorage();
        }
        event_.sequence=++memory_diagnostics.slow_total;
        event_.sampled_us=ended;
        event_.elapsed_us=ended-began_;
        event_.cached_bytes=backing_cache_bytes;
        event_.cached_objects=static_cast<uint32_t>(backing_cache_count);
        event_.available_pages=memorystatus_available_pages;
        proc_selfname(event_.process,sizeof(event_.process));
        memory_diagnostics.slow[(event_.sequence-1)%NativeAdrenoMemorySlowEventCount]=event_;
        if(memory_diagnostics.slow_count<NativeAdrenoMemorySlowEventCount) {
            ++memory_diagnostics.slow_count;
        }
    }
};

bool backingPressure()
{
    return memorystatus_available_pages<BackingCacheLowPages;
}

void noteBackingCacheSize()
{
    native_adreno_backing_cache_stats[5]=backing_cache_count;
    native_adreno_backing_cache_stats[6]=backing_cache_bytes;
    if(backing_cache_bytes>native_adreno_backing_cache_stats[7]) {
        native_adreno_backing_cache_stats[7]=backing_cache_bytes;
    }
    memory_diagnostics.cached_objects=backing_cache_count;
    memory_diagnostics.cached_bytes=backing_cache_bytes;
    if(backing_cache_count>memory_diagnostics.peak_objects) {
        memory_diagnostics.peak_objects=backing_cache_count;
    }
    if(backing_cache_bytes>memory_diagnostics.peak_bytes) {
        memory_diagnostics.peak_bytes=backing_cache_bytes;
    }
}

IOReturn destroyOwnedStorage(NativeAdrenoBackingStorage &storage)
{
    if(storage.quarantined) {
        return kIOReturnBusy;
    }
    const uint32_t release_flags=storage.write_combined?NativeAdrenoMemoryDiagnosticWriteCombined:0;
    if(storage.descriptor) {
        if(storage.prepared) {
            MemoryStageTime elapsed(MemoryComplete,storage.bytes);
            native_adreno_trace_stage=31;
            const IOReturn status=storage.descriptor->complete();
            native_adreno_trace_stage=32;
            if(status) {
                storage.quarantined=true;
                return status;
            }
            storage.prepared=false;
        }
        {
            MemoryStageTime elapsed(MemoryDescriptorRelease,storage.bytes);
            MemorySlowTime slow(NativeAdrenoMemorySlowOperation::OwnedDescriptorRelease,storage,
                storage.bytes,storage.output_bits,release_flags);
            native_adreno_trace_stage=33;
            storage.descriptor->release();
            native_adreno_trace_stage=34;
        }
        storage.descriptor=nullptr;
    }
    if(storage.spans) {
        IODeleteData(storage.spans,NativeAdrenoPhysicalSpan,storage.span_capacity);
    }
    storage={};
    return kIOReturnSuccess;
}

IOReturn evictBacking(CachedBacking &entry,unsigned reason)
{
    const size_t bytes=entry.storage.bytes;
    const bool quarantined=entry.storage.quarantined;
    ++memory_diagnostics.eviction_attempts[reason];
    const IOReturn status=destroyOwnedStorage(entry.storage);
    if(status) {
        ++memory_diagnostics.eviction_failures[reason];
        if(!quarantined && entry.storage.quarantined) {
            native_adreno_backing_cache_stats[11]=native_adreno_backing_cache_stats[11]+1;
        }
        return status;
    }
    entry.sequence=0;
    backing_cache_bytes-=bytes;
    --backing_cache_count;
    ++memory_diagnostics.evictions[reason];
    memory_diagnostics.evicted_bytes[reason]+=bytes;
    native_adreno_backing_cache_stats[3]=native_adreno_backing_cache_stats[3]+1;
    noteBackingCacheSize();
    return kIOReturnSuccess;
}

bool takeBacking(size_t bytes,unsigned bits,bool contiguous,bool write_combined,NativeAdrenoBackingStorage &storage)
{
    NativeAdrenoMemoryBucketStats &bucket=memory_diagnostics.buckets[backingSizeBucket(bytes)];
    bool any=false,sized=false,quarantined_match=false;
    for(auto &entry:backing_cache) {
        const NativeAdrenoBackingStorage &candidate=entry.storage;
        if(!candidate.descriptor) {
            continue;
        }
        any=true;
        if(candidate.bytes!=bytes) {
            continue;
        }
        sized=true;
        if(candidate.output_bits!=bits || candidate.contiguous!=contiguous || candidate.write_combined!=write_combined) {
            continue;
        }
        if(candidate.quarantined) {
            quarantined_match=true;
            continue;
        }
        storage=candidate;
        storage.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::Hit;
        entry={};
        backing_cache_bytes-=bytes;
        --backing_cache_count;
        native_adreno_backing_cache_stats[0]=native_adreno_backing_cache_stats[0]+1;
        ++bucket.hits;
        noteBackingCacheSize();
        return true;
    }
    native_adreno_backing_cache_stats[1]=native_adreno_backing_cache_stats[1]+1;
    ++bucket.misses;
    const BackingMissReason reason=quarantined_match?BackingMissQuarantined:
        sized?BackingMissAttributes:any?BackingMissSize:BackingMissEmpty;
    ++bucket.miss_reason[reason];
    storage.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::Miss;
    return false;
}

bool cacheBacking(NativeAdrenoBackingStorage &storage)
{
    if(!storage.descriptor || !storage.prepared || storage.quarantined ||
        !storage.spans || !storage.span_count || !storage.bytes || storage.bytes>BackingCacheObjectBytes) {
        const BackingRejectReason reason=!storage.descriptor?BackingRejectNotOwned:
            !storage.prepared?BackingRejectNotPrepared:
            storage.quarantined?BackingRejectQuarantined:!storage.spans || !storage.span_count?BackingRejectNoSpans:
            !storage.bytes?BackingRejectZeroSize:BackingRejectOversized;
        noteBackingReject(reason);
        return false;
    }
    while(backing_cache_count==BackingCacheEntries || backing_cache_bytes>BackingCacheBytes-storage.bytes) {
        CachedBacking *oldest=nullptr;
        for(auto &entry:backing_cache) {
            if(entry.storage.descriptor && !entry.storage.quarantined &&
                (!oldest || entry.sequence<oldest->sequence)) {
                oldest=&entry;
            }
        }
        if(!oldest) {
            noteBackingReject(BackingRejectNoEvictableEntry);
            return false;
        }
        const unsigned reason=backing_cache_count==BackingCacheEntries?0:1;
        (void)evictBacking(*oldest,reason);
    }
    for(auto &entry:backing_cache) {
        if(entry.storage.descriptor) {
            continue;
        }
        entry.storage=storage;
        entry.sequence=++backing_cache_sequence;
        backing_cache_bytes+=storage.bytes;
        ++backing_cache_count;
        storage={};
        native_adreno_backing_cache_stats[2]=native_adreno_backing_cache_stats[2]+1;
        ++memory_diagnostics.insertions;
        noteBackingCacheSize();
        return true;
    }
    noteBackingReject(BackingRejectNoSlot);
    return false;
}
}

IOReturn NativeAdrenoMemory::trimBackingCache(NativeAdrenoBackingTrimReason reason)
{
    IOReturn result=kIOReturnSuccess;
    for(auto &entry:backing_cache) {
        if(!entry.storage.descriptor) {
            continue;
        }
        const IOReturn status=evictBacking(entry,static_cast<unsigned>(reason));
        if(status && !result) {
            result=status;
        }
    }
    return result;
}

void NativeAdrenoMemory::copyDiagnostics(NativeAdrenoMemoryDiagnostics *out)
{
    if(!out) {
        return;
    }
    *out=memory_diagnostics;
    uint64_t upper=4096;
    for(unsigned index=0;index+1<NativeAdrenoMemorySizeBucketCount;++index) {
        out->buckets[index].upper_bound_bytes=upper;
        upper*=2;
    }
    for(unsigned index=0;index<out->slow_count;++index) {
        const uint64_t retained=out->slow_total-out->slow_count+index;
        out->slow[index]=memory_diagnostics.slow[retained%NativeAdrenoMemorySlowEventCount];
    }
}

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_pool_mib=0,native_adreno_pool_status=0;volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* The pool (see reservePool): a free list of page extents over one contiguous block; best fit, neighbours merged on release. */
extern "C" {volatile uint32_t native_adreno_pool_stats[8];}
/* The board diagnostic ABI retains this inactive field. */
extern "C" {volatile uint32_t native_adreno_pool_wc;}
namespace {
struct PoolExtent {size_t offset,bytes;};
struct Pool {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *virt=nullptr;uint64_t physical=0;size_t bytes=0,used=0,peak=0;
    PoolExtent free[512];unsigned count=0;
} pool;
bool poolTake(size_t bytes,size_t *offset) {
    int best=-1;
    for(unsigned i=0;i<pool.count;++i)if(pool.free[i].bytes>=bytes && (best<0 || pool.free[i].bytes<pool.free[best].bytes))best=(int)i;
    if(best<0)return false;
    *offset=pool.free[best].offset;
    pool.free[best].offset+=bytes;pool.free[best].bytes-=bytes;
    if(!pool.free[best].bytes) {for(unsigned i=(unsigned)best;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];--pool.count;}
    pool.used+=bytes;if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    return true;
}
void poolGive(size_t offset,size_t bytes) {
    unsigned at=0;while(at<pool.count && pool.free[at].offset<offset)++at;
    if(pool.count>=512) {pool.used-=bytes;return;}   /* cannot happen with bounded fragmentation; the extent is lost, not reused */
    for(unsigned i=pool.count;i>at;--i)pool.free[i]=pool.free[i-1];
    pool.free[at]={offset,bytes};++pool.count;
    if(at+1<pool.count && pool.free[at].offset+pool.free[at].bytes==pool.free[at+1].offset) {
        pool.free[at].bytes+=pool.free[at+1].bytes;
        for(unsigned i=at+1;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];
        --pool.count;
    }
    if(at && pool.free[at-1].offset+pool.free[at-1].bytes==pool.free[at].offset) {
        pool.free[at-1].bytes+=pool.free[at].bytes;
        for(unsigned i=at;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];
        --pool.count;
    }
    pool.used-=bytes;native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);
}
}
size_t NativeAdrenoMemory::poolBytes() {return pool.bytes;}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(pool.descriptor)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits))return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor)return kIOReturnNoMemory;
    status=descriptor->prepare();
    if(status!=kIOReturnSuccess) {descriptor->release();return status;}
    IOByteCount length=0,tail=0;
    const addr64_t first=descriptor->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    const addr64_t last=descriptor->getPhysicalSegment(bytes-4096,&tail,kIOMemoryMapperNone);
    unsigned char *virt=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    if(!first || (first&4095) || !virt || ((uintptr_t)virt&4095) || last!=first+bytes-4096 || first>=limit || bytes>limit-first ||
        descriptor->getLength()!=bytes) {
        (void)descriptor->complete();descriptor->release();return kIOReturnUnsupported;
    }
    pool.descriptor=descriptor;pool.virt=virt;pool.physical=first;pool.bytes=bytes;pool.used=pool.peak=0;
    pool.free[0]={0,bytes};pool.count=1;
    native_adreno_pool_stats[0]=(uint32_t)(bytes>>20);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled,
    bool reuse_backing,bool write_combined)
{
    if(backed() || storage_.spans || storage_.quarantined) {
        return kIOReturnBusy;
    }
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget || budget>256u*1024u*1024u) {
        return kIOReturnBadArgument;
    }
    if(bytes>budget) {
        return kIOReturnNoResources;
    }
    const NativeAdrenoMemorySource source=pooled?NativeAdrenoMemorySource::UserOwned:NativeAdrenoMemorySource::KernelOwned;
    cpu_cache_mode_=write_combined?NativeAdrenoCpuCacheMode::NormalUncached:NativeAdrenoCpuCacheMode::Cached;
    storage_.diagnostic_source=source;
    storage_.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
    storage_.output_bits=bits;
    storage_.contiguous=contiguous;
    MemorySlowTime slow(NativeAdrenoMemorySlowOperation::Allocation,storage_,bytes,bits,
        reuse_backing?NativeAdrenoMemoryDiagnosticReuseRequested:0,true,&pooled_);

    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);
    if(status) {
        return status;
    }
    noteBackingRequest(bytes,reuse_backing);
    if(backing_cache_count && backingPressure()) {
        native_adreno_backing_cache_stats[8]=native_adreno_backing_cache_stats[8]+1;
        (void)trimBackingCache(NativeAdrenoBackingTrimReason::MemoryPressure);
    }

    if(reuse_backing && takeBacking(bytes,bits,contiguous,write_combined,storage_)) {
        storage_.diagnostic_source=source;
        {
            MemoryStageTime elapsed(MemoryZero,bytes);
            bzero(storage_.mapping,bytes);
            __asm__ volatile("dsb sy":::"memory");
        }
        return kIOReturnSuccess;
    }

    const uint64_t limit=UINT64_C(1)<<bits;
    if(pooled && !write_combined && pool.descriptor && bytes<=pool.bytes) {
        size_t offset=0;
        if(poolTake(bytes,&offset)) {
            const uint64_t physical=pool.physical+offset;
            if(physical>=limit || bytes>limit-physical) {
                poolGive(offset,bytes);
            } else {
                storage_.spans=IONewData(NativeAdrenoPhysicalSpan,1);
                if(!storage_.spans) {
                    poolGive(offset,bytes);
                    return kIOReturnNoMemory;
                }
                storage_.span_capacity=1;
                storage_.span_count=1;
                storage_.spans[0]={0,physical,bytes};
                storage_.mapping=pool.virt+offset;
                storage_.bytes=bytes;
                storage_.prepared=true;
                pooled_=true;
                pool_offset_=offset;
                {
                    MemoryStageTime elapsed(MemoryZero,bytes);
                    bzero(storage_.mapping,bytes);
                }
                native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
                return kIOReturnSuccess;
            }
        }
        native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;
    }

    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    options|=write_combined?kIOMapWriteCombineCache:kIOMapCopybackCache;
    if(contiguous) {
        options|=kIOMemoryPhysicallyContiguous;
    }
    {
        MemoryStageTime elapsed(MemoryAllocate,bytes);
        storage_.descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    }
    if(!storage_.descriptor && backing_cache_count) {
        native_adreno_backing_cache_stats[9]=native_adreno_backing_cache_stats[9]+1;
        (void)trimBackingCache(NativeAdrenoBackingTrimReason::AllocationRetry);
        MemoryStageTime elapsed(MemoryAllocate,bytes);
        storage_.descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    }
    if(!storage_.descriptor) {
        return kIOReturnNoMemory;
    }
    storage_.write_combined=write_combined;
    {
        MemoryStageTime elapsed(MemoryPrepare,bytes);
        status=storage_.descriptor->prepare();
    }
    if(status) {
        MemoryStageTime elapsed(MemoryDescriptorRelease,bytes);
        MemorySlowTime slowRelease(NativeAdrenoMemorySlowOperation::OwnedDescriptorRelease,storage_,bytes,bits);
        storage_.descriptor->release();
        storage_.descriptor=nullptr;
        return status;
    }
    storage_.prepared=true;
    storage_.bytes=bytes;
    storage_.output_bits=bits;
    storage_.contiguous=contiguous;
    storage_.mapping=storage_.descriptor->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!storage_.mapping || ((uintptr_t)storage_.mapping&4095) || bytes>UINTPTR_MAX-(uintptr_t)storage_.mapping ||
        storage_.descriptor->getLength()!=bytes) {
        goto failed;
    }
    storage_.span_capacity=bytes/4096;
    storage_.spans=IONewData(NativeAdrenoPhysicalSpan,storage_.span_capacity);
    if(!storage_.spans) {
        status=kIOReturnNoMemory;
        goto failed;
    }
    {
        MemoryStageTime elapsed(MemorySpans,bytes);
        for(size_t offset=0;offset<bytes;offset+=4096) {
            IOByteCount length=0;
            const addr64_t physical=storage_.descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical) {
                status=kIOReturnUnsupported;
                goto failed;
            }
            if(contiguous && offset && physical!=storage_.spans[0].physical+offset) {
                status=kIOReturnUnsupported;
                goto failed;
            }
            if(storage_.span_count &&
                physical==storage_.spans[storage_.span_count-1].physical+storage_.spans[storage_.span_count-1].bytes) {
                storage_.spans[storage_.span_count-1].bytes+=4096;
            } else {
                storage_.spans[storage_.span_count++]={offset,physical,4096,write_combined?0u:1u};
            }
        }
    }
    {
        MemoryStageTime elapsed(MemoryZero,bytes);
        bzero(storage_.mapping,bytes);
    }
    native_adreno_pool_stats[5]=native_adreno_pool_stats[5]+1;
    native_adreno_pool_stats[6]=native_adreno_pool_stats[6]+(uint32_t)storage_.span_count;
    return kIOReturnSuccess;

failed:
    /* l744: same forced teardown as the import path - release() may leak. */
    if(imported_) {
        if(storage_.prepared) { (void)imported_->complete(); storage_.prepared=false; }
        imported_->release();
        imported_=nullptr;
        cpu_cache_mode_=NativeAdrenoCpuCacheMode::Unknown;
    }
    if(pooled_) {
        poolGive(pool_offset_,storage_.bytes);
        pooled_=false;
        storage_.prepared=false;
        pool_offset_=0;
    }
    if(storage_.descriptor) {
        if(storage_.prepared) { (void)storage_.descriptor->complete(); storage_.prepared=false; }
        storage_.descriptor->release();
        storage_.descriptor=nullptr;
    }
    if(storage_.spans) {
        IODeleteData(storage_.spans,NativeAdrenoPhysicalSpan,storage_.span_capacity);
        storage_.spans=nullptr;
    }
    storage_={};
    read_only_=false;
    return status;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
/* Only the complete current GOP allocation is a firmware import. Its CPU
 * mapping stays Device/uncached; do not reclassify arbitrary non-DRAM pages or
 * change their pmap attributes. The importing descriptor remains prepared and
 * owned by importTask until the normal GPU-unmap/release path completes. */
static uint64_t firmwareImportPhysical(IOMemoryDescriptor *memory,size_t bytes,unsigned bits,bool read_only)
{
    /* l747: the firmware route stays opt-in (nabu-gpu-scanout=1). It gives
     * the GOP a FirmwareUncached span - fine for the copy/scanout path but
     * it cannot back CA render-target textures (l746: the import succeeded
     * and tex stayed 0). The generic path now classifies the same pages
     * Cached (see NativeAdrenoQueryPageCacheMode), the class final's
     * in-window imports used and the only one proven to render. */
    int scanout=0;
    PE_parse_boot_argn("nabu-gpu-scanout",&scanout,sizeof(scanout));
    PE_Video video{};
    if(scanout!=1 || read_only || PE_current_console(&video) || video.v_depth!=32 || video.v_rotate ||
        !video.v_width || !video.v_height || video.v_width>16384 || video.v_height>16384 ||
        video.v_rowBytes>UINT64_MAX/video.v_height) {
        return 0;
    }
    const uint64_t extent=uint64_t(video.v_rowBytes)*video.v_height;
    if(extent!=bytes || memory->getLength()!=extent ||
        !nabu_gpu_copy_rows(static_cast<uint32_t>(video.v_width),static_cast<uint32_t>(video.v_height),
            video.v_rowBytes,extent) ||
        !nabu_gpu_firmware_range(video.v_baseAddr,extent,gPhysBase,gPhysSize,bits)) {
        return 0;
    }
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        const addr64_t physical=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(physical!=video.v_baseAddr+offset || length<4096) {
            return 0;
        }
    }
    return video.v_baseAddr;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(backed() || storage_.spans || storage_.quarantined)return kIOReturnBusy;
    native_adreno_import_stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget || budget>256u*1024u*1024u)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    storage_.diagnostic_source=NativeAdrenoMemorySource::Imported;
    storage_.diagnostic_cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
    storage_.output_bits=bits;
    MemorySlowTime slow(NativeAdrenoMemorySlowOperation::Import,storage_,bytes,bits,
        NativeAdrenoMemoryDiagnosticImported,true);
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    uint64_t firmware_physical=0;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_)return kIOReturnNoMemory;
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    storage_.prepared=true;
    storage_.bytes=bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    firmware_physical=firmwareImportPhysical(imported_,bytes,bits,read_only_);
    if(firmware_physical) {
        storage_.span_capacity=1;
        storage_.spans=IONewData(NativeAdrenoPhysicalSpan,1);
        if(!storage_.spans) {
            status=kIOReturnNoMemory;
            goto failed;
        }
        storage_.spans[0]={0,firmware_physical,bytes,0};
        storage_.span_count=1;
        cpu_cache_mode_=NativeAdrenoCpuCacheMode::FirmwareUncached;
    } else {
        /* Count the merged spans first: importing a large framebuffer used to
         * reserve one slot per 4 KiB page, which needs 128000 B for the GOP and
         * fails in KHEAP_DATA_BUFFERS. Validate every page without storing it,
         * then allocate exactly the number of runs the descriptor describes. */
        size_t needed=0;
        uint64_t last_physical=0;
        uint64_t last_bytes=0;
        unsigned last_writeback=0;
        for(size_t offset=0;offset<bytes;offset+=4096) {
            IOByteCount length=0;
            addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
                {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
                 status=kIOReturnUnsupported;goto failed;}
            const NativeAdrenoCpuCacheMode mode=NativeAdrenoQueryPageCacheMode(physical);
            if(mode==NativeAdrenoCpuCacheMode::Unknown) {
                native_adreno_import_fail_pa=physical;
                native_adreno_import_fail_offset=offset;
                status=kIOReturnUnsupported;
                goto failed;
            }
            const unsigned writeback=mode==NativeAdrenoCpuCacheMode::Cached?1u:0u;
            if(needed && writeback==last_writeback && physical==last_physical+last_bytes) {
                last_bytes+=4096;
            } else {
                ++needed;
                last_physical=physical;
                last_bytes=4096;
                last_writeback=writeback;
            }
        }
        storage_.span_capacity=needed?needed:1;
        storage_.spans=IONewData(NativeAdrenoPhysicalSpan,storage_.span_capacity);
        if(!storage_.spans) {
            status=kIOReturnNoMemory;
            goto failed;
        }
        last_physical=0;
        last_bytes=0;
        last_writeback=0;
        for(size_t offset=0;offset<bytes;offset+=4096) {
            IOByteCount length=0;
            addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            const NativeAdrenoCpuCacheMode mode=NativeAdrenoQueryPageCacheMode(physical);
            if(!offset) {
                cpu_cache_mode_=mode;
            } else if(cpu_cache_mode_!=mode) {
                cpu_cache_mode_=NativeAdrenoCpuCacheMode::Mixed;
            }
            const unsigned writeback=mode==NativeAdrenoCpuCacheMode::Cached?1u:0u;
            if(storage_.span_count && storage_.spans[storage_.span_count-1].writeback==writeback &&
                physical==storage_.spans[storage_.span_count-1].physical+storage_.spans[storage_.span_count-1].bytes) {
                storage_.spans[storage_.span_count-1].bytes+=4096;
            } else {
                storage_.spans[storage_.span_count++]={offset,physical,4096,writeback};
            }
        }
    }
    native_adreno_import_stage=19;
    native_adreno_pool_stats[7]=native_adreno_pool_stats[7]+(uint32_t)storage_.span_count;
    return kIOReturnSuccess;
failed:
    /* l744: a failed allocation/import owns nothing worth keeping, but
     * release()/destroyOwnedStorage() may early-return (Busy) or quarantine
     * on a complete() failure and keep the prepared descriptor and spans -
     * leaking import budget until later imports all fail (the bridge then
     * reports M18 and creates zero commands). Tear down unconditionally. */
    if(imported_) {
        if(storage_.prepared) { (void)imported_->complete(); storage_.prepared=false; }
        imported_->release();
        imported_=nullptr;
        cpu_cache_mode_=NativeAdrenoCpuCacheMode::Unknown;
    }
    if(pooled_) {
        poolGive(pool_offset_,storage_.bytes);
        pooled_=false;
        storage_.prepared=false;
        pool_offset_=0;
    }
    if(storage_.descriptor) {
        if(storage_.prepared) { (void)storage_.descriptor->complete(); storage_.prepared=false; }
        storage_.descriptor->release();
        storage_.descriptor=nullptr;
    }
    if(storage_.spans) {
        IODeleteData(storage_.spans,NativeAdrenoPhysicalSpan,storage_.span_capacity);
        storage_.spans=nullptr;
    }
    storage_={};
    read_only_=false;
    return status;
}
IOReturn NativeAdrenoMemory::release(bool cache_backing)
{
    native_adreno_trace_stage=30;
    if(cache_backing) {
        ++memory_diagnostics.insert_attempts;
    }
    if(device_references_ || storage_.quarantined) {
        if(cache_backing) {
            noteBackingReject(storage_.quarantined?BackingRejectQuarantined:BackingRejectBusy);
        }
        return kIOReturnBusy;
    }
    const bool pressure=backingPressure();
    if(pressure && backing_cache_count) {
        native_adreno_backing_cache_stats[8]=native_adreno_backing_cache_stats[8]+1;
        (void)trimBackingCache(NativeAdrenoBackingTrimReason::MemoryPressure);
    }
    if(cache_backing) {
        if(pressure) {
            noteBackingReject(BackingRejectPressure);
        } else if(imported_) {
            noteBackingReject(BackingRejectImported);
        } else if(pooled_) {
            noteBackingReject(BackingRejectPooled);
        } else if(cacheBacking(storage_)) {
            read_only_=false;
            return kIOReturnSuccess;
        }
    }
    if(imported_) {
        if(storage_.prepared) {
            MemoryStageTime elapsed(MemoryComplete,storage_.bytes);
            const IOReturn status=imported_->complete();
            if(status) {
                storage_.quarantined=true;
                return status;
            }
            storage_.prepared=false;
        }
        {
            MemoryStageTime elapsed(MemoryDescriptorRelease,storage_.bytes);
            MemorySlowTime slow(NativeAdrenoMemorySlowOperation::ImportedDescriptorRelease,storage_,
                storage_.bytes,storage_.output_bits);
            imported_->release();
        }
        imported_=nullptr;
        cpu_cache_mode_=NativeAdrenoCpuCacheMode::Unknown;
    }
    if(pooled_) {
        poolGive(pool_offset_,storage_.bytes);
        pooled_=false;
        storage_.prepared=false;
        pool_offset_=0;
    }
    const IOReturn status=destroyOwnedStorage(storage_);
    if(!status) {
        read_only_=false;
    }
    return status;
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !storage_.prepared || !storage_.span_count || storage_.quarantined)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(storage_.quarantined)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes)
{
    if(!backed() || !storage_.prepared || storage_.quarantined)return kIOReturnNotReady;
    if(!bytes || offset>storage_.bytes || bytes>storage_.bytes-offset)return kIOReturnBadArgument;
    __asm__ volatile("dsb st" ::: "memory");
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes)
{
    if(!backed() || !storage_.prepared || storage_.quarantined)return kIOReturnNotReady;
    if(!bytes || offset>storage_.bytes || bytes>storage_.bytes-offset)return kIOReturnBadArgument;
    __asm__ volatile("dsb sy" ::: "memory");
    return kIOReturnSuccess;
}
