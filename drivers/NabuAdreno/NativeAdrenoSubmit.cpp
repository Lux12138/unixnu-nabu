/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoScheduler.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address
 * and IB_SIZE in dwords (bits 0..19), exactly as the pinned adreno_pm4.xml
 * domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,SubmitMaxJobs=8,
    SubmitMaxBuffers=16,SubmitMaxIbBytes=0xfffffu*4u};
/* The board diagnostic ABI retains these zero-valued counters. */
extern "C" {volatile uint32_t native_adreno_coherence[8];}
extern "C" {volatile uint32_t native_adreno_wc_distrusted;}
extern "C" {volatile uint32_t native_adreno_wc_audit[6];}
/* Fence waits: [0] calls, [1] timeouts, [2] the fence of the last timed-out wait, [3] the fence value the GPU had written then, [4] the newest fence handed out,
 * [5] the newest completed, [6] waits for a fence no job holds (observeFence path), [7] the last fence refused as never submitted. */
extern "C" {volatile uint32_t native_adreno_fence_wait[8];}
/* The IBs of the last submissions: [slot] fence, iova low/high, bytes, then the first 28 dwords of the first IB (read as the kernel maps it). */
extern "C" {volatile uint32_t native_adreno_ib_log[4][4+28];volatile uint32_t native_adreno_ib_log_count;}
extern "C" {extern uint64_t (*native_adreno_vm_now_us)(void);extern volatile uint32_t native_adreno_method_stats[20][4];}

/* Bounded render-path profile for the GPU performance investigation.
 *
 * Every successful submission takes one record in a fixed 256-entry ring.
 * The completion path fills the fence latency when the CP timestamp is first
 * observed. The records are copied under a short spinlock by the gpu dump and
 * printed after the lock is released, so the render path never waits for the
 * console. No allocation, no disk I/O and no per-frame logging: a submission
 * only writes one record and updates a few counters. */
enum : unsigned {
    ProfileSamples = 256,
    ProfileBuckets = 8,
    ProfileClientChars = 16,
};
struct ProfileSample {
    uint32_t seq;
    uint32_t fence;
    uint32_t ibCount;
    uint32_t complete;
    char client[ProfileClientChars];
    uint64_t submitUs;
    uint64_t completeUs;
    uint32_t latencyUs;
    uint32_t retireUs;
    uint32_t cacheUs;
    uint32_t publishUs;
    uint32_t tlbUs;
    uint32_t ringUs;
    uint32_t submitCpuUs;
    uint32_t mapCalls;
    uint32_t freeCalls;
    uint32_t allocCalls;
    uint64_t mapUs;
    uint64_t freeUs;
    uint64_t allocUs;
    uint32_t resourceCount,importCount,fallbackReason;
    uint64_t importBytes;
    uint32_t aliasUs;
    uint64_t aliasQueries,aliasHits;
    uint64_t continuousSubmitUs;
    uint64_t continuousCompleteObservedUs;
};
static ProfileSample profileSamples[ProfileSamples];
static uint32_t profileHead;
static uint32_t profileSeq;
static uint32_t profileSubmitted;
static uint32_t profileCompleted;
static uint64_t profileLatencyTotal;
static uint64_t profileLatencyMax;
static uint64_t profileLatencyBuckets[ProfileBuckets];
static char profileClient[ProfileClientChars];
static uint32_t profileLock;
static uint64_t (*profileContinuousClock)(void);

enum : unsigned {
    ProfileSurfaceWrites = 4096,
    ProfileSurfaceDescriptors = 32,
    ProfileSurfaceBackings = 16,
    ProfileReleasedWriters = 64,
};

/* SurfaceWrite records one submission's declared writes to one imported BO.
 * Each BO links its own records through sequence numbers in the bounded ring.
 * A sequence mismatch identifies overwritten history without a global scan. */
struct SurfaceWrite {
    uint64_t sequence = 0;
    uint64_t previous = 0;
    uint64_t handle = 0;
    uint64_t submitUs = 0;
    uint64_t ringUs = 0;
    uint32_t fence = 0;
    uint32_t submitSequence = 0;
    uint32_t fallback = 0;
};

struct SurfaceWriter {
    uint64_t handle = 0;
    uint64_t firstPhysical = 0;
    uint64_t bytes = 0;
    uint64_t latestWrite = 0;
};

struct ReleasedSurfaceWriter {
    uint64_t firstPhysical = 0;
    uint64_t bytes = 0;
    uint64_t releasedUs = 0;
};

/* SurfaceDescriptor retains an exactly matched source descriptor while its
 * imported BOs remain mapped. Its generation handles keep repeated captures
 * independent of unrelated BO allocation. New aliases of the same first
 * physical byte and length require one additional complete extent comparison. */
struct SurfaceDescriptor {
    IOMemoryDescriptor *memory = nullptr;
    uint64_t firstPhysical = 0;
    uint64_t bytes = 0;
    uint64_t used = 0;
    uint64_t handles[ProfileSurfaceBackings]{};
    uint32_t count = 0;
    uint32_t flags = 0;
    bool dirty = false;
};

static SurfaceWrite surfaceWrites[ProfileSurfaceWrites];
static SurfaceWriter surfaceWriters[NativeAdrenoVmMaxObjects];
static SurfaceDescriptor surfaceDescriptors[ProfileSurfaceDescriptors];
static uint64_t surfaceWriteSequence;
static uint64_t surfaceDescriptorSequence;
static ReleasedSurfaceWriter releasedSurfaceWriters[ProfileReleasedWriters];
static uint32_t releasedSurfaceWriterNext;
static uint64_t releasedSurfaceHistoryExpiredUs;

extern "C" void native_adreno_profile_set_continuous_clock(uint64_t (*clock)(void))
{
    profileContinuousClock = clock;
}

static uint64_t profileContinuousNow()
{
    return profileContinuousClock ? profileContinuousClock() : 0;
}

