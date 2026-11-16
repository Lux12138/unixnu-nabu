/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_MEMORY_H
#define NATIVE_ADRENO_MEMORY_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "NabuGpuMemoryPolicy.h"
class IOBufferMemoryDescriptor;
class IOMemoryDescriptor;
struct NativeAdrenoPhysicalSpan {
    uint64_t offset,physical,bytes;
    unsigned writeback=1; // CPU cached pages use coherent GPU WB; Normal NC pages use GPU NC.
};
struct NativeAdrenoCpuState {
    uint64_t tcr,ttbr0,ttbr1,sctlr;
    uint64_t kernel_page_bytes,kernel_map_page_bytes;
};
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *);
enum class NativeAdrenoCpuCacheMode : unsigned {
    Unknown,
    Cached,
    NormalUncached,
    FirmwareUncached,
    Mixed
};
/* The pinned page supplies its actual pmap cache attributes. Import records
 * adjacent pages with the same GPU cache policy in one physical span. */
NativeAdrenoCpuCacheMode NativeAdrenoQueryPageCacheMode(uint64_t physical);

enum : unsigned {
    NativeAdrenoMemorySizeBucketCount=18,
    NativeAdrenoMemorySlowEventCount=64,
    NativeAdrenoBackingMissReasonCount=4,
    NativeAdrenoBackingEvictReasonCount=5,
    NativeAdrenoBackingRejectReasonCount=13
};

enum class NativeAdrenoBackingTrimReason : uint32_t {
    ClientClose=2,
    MemoryPressure=3,
    AllocationRetry=4
};

enum class NativeAdrenoMemorySource : uint32_t {
    Unknown,
    KernelOwned,
    UserOwned,
    Imported
};

enum class NativeAdrenoMemoryCacheResult : uint32_t {
    NotRequested,
    Hit,
    Miss
};

enum class NativeAdrenoMemorySlowOperation : uint32_t {
    Allocation,
    OwnedDescriptorRelease,
    ImportedDescriptorRelease,
    Import
};

enum NativeAdrenoMemoryDiagnosticFlags : uint32_t {
    NativeAdrenoMemoryDiagnosticContiguous=1u<<0,
    NativeAdrenoMemoryDiagnosticWriteCombined=1u<<1,
    NativeAdrenoMemoryDiagnosticImported=1u<<2,
    NativeAdrenoMemoryDiagnosticPooled=1u<<3,
    NativeAdrenoMemoryDiagnosticQuarantined=1u<<4,
    NativeAdrenoMemoryDiagnosticReuseRequested=1u<<5
};

/* NativeAdrenoMemoryBucketStats counts accepted allocation requests whose CPU
 * page configuration was available. A lookup has one hit or one miss reason;
 * bypassed requests did not ask to reuse backing storage. */
struct NativeAdrenoMemoryBucketStats {
    uint64_t upper_bound_bytes=0; // Zero denotes sizes above the preceding bucket.
    uint64_t requests=0,bytes=0,lookups=0,hits=0,misses=0,bypassed=0;
    uint64_t miss_reason[NativeAdrenoBackingMissReasonCount]{}; // Empty, size, attributes, quarantined.
};

/* NativeAdrenoMemorySlowEvent describes one completed operation of at least
 * 8000 us. Storage metadata identifies its owner and allocation attributes;
 * the cache and available-page values are sampled when the operation ends. */
struct NativeAdrenoMemorySlowEvent {
    uint64_t sequence=0,sampled_us=0,elapsed_us=0,bytes=0,spans=0;
    uint64_t cached_bytes=0,available_pages=0;
    uint32_t cached_objects=0,output_bits=0,flags=0;
    NativeAdrenoMemorySlowOperation operation=NativeAdrenoMemorySlowOperation::Allocation;
    NativeAdrenoMemorySource source=NativeAdrenoMemorySource::Unknown;
    NativeAdrenoMemoryCacheResult cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
    char process[20]{};
};

