/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_ABI_H
#define NATIVE_ADRENO_ABI_H
#define NATIVE_ADRENO_ASYNC_ABI 1
#include <stdint.h>

/* NativeAdreno wire records carry BO, mapping and submission requests through
 * IOKit external methods. Fixed records have exact lengths; SubmitV2 carries
 * bounded arrays at checked offsets in one copied packet. Both participants
 * use ARM64 little-endian fields. Unknown versions, methods and flags fail. */
enum {
    NativeAdrenoAbiVersion = 1,
    NativeAdrenoAbiMaxBuffers = 16,
    NativeAdrenoAbiMaxBoBytesShift = 28, /* 256MiB per object */
    NativeAdrenoMethodQuery = 1,
    NativeAdrenoMethodBoAlloc = 2,
    NativeAdrenoMethodBoMap = 3,
    NativeAdrenoMethodBoFree = 4,
    NativeAdrenoMethodSubmit = 5,
    NativeAdrenoMethodWaitFence = 6,
    NativeAdrenoMethodGetTimestamp = 7,
    NativeAdrenoMethodGetStatus = 8,
    NativeAdrenoMethodSetMetadata = 9,
    NativeAdrenoMethodGetMetadata = 10,
    NativeAdrenoMethodCreateQueue = 11,
    NativeAdrenoMethodCloseQueue = 12,
    NativeAdrenoMethodBoCpuMap = 13,
    NativeAdrenoMethodBoCpuUnmap = 14,
    NativeAdrenoMethodGetDeviceInfo = 15,
    NativeAdrenoMethodBoImport = 16,
    NativeAdrenoMethodReportBoProfile = 17,
    NativeAdrenoMethodBoCpuMapV2 = 18,
    NativeAdrenoMethodBoCpuUnmapV2 = 19,
    NativeAdrenoMethodQuerySubmitV2 = 20,
    NativeAdrenoMethodSubmitV2 = 21,
    NativeAdrenoMethodGetResourceBudget = 22,
    NativeAdrenoMethodQueryAsync = 23,
    NativeAdrenoMethodSyncCreate = 24,
    NativeAdrenoMethodSyncRelease = 25,
    NativeAdrenoMethodSyncSignal = 26,
    NativeAdrenoMethodSyncQuery = 27,
    NativeAdrenoMethodSyncWait = 28,
    NativeAdrenoMethodSubmitAsync = 29,
    NativeAdrenoMethodWaitCompletion = 30,
    NativeAdrenoMethodWaitCapacity = 31,
    NativeAdrenoMethodWaitBoIdle = 32,
    NativeAdrenoMethodLast = NativeAdrenoMethodWaitBoIdle,
    NativeAdrenoAbiMetadataBytes = 64,
    NativeAdrenoAbiMaxQueues = 8
};
struct NativeAdrenoAbiParams {
    uint32_t abi_version;
    uint32_t input_bits;          /* GPU VA bits below 2^input_bits */
    uint32_t output_bits;         /* physical address bits */
    uint32_t flags;               /* NativeAdrenoParams capability bits */
    uint64_t max_bo_bytes;
};
enum {
    NativeAdrenoParamsHostImport = 1, /* BoImport pins the caller's own pages (an IOSurface's) as a BO */
    NativeAdrenoParamsBoProfile = 2,
    NativeAdrenoParamsCpuMapV2 = 4,
    NativeAdrenoParamsBoProfileBudget = 8,
    NativeAdrenoParamsSubmitV2 = 16,
    NativeAdrenoParamsResourceBudget = 32,
    NativeAdrenoCpuMapVersion = 1
};

/* NativeAdrenoAbiBoProfile carries software allocation statistics from one
 * Vulkan device. A background reporter copies counters under the allocator
 * mutex, releases it, then sends these fixed records to the connection. USB
 * diagnostics print the saved records on request. The sequence and sample time
 * identify each snapshot; slow records have their own monotonically increasing
 * sequence. The driver retains bounded records per connection. */