static void profileForgetBacking(uint64_t handle)
{
    const size_t slot = size_t(handle & (NativeAdrenoVmMaxObjects - 1));
    if (surfaceWriters[slot].handle == handle) {
        const SurfaceWriter &writer = surfaceWriters[slot];
        ReleasedSurfaceWriter &released = releasedSurfaceWriters[releasedSurfaceWriterNext];
        if (released.releasedUs > releasedSurfaceHistoryExpiredUs) {
            releasedSurfaceHistoryExpiredUs = released.releasedUs;
        }
        released = {writer.firstPhysical, writer.bytes, profileContinuousNow()};
        releasedSurfaceWriterNext = (releasedSurfaceWriterNext + 1) % ProfileReleasedWriters;
        surfaceWriters[slot] = {};
    }

    for (SurfaceDescriptor &source : surfaceDescriptors) {
        for (uint32_t at = 0; at < source.count; ++at) {
            if (source.handles[at] != handle) {
                continue;
            }

            source.handles[at] = source.handles[--source.count];
            if (!source.count) {
                source.memory->release();
                source = {};
            }
            break;
        }
    }
}

static void profileNoteSurfaceWrites(NativeAdrenoVm *vm,
    const NativeAdrenoSubmitResource *resources, size_t resourceCount,
    const uint16_t *mappingResources, uint32_t fence, uint32_t submitSequence,
    uint32_t fallback, uint64_t submitUs, uint64_t ringUs)
{
    for (size_t at = 0; at < resourceCount; ++at) {
        const NativeAdrenoSubmitResource &resource = resources[at];
        if (!resource.owns_reference) {
            continue;
        }

        const NativeAdrenoMemory *backing = vm->resourceBacking(resource.handle);
        if (!backing || !backing->imported() || !backing->spanCount()) {
            continue;
        }

        const size_t slot = size_t(resource.handle & (NativeAdrenoVmMaxObjects - 1));
        bool writes = false;
        for (uint16_t item = mappingResources[slot]; item;) {
            const NativeAdrenoSubmitResource &range = resources[item - 1];
            writes |= (range.access & NativeAdrenoResourceWrite) != 0;
            item = range.next_for_mapping;
        }
        if (!writes) {
            continue;
        }

        SurfaceWriter &writer = surfaceWriters[slot];
        if (writer.handle != resource.handle) {
            writer = {};
            writer.handle = resource.handle;
            writer.firstPhysical = backing->spans()[0].physical;
            writer.bytes = backing->bytes();
            for (SurfaceDescriptor &source : surfaceDescriptors) {
                if (source.memory && source.firstPhysical == writer.firstPhysical &&
                    source.bytes == writer.bytes) {
                    source.dirty = true;
                }
            }
        }

        const uint64_t sequence = ++surfaceWriteSequence;
        SurfaceWrite &write = surfaceWrites[(sequence - 1) % ProfileSurfaceWrites];
        write = {sequence, writer.latestWrite, resource.handle, submitUs, ringUs,
            fence, submitSequence, fallback};
        writer.latestWrite = sequence;
    }
}
struct SubmitFailure {
    uint64_t sampled_us=0,handle=0,iova=0,bytes=0;
    uint32_t stage=0,status=0,index=0,ibs=0,resources=0;
    char client[ProfileClientChars]{};
    NativeAdrenoSubmitResourceFailure resource{};
};
static SubmitFailure firstSubmitFailure;
static SubmitFailure firstResourceFailure;
static uint64_t submitV2Counters[8]{}; /* Calls, resources, fallback, clean bytes, acquire bytes, busy, completed, failures. */
static uint64_t aliasTotals[4]{}; /* Elapsed us, mapping interval checks, callback ranges and Busy attempts at a query limit. */
/* Per-submission interval deltas of the ABI method totals, so the dump can
 * show the BO work between two submissions without sampling the clock. */