/* NativeAdrenoMemoryDiagnostics is a fixed-size copy of backing-cache counters
 * and slow operations. The resource gate serializes producers and the copy;
 * the published service owns the copied value while printing outside its lock. */
struct NativeAdrenoMemoryDiagnostics {
    NativeAdrenoMemoryBucketStats buckets[NativeAdrenoMemorySizeBucketCount]{};
    uint64_t insert_attempts=0,insertions=0;
    // Not owned, not prepared, not WC, quarantine, no spans, zero size,
    // oversized, no evictable entry, no slot, imported, pooled, pressure, busy.
    uint64_t insert_reject[NativeAdrenoBackingRejectReasonCount]{};
    // Object limit, byte limit, client close, memory pressure, allocation retry.
    uint64_t eviction_attempts[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t eviction_failures[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t evictions[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t evicted_bytes[NativeAdrenoBackingEvictReasonCount]{};
    uint64_t cached_objects=0,cached_bytes=0,peak_objects=0,peak_bytes=0;
    uint64_t slow_total=0;
    uint32_t slow_count=0;
    NativeAdrenoMemorySlowEvent slow[NativeAdrenoMemorySlowEventCount]{}; // Oldest first in a copied result.
};

/* NativeAdrenoBackingStorage owns a prepared allocation and the physical spans
 * discovered from its descriptor. The reusable-storage cache keeps these fields
 * after a user BO is unmapped, then transfers them to the next matching BO.
 * Imported pages use the same span metadata while NativeAdrenoMemory owns the
 * importing descriptor and its task lifetime. */
struct NativeAdrenoBackingStorage {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    void *mapping=nullptr;
    NativeAdrenoPhysicalSpan *spans=nullptr;
    size_t bytes=0,span_count=0,span_capacity=0;
    unsigned output_bits=0;
    bool contiguous=false,prepared=false,quarantined=false,write_combined=false;
    NativeAdrenoMemorySource diagnostic_source=NativeAdrenoMemorySource::Unknown;
    NativeAdrenoMemoryCacheResult diagnostic_cache_result=NativeAdrenoMemoryCacheResult::NotRequested;
};

/* NativeAdrenoMemory owns one BO's backing storage and counts references held
 * by GPU mappings and jobs. Calls run under the queue/resource gate. The VM
 * retires each reference after DMA completion and native TLB unmap, and the
 * user client removes every CPU view before offering storage for reuse.
 * release() keeps failed teardown state owned so the caller can retain it. */
class NativeAdrenoMemory final {
    NativeAdrenoBackingStorage storage_;
    IOMemoryDescriptor *imported_=nullptr;
    bool read_only_=false;
    NativeAdrenoCpuCacheMode cpu_cache_mode_=NativeAdrenoCpuCacheMode::Unknown;
    uint32_t device_references_=0;
    bool pooled_=false;
    size_t pool_offset_=0;
public:
    NativeAdrenoMemory()=default;
    ~NativeAdrenoMemory()=default;
    NativeAdrenoMemory(const NativeAdrenoMemory &)=delete;
    NativeAdrenoMemory &operator=(const NativeAdrenoMemory &)=delete;
    /* Explicit driver budget; bounded to 256MiB per backing object for now.
     * Error may retain quarantined allocation state if complete() fails.
     * Caller always keeps this object and checks quarantined(), even on error.
     * CPU native4K state and kernel page ABI are checked before allocation.
     * prepare(), raw unmapped physical segments, span/PA bounds and optional
     * physical contiguity are checked before zeroing the allocation. */
    /* The user-owned request keeps its ABI but selects the board's cache mode.
     * The seven-argument allocator still honors explicit firmware/cache modes. */
    IOReturn allocate(size_t bytes,unsigned output_bits,bool contiguous,size_t byte_budget,bool pooled=false,
        bool reuse_backing=false) {
        return allocate(bytes,output_bits,contiguous,byte_budget,pooled,reuse_backing,
            NabuGpuOwnedBoWriteCombined(pooled));
    }
    IOReturn allocate(size_t bytes,unsigned output_bits,bool contiguous,size_t byte_budget,bool pooled,
        bool reuse_backing,bool write_combined);
    /* The cache owns at most 64 allocations and 64 MiB. Matching allocations
     * retain their prepared descriptors, spans and CPU cache attributes;
     * allocate() clears every byte before reuse.
     * Allocation failure and low available memory drain it. Client close also
     * drains it; failed teardown stays owned and quarantined in the cache. */
    static IOReturn trimBackingCache(NativeAdrenoBackingTrimReason reason=NativeAdrenoBackingTrimReason::ClientClose);
    /* The caller holds the same queue/resource gate as allocate and release.
     * The result contains the retained slow events in increasing sequence order. */
    static void copyDiagnostics(NativeAdrenoMemoryDiagnostics *out);
    /* One physically contiguous block taken from the VM early (while memory is not yet fragmented) and cut into BOs afterwards: a BO from it is
     * one span of the GPU's table instead of one per scattered page (int-137: 59474 spans for 332 MiB, the 65536 span capacity ran out,
     * BoMap failed, the translator answered out of device memory and the compositor fell back to the CPU; each map also walked every span, 167 ms).
     * allocate(..., pooled=true) uses the pool when one is reserved and has room, else the scattered path. Only the board calls this. */
    static IOReturn reservePool(unsigned output_bits,size_t bytes);
    static size_t poolBytes();
    /* importTask pins the task's pages, records their existing CPU cache mode,
     * and checks spans and PA bounds as for allocate. The owner preserves that
     * allocation cache mode until GPU unmap; prepare() pins pages without
     * preventing a separate CPU request from changing their cache attributes. */
    IOReturn importTask(void *task,uint64_t address,size_t bytes,unsigned output_bits,size_t byte_budget);
    /* Board-only: the exact firmware GOP allocation outside the XNU-owned
     * memory window. CPU Device accesses and GPU Normal NC accesses carry no
     * cached aliases. This does not enable cached RAM/IOSurface imports. */
    IOReturn importFirmwareScanout(IOMemoryDescriptor *,unsigned output_bits);
    bool imported() const {return imported_!=nullptr;}
    NativeAdrenoCpuCacheMode cpuCacheMode() const {return cpu_cache_mode_;}
    bool cpuCacheCoherent() const {return cpu_cache_mode_==NativeAdrenoCpuCacheMode::Cached;}
    /* An import of memory the owner maps read-only (the kernel could pin it for reading only): the GPU may read it, never write it. */
    bool readOnly() const {return read_only_;}
    bool writeCombined() const {return storage_.write_combined;}
    bool backed() const {return storage_.descriptor || imported_ || pooled_;}
    /* cache_backing requires the caller to have removed every CPU mapping.
     * A prepared owned allocation can then enter the cache if
     * device references are zero. Imports and quarantined storage cannot enter. */
    IOReturn release(bool cache_backing=false);
    IOReturn holdForDevice();
    IOReturn retireAfterQuiescence();
    void quarantine() {storage_.quarantined=true;}
    /* The device's coherent WB or Normal NC mapping observes CPU stores after
     * publication. The caller excludes conflicting accesses until its fence. */
    IOReturn publishForDevice(size_t offset,size_t bytes);
    /* The caller observes the device fence before ordering subsequent CPU reads. */
    IOReturn acquireDeviceWrites(size_t offset,size_t bytes);
    bool quarantined() const {return storage_.quarantined;}
    uint32_t deviceReferences() const {return device_references_;}
    size_t bytes() const {return storage_.bytes;}
    size_t spanCount() const {return storage_.span_count;}
    const NativeAdrenoPhysicalSpan *spans() const {return storage_.spans;}
    void *mapping() const {return storage_.mapping;}
    /* Borrowed descriptor, never release independently or expose table pools
     * through clientMemoryForType. Sharing requires a separate owner/task ABI. */
    IOBufferMemoryDescriptor *descriptor() const {return storage_.descriptor;}
};
#endif