enum {
    NativeAdrenoBoProfileVersion = 1,
    NativeAdrenoBoProfileCounters = 0,
    NativeAdrenoBoProfileSize = 1,
    NativeAdrenoBoProfileUsage = 2,
    NativeAdrenoBoProfileSlow = 3,
    NativeAdrenoBoProfileBudget = 4,
    NativeAdrenoBoProfileSizeCount = 17,
    NativeAdrenoBoProfileUsageCount = 16,
    NativeAdrenoBoProfileSlowCount = 64,
    NativeAdrenoBoSlowAllocate = 1,
    NativeAdrenoBoSlowImport = 2,
    NativeAdrenoBoSlowRelease = 3,
    NativeAdrenoBoSlowRetire = 4,
    NativeAdrenoBoCacheHit = 0,
    NativeAdrenoBoCacheEmpty = 1,
    NativeAdrenoBoCacheSize = 2,
    NativeAdrenoBoCacheAlignment = 3,
    NativeAdrenoBoCacheFlags = 4,
    NativeAdrenoBoCacheMemoryProperty = 5,
    NativeAdrenoBoCachePending = 6,
    NativeAdrenoBoCacheFixedAddress = 7,
    NativeAdrenoBoCacheExcludedFlags = 8,
    NativeAdrenoBoCacheOversize = 9,
    NativeAdrenoBoCacheImport = 10,
    NativeAdrenoBoCacheFailure = 11,
    NativeAdrenoBoCacheNotRequested = 12
};
struct NativeAdrenoAbiBoCacheCounters {
    uint64_t requests,requested_bytes,eligible,hits,misses;
    uint64_t miss_empty,miss_size,miss_alignment,miss_flags,miss_memory_property,miss_pending;
    uint64_t bypass_fixed,bypass_flags,bypass_oversize,imports,import_bytes;
    uint64_t puts,evict_objects,evict_bytes,evict_iova,evict_allocation,evict_close;
    uint64_t cached_objects,cached_bytes,peak_objects,peak_bytes,pending_objects,pending_bytes;
    uint64_t compatible_pending_requests,pressure_waits,reaped,slow_total,slow_overwritten,report_failures;
};
struct NativeAdrenoAbiBoCacheBin {
    uint64_t upper_bytes;         /* Inclusive size bound; UINT64_MAX is the final bin. */
    uint64_t requests,requested_bytes,eligible,hits,misses,compatible_pending_requests;
    char name[32];               /* Allocation purpose, or empty for a size bin. */
};
struct NativeAdrenoAbiBoSlowEvent {
    uint64_t elapsed_us,bytes,alignment,handle,iova,cached_bytes,pending_bytes;
    uint32_t operation,cache_reason,alloc_flags,memory_property;
    uint32_t connection_fence,cached_objects,pending_objects,status;
    char name[32];
};
struct NativeAdrenoAbiBoCacheBudget {
    uint64_t node_objects,node_limit,metadata_bytes,cache_budget_bytes;
    uint64_t reclaim_bytes,reclaim_high_bytes,reclaim_low_bytes;
};
struct NativeAdrenoAbiBoProfile {
    uint32_t version,kind,index,reserved;
    uint64_t sequence,sampled_us,device_id;
    union {
        NativeAdrenoAbiBoCacheCounters counters;
        NativeAdrenoAbiBoCacheBin bin;
        NativeAdrenoAbiBoSlowEvent slow;
        NativeAdrenoAbiBoCacheBudget budget;
    } data;
};
enum {
    NativeAdrenoBoAllocContiguous = 1
};
struct NativeAdrenoAbiBoAlloc {
    uint64_t bytes;               /* 4KiB multiple, <= max_bo_bytes */
    uint32_t align_log2;          /* 12..24 */
    uint32_t allocation_flags;    /* NativeAdrenoBoAlloc flags; zero uses CPU WB and coherent GPU access. */
};
struct NativeAdrenoAbiBoAllocReply {
    uint64_t handle;              /* opaque to userspace, nonzero */
};
/* NativeAdrenoAbiBoImport pins the calling task's pages until BoFree while
 * preserving its CPU mapping and cache mode. Hardware-coherent submission
 * uses cached GPU mappings for cached CPU pages and uncached GPU mappings for
 * Normal uncached CPU pages, with ordering before submission and completion.
 * Producers finish CPU writes before submit and resume access after completion.
 * An imported BO has no CPU-map token. */
