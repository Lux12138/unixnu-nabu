/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_USERCLIENT_H
#define NATIVE_ADRENO_USERCLIENT_H
#include "NativeAdrenoABI.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoStatus.h"

/* NativeAdrenoUserClient owns a bounded table of BOs and the queues associated
 * with one connection. A handle stores a slot and its generation so a freed
 * handle cannot identify the next BO allocated in that slot. Allocation takes
 * an entry from a free list; lookup and live-object accounting are constant time.
 *
 * The IOKit wrapper removes CPU views before surrendering their final lease.
 * freeBo retires completed submissions, removes the VM mapping and releases
 * the backing storage in that order. Storage may enter the shared cache only
 * after these steps and all device references have retired. */
enum : size_t {NativeAdrenoClientMaxObjects=4096};
class NativeAdrenoUserClient final {
    struct Queue {uint32_t id=0;bool active=false;};
    struct Entry {
        NativeAdrenoMemory memory;
        uint64_t handle=0;
        uint64_t generation=0;
        uint32_t next_free=NativeAdrenoClientMaxObjects;
        uint64_t mapped=0;
        uint64_t iova=0;
        bool used=false;
        bool mapped_state=false;
        bool unreleasable=false;
        bool imported=false;
        uint32_t metadata_bytes=0;
        uint32_t token=0;
        uint32_t mapping_leases=0;
        uint32_t controlled_mapping_leases=0;
        char metadata[NativeAdrenoAbiMetadataBytes]{};
    };
    /* open() allocates the entries and links them in ascending slot order. */
    Entry *entries_=nullptr;
    size_t entry_count_=0;
    size_t live_count_=0;
    size_t mapped_count_=0;
    NativeAdrenoSubmitResource *submit_resources_=nullptr;
    NativeAdrenoSubmitBuffer *submit_buffers_=nullptr;
    NativeAdrenoAbiResourceBudget map_failure_{};
    uint32_t free_head_=NativeAdrenoClientMaxObjects;
    Queue queues_[NativeAdrenoAbiMaxQueues]{};
    NativeAdrenoVm *vm_=nullptr;
    NativeAdrenoSubmit *submit_=nullptr;
    NativeAdrenoRing *ring_=nullptr;
    const NativeAdrenoTimer *timer_=nullptr;
    const NativeAdrenoStatus *status_=nullptr;
    const NativeAdrenoAbiDeviceInfo *info_=nullptr;
    uint32_t input_bits_=0,output_bits_=0;
    /* Scope on the shared ring and GPU space: this client's queues are the submit object's queues [queue_base_, +MaxQueues), and with a
     * window (window_bytes_ nonzero) every mapping must lie inside [window_start_, +window_bytes_), which GetDeviceInfo reports as the VA. */
    uint32_t queue_base_=0;
    /* A client owns a queue range once setScope() gave it one or it has submitted into the default one (a single client that never scopes owns
     * range 0). Before either, queue_base_ is only the default 0, which a scoped client may own, so a check of "this client's own range" would
     * test someone else's jobs. */
    bool scoped_=false,used_=false;
    uint64_t window_start_=0,window_bytes_=0,window_peak_=0;
    bool open_=false;
    Entry *find(uint64_t);
    const Entry *find(uint64_t) const;
    Entry *takeEntry();
    void returnEntry(Entry &);
public:
    NativeAdrenoUserClient()=default;
    ~NativeAdrenoUserClient()=default;
    NativeAdrenoUserClient(const NativeAdrenoUserClient &)=delete;
    NativeAdrenoUserClient &operator=(const NativeAdrenoUserClient &)=delete;
    /* Mounts one already-qualified context. input/output bits describe the
     * active native4K arena and are reported, never guessed. */
    IOReturn open(uint32_t abi_version,NativeAdrenoVm *,NativeAdrenoSubmit *,
        NativeAdrenoRing *,uint32_t input_bits,uint32_t output_bits);
    /* One client of several on the same VM, submit object and ring: its queue range (queue_base a multiple of NativeAdrenoAbiMaxQueues) and
     * GPU address window (4 KiB aligned, inside the input range; bytes 0 = unrestricted). Only while open with no live object or job. */
    IOReturn setScope(uint32_t queue_base,uint64_t window_start,uint64_t window_bytes);
    /* Highest end address (exclusive) any mapping of this client has reached, 0 before the first. */
    uint64_t windowPeak() const {return window_peak_;}
    /* Whether any job is unobserved in the range this client owns (never, while it owns none). */
    bool ownRangeOutstanding() const;
    void beginClose();
    /* Refuses while any object is live or any of this client's jobs is unobserved. */
    IOReturn close();
    /* Connection teardown requires the IOKit wrapper to remove every CPU view
     * first. It waits for outstanding jobs, removes each GPU mapping, releases
     * its storage, closes the queues and drains the backing cache. A failure
     * retains every object whose teardown has not completed. */
    IOReturn releaseAll(const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Backing object of a token returned by cpuMap while its lease is held (clientMemoryForType); null otherwise. */
    NativeAdrenoMemory *memoryForToken(uint32_t token);
    /* CPU mappings of the BO of this token that BoCpuMap handed out and BoCpuUnmap has not given back; 0 for no such BO. */
    uint32_t mappingLeases(uint32_t token) const;
    /* The IOKit wrapper checks this before removing views named by a token. */
    bool hasMappingLease(uint64_t handle,uint32_t token) const;
    IOReturn query(NativeAdrenoAbiParams *);
    IOReturn allocBo(const NativeAdrenoAbiBoAlloc *,NativeAdrenoAbiBoAllocReply *);
    IOReturn allocBoInner(const NativeAdrenoAbiBoAlloc *,NativeAdrenoAbiBoAllocReply *);
    IOReturn mapBo(const NativeAdrenoAbiBoMap *,NativeAdrenoAbiBoMapReply *);
    /* BoImport: pins the caller's pages. Only the IOKit wrapper can call it, with the calling task; dispatch() refuses the method
     * because it has no task (the kernel mailbox path cannot import). */
    IOReturn importBo(const NativeAdrenoAbiBoImport *,void *task,NativeAdrenoAbiBoAllocReply *);
    /* Unmaps if still mapped, then releases the backing object. A failure at
     * either step keeps the entry live and unmapped where possible. */
    IOReturn freeBo(const NativeAdrenoAbiBoFree *);
    IOReturn submit(const NativeAdrenoAbiSubmit *,NativeAdrenoAbiSubmitReply *,
        const NativeAdrenoRingClock *);
    IOReturn submitV2(const void *,size_t,NativeAdrenoAbiSubmitV2Reply *,
        const NativeAdrenoAbiSubmitAsync *async=nullptr);
    IOReturn submitAsync(const void *,size_t,NativeAdrenoAbiAsyncReply *);
    IOReturn submitCaps(NativeAdrenoAbiSubmitCaps *);
    IOReturn resourceBudget(NativeAdrenoAbiResourceBudget *) const;
    IOReturn cpuMap(const NativeAdrenoAbiBoCpuMap *,NativeAdrenoAbiBoCpuMapReply *,bool controlled=false);
    IOReturn cpuUnmap(const NativeAdrenoAbiBoCpuUnmap *,bool controlled=false);
    IOReturn createQueue(NativeAdrenoAbiQueueReply *);
    IOReturn closeQueue(const NativeAdrenoAbiQueue *);
    IOReturn setMetadata(const NativeAdrenoAbiMetadata *);
    IOReturn getMetadata(const NativeAdrenoAbiMetadata *,NativeAdrenoAbiMetadata *);
    IOReturn waitFence(const NativeAdrenoAbiWaitFence *,const NativeAdrenoRingClock *);
    /* One external-method entry: exact record sizes are required, unknown
     * methods are unsupported and no output byte is modified on failure. The
     * IOKit wrapper should only copy these buffers and call this function. */
    IOReturn dispatch(uint32_t method,const void *input,size_t input_size,
        void *output,size_t output_size,const NativeAdrenoRingClock *);
    static size_t inputSize(uint32_t method);
    static size_t outputSize(uint32_t method);
    size_t liveObjects() const;
    /* Optional GPU timestamp source; absent means the method stays unsupported. */
    IOReturn attachTimer(const NativeAdrenoTimer *);
    IOReturn timestamp(NativeAdrenoAbiTimestamp *);
    /* Optional counters; absent means the status method stays unsupported. */
    IOReturn attachStatus(const NativeAdrenoStatus *);
    IOReturn status(NativeAdrenoAbiStatus *);
    /* Optional constant device facts; absent means the method stays unsupported. */
    IOReturn attachDeviceInfo(const NativeAdrenoAbiDeviceInfo *);
    IOReturn deviceInfo(NativeAdrenoAbiDeviceInfo *);
    bool mapped(uint64_t handle);
    /* Backing object of a live handle, for the IOKit wrapper's clientMemoryForType and for kernel self-tests;
     * null for an unknown or unreleasable handle. The client keeps ownership. */
    NativeAdrenoMemory *memoryOf(uint64_t handle);
};
#endif