static uint32_t profileLastCalls[6];
static uint32_t profileLastUs[6];
static uint32_t profileMapCalls[5];
static uint64_t profileMapUs[5];
static uint64_t profileMapMax[5];
static uint32_t profileLockCalls[NativeAdrenoMethodGetResourceBudget+1];
static uint64_t profileLockUs[NativeAdrenoMethodGetResourceBudget+1];
static uint64_t profileLockMax[NativeAdrenoMethodGetResourceBudget+1];
static uint64_t profileNow()
{
    return native_adreno_vm_now_us ? native_adreno_vm_now_us() : 0;
}
static void profileEnter()
{
    while (__atomic_test_and_set(&profileLock, __ATOMIC_ACQUIRE)) {
    }
}
static void profileLeave()
{
    __atomic_clear(&profileLock, __ATOMIC_RELEASE);
}
extern "C" void native_adreno_submit_failure(uint32_t stage,uint32_t status,uint64_t handle,
    uint64_t iova,uint32_t index,uint32_t ibs,uint32_t resources,uint64_t bytes,
    const NativeAdrenoSubmitResourceFailure *resource)
{
    profileEnter();
    ++submitV2Counters[7];
    const bool first_resource=stage==2 && resource && !firstResourceFailure.stage;
    if(!firstSubmitFailure.stage || first_resource) {
        SubmitFailure failure;
        failure.sampled_us=profileNow();
        failure.handle=handle;
        failure.iova=iova;
        failure.bytes=bytes;
        failure.stage=stage;
        failure.status=status;
        failure.index=index;
        failure.ibs=ibs;
        failure.resources=resources;
        memcpy(failure.client,profileClient,sizeof(profileClient));
        if(resource) {
            failure.resource=*resource;
        }
        if(!firstSubmitFailure.stage) {
            firstSubmitFailure=failure;
        }
        if(first_resource) {
            firstResourceFailure=failure;
        }
    }
    profileLeave();
}
extern "C" void native_adreno_profile_set_client(const char *name)
{
    if (!name) {
        name = "?";
    }
    profileEnter();
    unsigned at = 0;
    for (; at < sizeof(profileClient); ++at) {
        profileClient[at] = 0;
    }
    for (at = 0; at + 1 < sizeof(profileClient) && name[at]; ++at) {
        profileClient[at] = name[at];
    }
    profileLeave();
}
extern "C" void native_adreno_profile_map(uint32_t pages, uint64_t us)
{
    const unsigned bucket = pages == 1 ? 4 : pages < 16 ? 0 : pages < 256 ? 1 : pages < 4096 ? 2 : 3;
    profileEnter();
    profileMapCalls[bucket] = profileMapCalls[bucket] + 1;
    profileMapUs[bucket] += us;
    if (us > profileMapMax[bucket]) {
        profileMapMax[bucket] = us;
    }
    profileLeave();
}
extern "C" void native_adreno_profile_lock_wait(uint32_t method, uint64_t us)
{
    if (method <= NativeAdrenoMethodGetResourceBudget) {
        profileEnter();
        profileLockCalls[method] = profileLockCalls[method] + 1;
        profileLockUs[method] += us;
        if (us > profileLockMax[method]) {
            profileLockMax[method] = us;
        }
        profileLeave();
    }
}
static uint32_t profileNoteSubmit(uint32_t fence, uint32_t ibCount,
    uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, uint64_t t5,
    uint32_t resources=0,uint32_t imports=0,uint32_t fallback=0,uint64_t import_bytes=0,
    uint32_t alias_us=0,uint64_t alias_queries=0,uint64_t alias_hits=0,uint32_t cache_us=UINT32_MAX,
    uint64_t continuous_submit_us=0)
{
    profileEnter();
    uint32_t intervalCalls[6] = {};
    uint32_t intervalUs[6] = {};
    for (unsigned method = 2; method <= 5; ++method) {
        const uint32_t calls = native_adreno_method_stats[method][0];
        const uint32_t us = native_adreno_method_stats[method][3];
        intervalCalls[method] = calls - profileLastCalls[method];
        intervalUs[method] = us - profileLastUs[method];
        profileLastCalls[method] = calls;
        profileLastUs[method] = us;
    }
    ProfileSample &sample = profileSamples[profileHead];
    sample.seq = ++profileSeq;
    sample.fence = fence;
    sample.ibCount = ibCount;
    sample.resourceCount=resources;
    sample.importCount=imports;
    sample.fallbackReason=fallback;
    sample.importBytes=import_bytes;
    sample.complete = 0;
    for (unsigned at = 0; at < sizeof(sample.client); ++at) {
        sample.client[at] = profileClient[at];
    }
    sample.submitUs = t0;
    sample.completeUs = 0;
    sample.continuousSubmitUs = continuous_submit_us;
    sample.continuousCompleteObservedUs = 0;
    sample.latencyUs = 0;
    sample.retireUs = uint32_t(t1 - t0);
    sample.cacheUs = cache_us==UINT32_MAX?uint32_t(t2-t1):cache_us;
    sample.publishUs = cache_us==UINT32_MAX?uint32_t(t3-t2):uint32_t(t3-t1)-cache_us;
    sample.tlbUs = uint32_t(t4 - t3);
    sample.ringUs = uint32_t(t5 - t4);
    sample.submitCpuUs = uint32_t(t5 - t0);
    sample.aliasUs=alias_us;
    sample.aliasQueries=alias_queries;
    sample.aliasHits=alias_hits;
    sample.mapCalls = intervalCalls[3];
    sample.mapUs = intervalUs[3];
    sample.freeCalls = intervalCalls[4];
    sample.freeUs = intervalUs[4];
    sample.allocCalls = intervalCalls[2];
    sample.allocUs = intervalUs[2];
    profileHead = (profileHead + 1) % ProfileSamples;
    profileSubmitted = profileSubmitted + 1;
    const uint32_t sequence = sample.seq;
    profileLeave();
    return sequence;
}
static void profileNoteComplete(uint32_t fence)
{
    if (!fence) {
        return;
    }
    const uint64_t now = profileNow();
    const uint64_t continuousNow = profileContinuousNow();
    if (!now) {
        return;
    }
    profileEnter();
    for (unsigned at = 0; at < ProfileSamples; ++at) {
        ProfileSample &sample = profileSamples[at];
        if (!sample.seq || sample.complete || !sample.fence) {
            continue;
        }
        if (int32_t(fence - sample.fence) < 0) {
            continue;
        }
        sample.complete = 1;
        sample.completeUs = now;
        sample.continuousCompleteObservedUs = continuousNow;
        const uint64_t latency = now > sample.submitUs ? now - sample.submitUs : 0;
        sample.latencyUs = uint32_t(latency > 0xffffffffu ? 0xffffffffu : latency);
        profileCompleted = profileCompleted + 1;
        profileLatencyTotal += latency;
        if (latency > profileLatencyMax) {
            profileLatencyMax = latency;
        }
        const unsigned bucket = latency < 5000 ? 0 : latency < 10000 ? 1 : latency < 20000 ? 2 :
            latency < 50000 ? 3 : latency < 100000 ? 4 : latency < 200000 ? 5 :
            latency < 500000 ? 6 : 7;
        profileLatencyBuckets[bucket] = profileLatencyBuckets[bucket] + 1;
    }
    profileLeave();
}

static bool profileMatchSurface(IOMemoryDescriptor *source,
    const NativeAdrenoMemory *backing, NativeAdrenoSurfaceProfile &result)
{
    if (!backing || !backing->imported() || backing->bytes() != source->getLength()) {
        return false;
    }

    uint64_t offset = 0;
    for (size_t at = 0; at < backing->spanCount(); ++at) {
        const NativeAdrenoPhysicalSpan &span = backing->spans()[at];
        if (span.offset != offset) {
            return false;
        }

        uint64_t compared = 0;
        while (compared < span.bytes) {
            if (result.matchSegments == 65536) {
                result.flags |= NativeAdrenoSurfaceProfileMatchLimit;
                return false;
            }
            ++result.matchSegments;

            IOByteCount available = 0;
            const uint64_t physical = source->getPhysicalSegment(offset, &available, kIOMemoryMapperNone);
            if (!physical || !available) {
                result.flags |= NativeAdrenoSurfaceProfileSegmentsUnavailable;
                return false;
            }
            if (physical != span.physical + compared) {
                return false;
            }

            const uint64_t remaining = span.bytes - compared;
            const uint64_t count = available < remaining ? available : remaining;
            compared += count;
            offset += count;
        }
    }
    return offset == source->getLength();
}