struct NativeAdrenoAbiBoImport {
    uint64_t address;             /* 4KiB aligned */
    uint64_t bytes;               /* 4KiB multiple, <= max_bo_bytes */
};
struct NativeAdrenoAbiBoMap {
    uint64_t handle;
    uint64_t iova;                /* 4KiB aligned, inside the mounted aperture */
    uint32_t permissions;         /* bit0 read, bit1 write; read required */
    uint32_t writeback;           /* 0 selects backing attributes, 1 requires cached coherent backing. */
    uint32_t shareability;        /* 2 outer, 3 inner */
    uint32_t reserved;            /* zero */
};
struct NativeAdrenoAbiBoMapReply {
    uint64_t iova;
};
struct NativeAdrenoAbiBoFree {
    uint64_t handle;
};
struct NativeAdrenoAbiSubmitBuffer {
    uint64_t handle;
    uint64_t iova;
    uint64_t bytes;               /* 4-byte multiple, dword count = bytes/4 */
};
struct NativeAdrenoAbiSubmit {
    uint32_t buffer_count;        /* 1..NativeAdrenoAbiMaxBuffers */
    uint32_t reserved;            /* zero */
    uint64_t queue;               /* open queue; 0 is created by open() */
    uint64_t wait_fence;          /* zero or an earlier fence of this client */
    uint64_t wait_timeout_ns;     /* used only when wait_fence is nonzero */
    NativeAdrenoAbiSubmitBuffer buffers[NativeAdrenoAbiMaxBuffers];
};
struct NativeAdrenoAbiMetadata {
    uint64_t handle;              /* object identity */
    uint32_t bytes;               /* set: length; get: requested capacity */
    uint32_t reserved;            /* zero */
    char text[NativeAdrenoAbiMetadataBytes]; /* raw bytes, not NUL terminated */
};
struct NativeAdrenoAbiSubmitReply {
    uint64_t fence;               /* nonzero CP timestamp value */
};
enum {
    NativeAdrenoSubmitVersion = 1,
    NativeAdrenoSubmitMaxPacketBytes = 262144,
    NativeAdrenoSubmitMaxIbs = 512,
    NativeAdrenoSubmitMaxResources = 4096,
    NativeAdrenoSubmitKernelCacheMaintenance = 1,
    NativeAdrenoSubmitCachedCoherentImports = 2,
    NativeAdrenoSubmitHardwareCoherent = 4,
    NativeAdrenoSubmitAllMapped = 1,
    NativeAdrenoResourceRead = 1,
    NativeAdrenoResourceWrite = 2,
    NativeAdrenoFallbackNone = 0,
    NativeAdrenoFallbackIncompleteTracking = 1,
    NativeAdrenoFallbackDeviceAddress = 2,
    NativeAdrenoFallbackDescriptorBuffer = 4,
    NativeAdrenoFallbackLegacySubmit = 8,
    NativeAdrenoResourceBudgetVersion = 1,
    NativeAdrenoBudgetMapNoSpace = 1,
    NativeAdrenoMapFailureNone = 0,
    NativeAdrenoMapFailureCapacity = 1,
    NativeAdrenoMapFailureAddress = 2,
    NativeAdrenoMapFailureTables = 3,
    NativeAdrenoMapFailureOther = 4,
    NativeAdrenoMapFailureActiveAlias = 5
};
struct NativeAdrenoAbiSubmitCaps {
    uint32_t version,max_packet_bytes,max_ibs,max_resources;
    uint32_t flags,reserved;      /* NativeAdrenoSubmit capability flags. */
    uint64_t epoch;
};
/* NativeAdrenoAbiSubmitV2 contains offsets into one copied packet. Its arrays
 * contain NativeAdrenoAbiSubmitIbV2 and NativeAdrenoAbiSubmitResourceV2 records.
 * Each resource handle already includes the BO generation. The connection
 * keeps its GPU mapping unchanged until BoFree, so an additional mapping ID
 * is unnecessary. AllMapped retains every mapped BO in this connection while
 * the explicit resource array still supplies the IB indices. */
struct NativeAdrenoAbiSubmitV2 {
    uint32_t version,header_bytes,total_bytes,flags;
    uint32_t ib_count,resource_count,ib_offset,resource_offset;
    uint64_t queue,epoch;
    uint32_t fallback_reason,reserved;
    uint64_t reserved2;
};
struct NativeAdrenoAbiSubmitResourceV2 {
    uint64_t handle,offset,bytes;
    uint32_t access,reserved;
};
struct NativeAdrenoAbiSubmitIbV2 {
    uint32_t resource_index,reserved;
    uint64_t offset,bytes;        /* BO-relative range, contained in its resource. */
};
struct NativeAdrenoAbiSubmitV2Reply {
    uint64_t fence,epoch;
};