void NativeAdrenoSubmit::captureSurfaceProfile(IOMemoryDescriptor *memory,
    uint32_t capturedFence, uint64_t compositionStartUs, NativeAdrenoSurfaceProfile *output)
{
    NativeAdrenoSurfaceProfile &result = *output;
    result = {};
    result.compositionStartUs = compositionStartUs;
    result.capturedFence = capturedFence;
    result.capturedUs = profileContinuousNow();
    if (!compositionStartUs) {
        result.flags = NativeAdrenoSurfaceProfileMissingStart;
        return;
    }

    SurfaceDescriptor *source = nullptr;
    for (SurfaceDescriptor &candidate : surfaceDescriptors) {
        if (candidate.memory == memory) {
            source = &candidate;
            break;
        }
    }

    const uint64_t matchBegan = profileContinuousNow();
    if (source && !source->dirty) {
        result.matchCacheHit = 1;
    } else {
        if (!source) {
            IOByteCount available = 0;
            const uint64_t firstPhysical = memory->getPhysicalSegment(0, &available, kIOMemoryMapperNone);
            if (!firstPhysical || !available) {
                result.flags = NativeAdrenoSurfaceProfileSegmentsUnavailable |
                    NativeAdrenoSurfaceProfileNoMatchingWrites;
                return;
            }

            for (SurfaceDescriptor &candidate : surfaceDescriptors) {
                if (!source || !candidate.memory || candidate.used < source->used) {
                    source = &candidate;
                }
                if (!candidate.memory) {
                    break;
                }
            }
            if (source->memory) {
                source->memory->release();
            }
            *source = {};
            source->memory = memory;
            source->memory->retain();
            source->firstPhysical = firstPhysical;
            source->bytes = memory->getLength();
        }

        source->dirty = false;
        for (const SurfaceWriter &writer : surfaceWriters) {
            if (!writer.handle || writer.firstPhysical != source->firstPhysical ||
                writer.bytes != source->bytes) {
                continue;
            }

            bool known = false;
            for (uint32_t at = 0; at < source->count; ++at) {
                known |= source->handles[at] == writer.handle;
            }
            if (known) {
                continue;
            }
            if (source->count == ProfileSurfaceBackings) {
                source->flags |= NativeAdrenoSurfaceProfileMatchLimit;
                break;
            }

            const NativeAdrenoMemory *backing = vm_->resourceBacking(writer.handle);
            if (profileMatchSurface(memory, backing, result)) {
                source->handles[source->count++] = writer.handle;
            }
            if (result.flags & NativeAdrenoSurfaceProfileMatchLimit) {
                source->flags |= NativeAdrenoSurfaceProfileMatchLimit;
                break;
            }
        }
    }
    result.matchUs = profileContinuousNow() - matchBegan;
    result.flags |= source->flags;
    source->used = ++surfaceDescriptorSequence;
    if (compositionStartUs <= releasedSurfaceHistoryExpiredUs) {
        result.flags |= NativeAdrenoSurfaceProfileWriteHistoryExpired;
    }
    for (const ReleasedSurfaceWriter &released : releasedSurfaceWriters) {
        if (released.releasedUs >= compositionStartUs &&
            released.firstPhysical == source->firstPhysical && released.bytes == source->bytes) {
            result.flags |= NativeAdrenoSurfaceProfileReleasedBacking;
        }
    }
    if (!source->count) {
        source->memory->release();
        *source = {};
        result.flags |= NativeAdrenoSurfaceProfileNoMatchingWrites;
        return;
    }

    uint32_t seenFences[ProfileSamples]{};
    uint32_t seenCount = 0;
    uint32_t lastSubmitSequence = 0;
    for (uint32_t backingAt = 0; backingAt < source->count; ++backingAt) {
        const uint64_t handle = source->handles[backingAt];
        const SurfaceWriter &writer = surfaceWriters[handle & (NativeAdrenoVmMaxObjects - 1)];
        if (writer.handle != handle || !vm_->resourceBacking(handle)) {
            result.flags |= NativeAdrenoSurfaceProfileReleasedBacking;
            continue;
        }

        uint64_t sequence = writer.latestWrite;
        for (unsigned visited = 0; sequence && visited < ProfileSurfaceWrites; ++visited) {
            const SurfaceWrite &write = surfaceWrites[(sequence - 1) % ProfileSurfaceWrites];
            if (write.sequence != sequence || write.handle != handle) {
                result.flags |= NativeAdrenoSurfaceProfileWriteHistoryExpired;
                break;
            }
            sequence = write.previous;
            if (write.submitUs < compositionStartUs) {
                break;
            }
            if (int32_t(write.fence - capturedFence) > 0) {
                continue;
            }

            bool known = false;
            for (uint32_t at = 0; at < seenCount; ++at) {
                known |= seenFences[at] == write.fence;
            }
            if (known) {
                continue;
            }
            if (seenCount == ProfileSamples) {
                result.flags |= NativeAdrenoSurfaceProfileMatchLimit;
                break;
            }
            seenFences[seenCount++] = write.fence;

            ++result.submissions;
            if (write.fallback) {
                ++result.fallbackSubmissions;
                result.flags |= NativeAdrenoSurfaceProfileFallbackAccess;
            } else {
                ++result.exactSubmissions;
            }
            if (!result.firstSubmitUs || write.submitUs < result.firstSubmitUs) {
                result.firstSubmitUs = write.submitUs;
            }
            if (write.ringUs > result.lastSubmitUs) {
                result.lastSubmitUs = write.ringUs;
            }
            if (!result.firstFence || int32_t(write.fence - result.firstFence) < 0) {
                result.firstFence = write.fence;
            }
            if (!result.lastFence || int32_t(write.fence - result.lastFence) > 0) {
                result.lastFence = write.fence;
                lastSubmitSequence = write.submitSequence;
            }

            if (result.storedSubmissions < NativeAdrenoSurfaceProfileSubmissionCapacity) {
                const uint32_t at = result.storedSubmissions++;
                result.fences[at] = write.fence;
                result.sequences[at] = write.submitSequence;
            } else {
                result.flags |= NativeAdrenoSurfaceProfileOutputTruncated;
            }
        }
    }

    if (!result.submissions) {
        result.flags |= NativeAdrenoSurfaceProfileNoMatchingWrites;
        return;
    }

    profileEnter();
    const ProfileSample &last = profileSamples[(lastSubmitSequence - 1) % ProfileSamples];
    if (last.seq != lastSubmitSequence || last.fence != result.lastFence) {
        result.flags |= NativeAdrenoSurfaceProfileSubmitHistoryExpired;
    } else if (last.complete) {
        result.completeObservedUs = last.continuousCompleteObservedUs;
    }
    profileLeave();
    if (!result.completeObservedUs) {
        result.flags |= NativeAdrenoSurfaceProfileCompletionMissing;
    }
}
/* Called by the gpu dump. Copy under the spinlock, then print without it. */
extern "C" void native_adreno_profile_dump(void)
{
    static ProfileSample samples[ProfileSamples];
    static uint32_t submitted, completed;
    static uint64_t latencyTotal, latencyMax;
    static uint64_t buckets[ProfileBuckets];
    static uint32_t mapCalls[5];
    static uint64_t mapUs[5];
    static uint64_t mapMax[5];
    static uint32_t lockCalls[NativeAdrenoMethodGetResourceBudget+1];
    static uint64_t lockUs[NativeAdrenoMethodGetResourceBudget+1];
    static uint64_t lockMax[NativeAdrenoMethodGetResourceBudget+1];
    SubmitFailure failure,resource_failure;
    uint64_t v2[8];
    uint64_t aliases[4];
    profileEnter();
    failure=firstSubmitFailure;
    resource_failure=firstResourceFailure;
    memcpy(v2,submitV2Counters,sizeof(v2));
    memcpy(aliases,aliasTotals,sizeof(aliases));
    submitted = profileSubmitted;
    completed = profileCompleted;
    latencyTotal = profileLatencyTotal;
    latencyMax = profileLatencyMax;
    for (unsigned at = 0; at < ProfileSamples; ++at) {
        samples[at] = profileSamples[at];
    }
    for (unsigned at = 0; at < ProfileBuckets; ++at) {
        buckets[at] = profileLatencyBuckets[at];
    }
    for (unsigned at = 0; at < 5; ++at) {
        mapCalls[at] = profileMapCalls[at];
        mapUs[at] = profileMapUs[at];
        mapMax[at] = profileMapMax[at];
    }
    for (unsigned at = 0; at <= NativeAdrenoMethodGetResourceBudget; ++at) {
        lockCalls[at] = profileLockCalls[at];
        lockUs[at] = profileLockUs[at];
        lockMax[at] = profileLockMax[at];
    }
    profileLeave();
    IOLog("NativeAdrenoProfile: resources calls=%llu objects=%llu fallback=%llu clean_bytes=%llu acquire_bytes=%llu busy=%llu completed=%llu failures=%llu\n",
        v2[0],v2[1],v2[2],v2[3],v2[4],v2[5],v2[6],v2[7]);
    IOLog("NativeAdrenoProfile: aliases version=2 us=%llu queries=%llu hits=%llu limit_busy=%llu\n",
        aliases[0],aliases[1],aliases[2],aliases[3]);
    if(failure.stage) {
        IOLog("NativeAdrenoProfile: first_failure client=%s stage=%u status=%08x index=%u ib=%u resources=%u t=%llu handle=%llx iova=%llx bytes=%llu\n",
            failure.client,failure.stage,failure.status,failure.index,failure.ibs,failure.resources,
            failure.sampled_us,failure.handle,failure.iova,failure.bytes);
    }
    if(resource_failure.stage) {
        const auto &resource=resource_failure.resource;
        static const char *const reasons[]={"none","handle_missing","not_mapped","unreleasable",
            "access_format","vm_query","permissions","range"};
        const unsigned reason=static_cast<unsigned>(resource.reason);
        IOLog("NativeAdrenoProfile: resource_failure client=%s index=%u reason=%s t=%llu handle=%llx\n",
            resource_failure.client,resource_failure.index,
            reason<sizeof(reasons)/sizeof(reasons[0])?reasons[reason]:"unknown",
            resource_failure.sampled_us,resource_failure.handle);
        IOLog("NativeAdrenoProfile: resource_request access=%x reserved=%x offset=%llx bytes=%llu\n",
            resource.access,resource.reserved,resource_failure.iova,resource_failure.bytes);
        IOLog("NativeAdrenoProfile: resource_entry slot_present=%u used=%u mapped=%u unreleasable=%u handle=%llx\n",
            resource.slot_present,resource.used,resource.mapped,resource.unreleasable,resource.entry_handle);
        IOLog("NativeAdrenoProfile: resource_memory mapped_handle=%llx bytes=%llu read_only=%u\n",
            resource.mapped_handle,resource.memory_bytes,resource.read_only);
        IOLog("NativeAdrenoProfile: resource_vm queried=%u status=%08x permissions=%x iova=%llx bytes=%llu\n",
            resource.vm_queried,resource.vm_status,resource.permissions,resource.vm_iova,resource.vm_bytes);
    }
    IOLog("NativeAdrenoProfile: summary submitted=%u completed=%u lat_total=%llu lat_max=%llu buckets=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        submitted, completed, (unsigned long long)latencyTotal, (unsigned long long)latencyMax,
        (unsigned long long)buckets[0], (unsigned long long)buckets[1], (unsigned long long)buckets[2],
        (unsigned long long)buckets[3], (unsigned long long)buckets[4], (unsigned long long)buckets[5],
        (unsigned long long)buckets[6], (unsigned long long)buckets[7]);
    IOLog("NativeAdrenoProfile: lock_wait m2=%u/%llu/%llu m3=%u/%llu/%llu m4=%u/%llu/%llu m5=%u/%llu/%llu m21=%u/%llu/%llu\n",
        lockCalls[2], (unsigned long long)lockUs[2], (unsigned long long)lockMax[2],
        lockCalls[3], (unsigned long long)lockUs[3], (unsigned long long)lockMax[3],
        lockCalls[4], (unsigned long long)lockUs[4], (unsigned long long)lockMax[4],
        lockCalls[5], (unsigned long long)lockUs[5], (unsigned long long)lockMax[5],
        lockCalls[21], (unsigned long long)lockUs[21], (unsigned long long)lockMax[21]);
    IOLog("NativeAdrenoProfile: map_pages one=%u/%llu/%llu small=%u/%llu/%llu medium=%u/%llu/%llu large=%u/%llu/%llu huge=%u/%llu/%llu\n",
        mapCalls[4], (unsigned long long)mapUs[4], (unsigned long long)mapMax[4],
        mapCalls[0], (unsigned long long)mapUs[0], (unsigned long long)mapMax[0],
        mapCalls[1], (unsigned long long)mapUs[1], (unsigned long long)mapMax[1],
        mapCalls[2], (unsigned long long)mapUs[2], (unsigned long long)mapMax[2],
        mapCalls[3], (unsigned long long)mapUs[3], (unsigned long long)mapMax[3]);
    for (unsigned at = 0; at < ProfileSamples; ++at) {
        const ProfileSample &sample = samples[at];
        if (!sample.seq) {
            continue;
        }
        IOLog("NativeAdrenoProfile: S seq=%u fence=%u client=%s ib=%u complete=%u t=%llu c=%llu lat=%u submit_cpu=%u retire=%u cache=%u publish=%u tlb=%u ring=%u map=%u/%llu free=%u/%llu alloc=%u/%llu\n",
            sample.seq, sample.fence, sample.client, sample.ibCount, sample.complete,
            (unsigned long long)sample.submitUs, (unsigned long long)sample.completeUs,
            sample.latencyUs, sample.submitCpuUs, sample.retireUs, sample.cacheUs,
            sample.publishUs, sample.tlbUs, sample.ringUs,
            sample.mapCalls, (unsigned long long)sample.mapUs,
            sample.freeCalls, (unsigned long long)sample.freeUs,
            sample.allocCalls, (unsigned long long)sample.allocUs);
        IOLog("NativeAdrenoProfile: R seq=%u fence=%u resources=%u imports=%u import_bytes=%llu fallback=%u\n",
            sample.seq,sample.fence,
            sample.resourceCount,sample.importCount,(unsigned long long)sample.importBytes,sample.fallbackReason);
        IOLog("NativeAdrenoProfile: A seq=%u fence=%u alias_us=%u queries=%llu hits=%llu\n",
            sample.seq,sample.fence,sample.aliasUs,sample.aliasQueries,sample.aliasHits);
    }
}
IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::find(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::free()
{
    for(auto &job:jobs_)if(!job.active)return &job;
    return nullptr;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    resource_storage_=IONewData(NativeAdrenoSubmitResource,size_t(SubmitMaxJobs)*NativeAdrenoSubmitMaxResources);
    mapping_resource_storage_=IONewData(uint16_t,size_t(SubmitMaxJobs)*NativeAdrenoVmMaxObjects);
    command_words_=IONewData(uint32_t,NativeAdrenoSubmitMaxIbs*4);
    if(!resource_storage_ || !mapping_resource_storage_ || !command_words_) {
        if(resource_storage_) {
            IODeleteData(resource_storage_,NativeAdrenoSubmitResource,size_t(SubmitMaxJobs)*NativeAdrenoSubmitMaxResources);
            resource_storage_=nullptr;
        }
        if(command_words_) {
            IODeleteData(command_words_,uint32_t,NativeAdrenoSubmitMaxIbs*4);
            command_words_=nullptr;
        }
        if(mapping_resource_storage_) {
            IODeleteData(mapping_resource_storage_,uint16_t,size_t(SubmitMaxJobs)*NativeAdrenoVmMaxObjects);
            mapping_resource_storage_=nullptr;
        }
        return kIOReturnNoMemory;
    }
    vm_=vm;ring_=ring;attached_=true;stats_={};
    scheduler_=new NativeAdrenoScheduler;
    if(!scheduler_ || scheduler_->attach(vm,this,ring)) {
        if(scheduler_) {
            delete scheduler_;
            scheduler_=nullptr;
        }
        detach();
        return kIOReturnNoMemory;
    }
    memset(surfaceWriters,0,sizeof(surfaceWriters));
    memset(surfaceWrites,0,sizeof(surfaceWrites));
    memset(releasedSurfaceWriters,0,sizeof(releasedSurfaceWriters));
    surfaceWriteSequence=0;
    releasedSurfaceWriterNext=0;
    releasedSurfaceHistoryExpiredUs=0;
    vm_->setBackingReleaseObserver(profileForgetBacking);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::release(Job &job)
{
    __asm__ volatile("dsb sy" ::: "memory");
    if(job.resources) {
        for(size_t at=0;at<job.resource_count;++at) {
            if(!job.resources[at].owns_reference) {
                continue;
            }
            if(vm_->retireJobAfterCompletion(job.resources[at].handle)) {
                return poison();
            }
            ++stats_.references_retired;
        }
        profileEnter();
        ++submitV2Counters[6];
        profileLeave();
        job={};
        return kIOReturnSuccess;
    }
    IOReturn status=kIOReturnSuccess;
    for(size_t i=0;i<job.count;++i) {
        IOReturn retired=vm_->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    job={};return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    if(poisoned_ || outstanding())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    if(scheduler_) {
        const IOReturn status=scheduler_->detach();
        if(status) {
            return status;
        }
        delete scheduler_;
        scheduler_=nullptr;
    }
    vm_->setBackingReleaseObserver(nullptr);
    for (SurfaceDescriptor &source : surfaceDescriptors) {
        if (source.memory) {
            source.memory->release();
            source = {};
        }
    }
    IODeleteData(resource_storage_,NativeAdrenoSubmitResource,size_t(SubmitMaxJobs)*NativeAdrenoSubmitMaxResources);
    IODeleteData(mapping_resource_storage_,uint16_t,size_t(SubmitMaxJobs)*NativeAdrenoVmMaxObjects);
    mapping_resource_storage_=nullptr;
    IODeleteData(command_words_,uint32_t,NativeAdrenoSubmitMaxIbs*4);
    resource_storage_=nullptr;
    command_words_=nullptr;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint32_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint32_t first,uint32_t count) const
{
    if(scheduler_ && scheduler_->rangeOutstanding(first,count)) {
        return true;
    }
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstanding() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint32_t queue,uint32_t *fence)
{
    const uint64_t profileT0=profileNow();
    uint64_t profileT1=profileT0,profileT2=profileT0,profileT3=profileT0,profileT4=profileT0,profileT5=profileT0;
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    /* Every buffer is validated against the live VM before any reference. */
    uint64_t handles[SubmitMaxBuffers];size_t handle_count=0;
    uint32_t words[SubmitMaxBuffers*(1+SubmitIbWords)];size_t word_count=0;
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)
            return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova ||
            buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)
            return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    IOReturn swept=retireCompleted();
    profileT1=profileNow();
    if(swept)return swept;
    Job *job=free();
    if(!job)return kIOReturnBusy;
    for(size_t i=0;i<handle_count;++i) {
        IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            while(i) {vm_->retireJobAfterCompletion(handles[--i]);++stats_.references_retired;}
            return status;
        }
        ++stats_.references_held;
    }
    uint32_t value=0;
    __asm__ volatile("dsb sy":::"memory");
    profileT2=profileNow();
    profileT3=profileT2;
    IOReturn status=vm_->invalidateTlb();
    profileT4=profileNow();
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    {
        const uint32_t slot=native_adreno_ib_log_count&3;uint32_t head[28];
        (void)vm_->peekWords(buffers[0].iova,head,28);
        native_adreno_ib_log[slot][0]=ring_->lastSubmitted()+1;native_adreno_ib_log[slot][1]=(uint32_t)buffers[0].iova;
        native_adreno_ib_log[slot][2]=(uint32_t)(buffers[0].iova>>32);native_adreno_ib_log[slot][3]=(uint32_t)buffers[0].bytes;
        for(unsigned i=0;i<28;++i)native_adreno_ib_log[slot][4+i]=head[i];
        native_adreno_ib_log_count=native_adreno_ib_log_count+1;
    }
    status=ring_->submitFramed(words,word_count,&value);
    profileT5=profileNow();
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the
         * referenced buffers cannot be retired here. */
        if(ring_->poisoned()) {
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;
            return poison();
        }
        while(handle_count) {vm_->retireJobAfterCompletion(handles[--handle_count]);++stats_.references_retired;}
        return status;
    }
    for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
    job->count=handle_count;job->fence=value;job->queue=queue;job->active=true;
    ++stats_.submissions;
    if(outstanding()>stats_.peak_jobs)stats_.peak_jobs=outstanding();
    profileNoteSubmit(value,(uint32_t)count,profileT0,profileT1,profileT2,profileT3,profileT4,profileT5);
    if(ring_->completedFence()>=value)profileNoteComplete(value);
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::submitResources(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    const NativeAdrenoSubmitResource *resources,size_t resource_count,uint32_t queue,
    uint32_t fallback_reason,uint32_t *fence)
{
    if(!fence) {
        return kIOReturnBadArgument;
    }
    *fence=0;
    if(!attached_ || poisoned_) {
        return kIOReturnNotReady;
    }
    if((count && !buffers) || count>NativeAdrenoSubmitMaxIbs ||
        (resource_count && !resources) || resource_count>NativeAdrenoSubmitMaxResources) {
        return kIOReturnBadArgument;
    }

    const uint64_t continuousBegan=profileContinuousNow();
    const uint64_t began=profileNow();
    IOReturn status=retireCompleted();
    const uint64_t retired=profileNow();
    if(status) {
        return status;
    }
    Job *job=free();
    if(!job) {
        return kIOReturnBusy;
    }

    size_t word_count=0;
    for(size_t at=0;at<count;++at) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[at];
        uint64_t iova=0;
        size_t bytes=0;
        unsigned permissions=0;
        status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes ||
            !(permissions&ArmSmmu4KRead) || buffer.iova<iova || buffer.iova-iova>bytes ||
            buffer.bytes>bytes-(buffer.iova-iova)) {
            native_adreno_submit_failure(4,kIOReturnBadArgument,buffer.handle,buffer.iova,
                uint32_t(at),uint32_t(count),uint32_t(resource_count));
            return kIOReturnBadArgument;
        }
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success) {
            return poison();
        }
        command_words_[word_count++]=header;
        command_words_[word_count++]=uint32_t(buffer.iova);
        command_words_[word_count++]=uint32_t(buffer.iova>>32);
        command_words_[word_count++]=uint32_t(buffer.bytes/4);
    }

    NativeAdrenoSubmitResource *held=resource_storage_+(job-jobs_)*NativeAdrenoSubmitMaxResources;
    uint16_t *mapping_resources=mapping_resource_storage_+(job-jobs_)*NativeAdrenoVmMaxObjects;
    uint64_t referenced_slots[(NativeAdrenoVmMaxObjects+63)/64]{};
    const size_t overlap_count=0;
    for(size_t at=0;at<resource_count;++at) {
        held[at]=resources[at];
        NativeAdrenoSubmitResource &resource=held[at];
        const size_t slot=size_t(resource.handle&(NativeAdrenoVmMaxObjects-1));
        const uint64_t bit=UINT64_C(1)<<(slot&63);
        if(!(referenced_slots[slot/64]&bit)) {
            referenced_slots[slot/64]|=bit;
            mapping_resources[slot]=0;
        }
        resource.next_for_mapping=mapping_resources[slot];
        mapping_resources[slot]=uint16_t(at+1);
    }

    uint64_t alias_queries=0,alias_hits=0;
    const size_t cache_line=64;
    const uint32_t alias_us=0;

    size_t held_count=0;
    for(;held_count<resource_count;++held_count) {
        NativeAdrenoSubmitResource &resource=held[held_count];
        resource.owns_reference=false;
        const size_t slot=size_t(resource.handle&4095);
        if(mapping_resources[slot]!=held_count+1) {
            continue;
        }
        status=vm_->holdJob(held[held_count].handle);
        if(status) {
            break;
        }
        resource.owns_reference=true;
        ++stats_.references_held;
    }
    if(status) {
        while(held_count) {
            const NativeAdrenoSubmitResource &resource=held[--held_count];
            if(resource.owns_reference) {
                vm_->retireJobAfterCompletion(resource.handle);
                ++stats_.references_retired;
            }
        }
        return status;
    }

    uint64_t import_bytes=0;
    uint32_t import_count=0;
    for(size_t at=0;at<resource_count;++at) {
        const NativeAdrenoSubmitResource &resource=held[at];
        if(vm_->resourceImported(resource.handle)) {
            import_count+=resource.owns_reference;
            import_bytes+=resource.bytes;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t published=profileNow();
    /* Mapping fills invalid leaves, and unmapping synchronizes its removed
     * translations before returning. Submission preserves those translations. */
    const uint64_t tlb=profileNow();
    if(!status) {
        uint32_t value=0;
        status=ring_->submitFramed(command_words_,word_count,&value);
        const uint64_t continuousPublished=profileContinuousNow();
        if(!status || ring_->poisoned()) {
            job->resources=held;
            job->resource_count=resource_count;
            job->overlap_count=overlap_count;
            job->mapping_resources=mapping_resources;
            job->cache_line=cache_line;
            job->queue=queue;
            job->fence=value;
            job->active=true;
            if(status) {
                ++stats_.poisoned_jobs;
                native_adreno_submit_failure(6,status,0,0,0,uint32_t(count),uint32_t(resource_count));
                return poison();
            }
            ++stats_.submissions;
            if(outstanding()>stats_.peak_jobs) {
                stats_.peak_jobs=outstanding();
            }
            profileEnter();
            ++submitV2Counters[0];
            submitV2Counters[1]+=resource_count;
            submitV2Counters[2]+=fallback_reason!=0;
            profileLeave();
            const uint32_t submitSequence=profileNoteSubmit(value,uint32_t(count),began,retired,retired,published,tlb,profileNow(),
                uint32_t(resource_count),import_count,fallback_reason,import_bytes,
                alias_us,alias_queries,alias_hits,0,continuousBegan);
            profileNoteSurfaceWrites(vm_,held,resource_count,mapping_resources,value,submitSequence,
                fallback_reason,continuousBegan,continuousPublished);
            *fence=value;
            return kIOReturnSuccess;
        }
        if(status==kIOReturnNoResources) {
            status=kIOReturnBusy;
        }
    }

    while(held_count) {
        const NativeAdrenoSubmitResource &resource=held[--held_count];
        if(resource.owns_reference) {
            vm_->retireJobAfterCompletion(resource.handle);
            ++stats_.references_retired;
        }
    }
    return status;
}

IOReturn NativeAdrenoSubmit::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!fence)return kIOReturnBadArgument;
    const NativeAdrenoRingFence result=ring_->observeFence(fence,clock,timeout_us,nullptr);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
    if(result==NativeAdrenoRingFenceTimeout)return kIOReturnTimeout;
    profileNoteComplete(fence);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Job *job=find(fence);
    uint32_t observed=0;
    native_adreno_fence_wait[0]=native_adreno_fence_wait[0]+1;
    if(!job) {
        native_adreno_fence_wait[6]=native_adreno_fence_wait[6]+1;
        const NativeAdrenoRingFence passed=ring_->observeFence(fence,clock,timeout_us,&observed);
        if(passed==NativeAdrenoRingFencePoisoned)return poison();
        if(passed==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
        if(passed==NativeAdrenoRingFenceTimeout) {
            ++stats_.timeouts;
            native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
            native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
            return kIOReturnTimeout;
        }
        profileNoteComplete(fence);
        return retireCompleted();
    }
    const NativeAdrenoRingFence result=ring_->wait(fence,clock,timeout_us,&observed);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
    if(result==NativeAdrenoRingFenceTimeout) {
        ++stats_.timeouts;
        native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
        native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
        return kIOReturnTimeout;
    }
    profileNoteComplete(fence);
    return retireCompleted();
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(outstanding()) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(rangeOutstanding(first,count)) {
        if(scheduler_) {
            const IOReturn scheduled=scheduler_->advance();
            if(scheduled) {
                return scheduled;
            }
            if(!rangeOutstanding(first,count)) {
                break;
            }
        }
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && job.queue-first<count && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        if(!oldest) {
            return kIOReturnTimeout;
        }
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    if (!attached_ || poisoned_) {
        return kIOReturnNotReady;
    }

    Job *completed[SubmitMaxJobs];
    size_t count = 0;
    for (auto &job : jobs_) {
        if (!job.active) {
            continue;
        }

        const NativeAdrenoRingFence state = ring_->observeFence(job.fence, nullptr, 0, nullptr);
        if (state == NativeAdrenoRingFencePoisoned) {
            return poison();
        }
        if (state != NativeAdrenoRingFenceComplete) {
            continue;
        }

        profileNoteComplete(job.fence);
        ring_->collectGpuProfile(job.fence,job.queue,true);
        completed[count++] = &job;
    }
    if (!count) {
        return kIOReturnSuccess;
    }

    for (size_t i = 0; i < count; ++i) {
        const IOReturn status = release(*completed[i]);
        if (status) {
            return status;
        }

        ++stats_.completed;
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoSubmit::releaseAfterDmaStopped()
{
    if(scheduler_) {
        scheduler_->fault(kIOReturnIOError,true);
    }
    for(Job &job:jobs_) {
        if(job.active) {
            ring_->collectGpuProfile(job.fence,job.queue,false);
            const IOReturn status=release(job);
            if(status) {
                return status;
            }
        }
    }
    return kIOReturnSuccess;
}