enum {
    NativeAdrenoAsyncVersion = 1,
    NativeAdrenoAsyncMaxJobs = 64,
    NativeAdrenoAsyncMaxSyncs = 4096,
    NativeAdrenoAsyncMaxWaits = 128,
    NativeAdrenoAsyncMaxSignals = 128,
    NativeAdrenoAsyncTimeline = 1,
    NativeAdrenoAsyncWaitAny = 1,
    NativeAdrenoAsyncWaitPending = 2,
};
struct NativeAdrenoAbiAsyncCaps {
    uint32_t version,max_jobs,max_waits,max_signals;
    uint64_t epoch;
};
struct NativeAdrenoAbiSyncCreate {
    uint64_t initial_value;
    uint32_t flags,reserved;
};
struct NativeAdrenoAbiSyncHandle {
    uint64_t handle;
};
struct NativeAdrenoAbiSyncPoint {
    uint64_t handle,value;
};
struct NativeAdrenoAbiSyncValue {
    uint64_t value;
};
/* NativeAdrenoAbiSubmitAsync appends dependency arrays to the established
 * resource packet. The embedded packet uses its own byte offsets. The outer
 * offsets name NativeAdrenoAbiSyncPoint arrays. No pointer crosses the ABI. */
struct NativeAdrenoAbiSubmitAsync {
    uint32_t version,total_bytes,submit_offset,submit_bytes;
    uint32_t wait_offset,wait_count,signal_offset,signal_count;
    uint64_t epoch;
};
struct NativeAdrenoAbiAsyncReply {
    uint64_t completion,epoch;
};
struct NativeAdrenoAbiSyncWait {
    uint32_t version,count,flags,reserved;
    uint64_t timeout_ns,epoch;
    NativeAdrenoAbiSyncPoint points[NativeAdrenoAsyncMaxWaits];
};
struct NativeAdrenoAbiCompletionWait {
    uint64_t completion,timeout_ns,epoch;
};
struct NativeAdrenoAbiCapacityWait {
    uint64_t timeout_ns,epoch;
};
struct NativeAdrenoAbiBoWait {
    uint64_t handle,timeout_ns,epoch;
};
/* NativeAdrenoAbiResourceBudget reports the shared mapping table and this
 * connection's share. connection_target is a fair cache target, not an
 * allocation refusal threshold. BoMap returns NoSpace only when no shared
 * mapping entry is available; page-table failures retain their own status. */
struct NativeAdrenoAbiResourceBudget {
    uint32_t version,flags,capacity,used,reserved,connection_used,connection_target,active_connections;
    uint64_t generation,last_failure_handle,last_failure_iova;
    uint32_t last_failure_reason,last_failure_status;
};
struct NativeAdrenoAbiWaitFence {
    uint64_t fence;
    uint64_t timeout_ns;          /* 0 polls once */
};
struct NativeAdrenoAbiTimestamp {
    uint64_t ticks;               /* GPU always-on counter value */
};
struct NativeAdrenoAbiBoCpuMap {
    uint64_t handle;
};
struct NativeAdrenoAbiBoCpuMapReply {
    uint32_t token;               /* clientMemoryForType selector */
    uint32_t reserved;            /* zero */
    uint64_t bytes;               /* bytes the mapping covers */
};
struct NativeAdrenoAbiBoCpuUnmap {
    uint64_t handle;
    uint32_t token;               /* exactly the token returned by map */
    uint32_t reserved;            /* zero */
};
/* NativeAdrenoAbiBoCpuMapV2 creates a complete CPU mapping in the calling task.
 * The connection retains the IOMemoryMap and its BO lease until UnmapV2 or
 * connection close removes the actual mapping. A nonzero address requests an
 * exact placement; zero lets the VM select an address. */
struct NativeAdrenoAbiBoCpuMapV2 {
    uint64_t handle;
    uint64_t address;
    uint32_t version;
    uint32_t flags;               /* zero */
};
struct NativeAdrenoAbiBoCpuMapV2Reply {
    uint64_t map_id;              /* unique within this connection */
    uint64_t address;
    uint64_t bytes;
};
struct NativeAdrenoAbiBoCpuUnmapV2 {
    uint64_t handle;
    uint64_t map_id;
};
struct NativeAdrenoAbiQueue {
    uint64_t queue;               /* zero for create, an open id for close */
};
struct NativeAdrenoAbiQueueReply {
    uint64_t queue;               /* nonzero id of the created queue */
};
struct NativeAdrenoAbiStatus {
    uint64_t faults;              /* monotonic; change means context lost */
    uint64_t suspends;            /* monotonic suspend/reset count */
};
/* Fixed facts of the mounted GPU that the Vulkan driver needs and cannot derive: all come from the board code, which takes
 * them from the pinned sources (a6xx catalog, UBWC config, the programmed UCHE ranges), never from a guess. */
enum {
    NativeAdrenoDeviceClientIova = 1,     /* flags: the client chooses BO addresses (BoMap iova) */
    NativeAdrenoDeviceCachedCoherent = 2, /* Cached coherent BO allocation is supported. */
    NativeAdrenoDeviceCachedCoherentImports = 4 /* Qualified cached imports use coherent SMMU mappings. */
};
struct NativeAdrenoAbiDeviceInfo {
    uint32_t chip_id;             /* ADRENO_CHIPID, e.g. 0x43050a01 */
    uint32_t gmem_bytes;
    uint64_t gmem_base;           /* UCHE GMEM window start (GPU address) */
    uint64_t uche_trap_base;
    uint64_t va_start;            /* first GPU address the client may map */
    uint64_t va_size;
    uint32_t highest_bank_bit;
    uint32_t ubwc_swizzle;        /* bit0 level 1, bit1 level 2, bit2 level 3 */
    uint32_t macrotile_mode;      /* 0 four channel, 1 eight channel */
    uint32_t ubwc_version;        /* encoder version, 4 for UBWC 4.0 */
    uint32_t flags;
    uint32_t reserved;            /* zero */
};
#if defined(__cplusplus)
static_assert(sizeof(NativeAdrenoAbiDeviceInfo) == 64, "ABI device info layout");
static_assert(sizeof(NativeAdrenoAbiParams) == 24, "ABI params layout");
static_assert(sizeof(NativeAdrenoAbiBoAlloc) == 16, "ABI alloc layout");
static_assert(sizeof(NativeAdrenoAbiBoMap) == 32, "ABI map layout");
static_assert(sizeof(NativeAdrenoAbiBoImport) == 16, "ABI import layout");
static_assert(sizeof(NativeAdrenoAbiSubmitBuffer) == 24, "ABI buffer layout");
static_assert(sizeof(NativeAdrenoAbiSubmit) == 416, "ABI submit layout");
static_assert(sizeof(NativeAdrenoAbiSubmitCaps) == 32, "ABI submit caps layout");
static_assert(sizeof(NativeAdrenoAbiSubmitV2) == 64, "ABI submit v2 layout");
static_assert(sizeof(NativeAdrenoAbiSubmitResourceV2) == 32, "ABI submit resource layout");
static_assert(sizeof(NativeAdrenoAbiSubmitIbV2) == 24, "ABI submit ib layout");
static_assert(sizeof(NativeAdrenoAbiSubmitV2Reply) == 16, "ABI submit v2 reply layout");
static_assert(sizeof(NativeAdrenoAbiResourceBudget) == 64, "ABI resource budget layout");
static_assert(sizeof(NativeAdrenoAbiBoCpuMap) == 8, "ABI cpu map layout");
static_assert(sizeof(NativeAdrenoAbiBoCpuMapReply) == 16, "ABI cpu map reply layout");
static_assert(sizeof(NativeAdrenoAbiBoCpuUnmap) == 16, "ABI cpu unmap layout");
static_assert(sizeof(NativeAdrenoAbiQueue) == 8, "ABI queue layout");
static_assert(sizeof(NativeAdrenoAbiQueueReply) == 8, "ABI queue reply layout");
static_assert(sizeof(NativeAdrenoAbiMetadata) == 80, "ABI metadata layout");
static_assert(sizeof(NativeAdrenoAbiWaitFence) == 16, "ABI wait layout");
static_assert(sizeof(NativeAdrenoAbiTimestamp) == 8, "ABI timestamp layout");
static_assert(sizeof(NativeAdrenoAbiStatus) == 16, "ABI status layout");
#endif
#endif
