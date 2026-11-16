/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_DISPLAY_H
#define NATIVE_ADRENO_DISPLAY_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
class IOMemoryDescriptor;
struct NativeAdrenoSurfaceRead;

/* NativeAdrenoSurfaceRead retains the prepared surface memory and its GPU
 * mapping references while DPU reads it. Acquisition records the current GPU
 * writers and registers the DPU read before allowing another GPU submission.
 * A CPU-produced surface with no GPU writer is ready immediately.
 *
 * Success means ready; NotReady means that a writer is outstanding and the
 * callback will report completion. Both results transfer one read to out.
 * Other errors leave out null. Callbacks run without the GPU lock. Display
 * signals its workloop from the callback and releases the read after the DPU
 * has stopped scanning it, or after cancelling an unpresented request. */
extern "C" IOReturn native_adreno_surface_acquire_read(IOMemoryDescriptor *,
    void (*ready)(void *, IOReturn), void *context, NativeAdrenoSurfaceRead **out);

/* The hardware fence is diagnostic information. The read object owns all
 * writer dependencies, including work accepted before its ring publication. */
extern "C" IOReturn native_adreno_surface_read_status(NativeAdrenoSurfaceRead *,
    uint32_t *latest_hardware_fence);

/* Release cancels a pending callback and waits for an executing callback to
 * return before releasing context ownership. The callback must not call this
 * function for its own read. After return, context is never accessed again. */
extern "C" void native_adreno_surface_release_read(NativeAdrenoSurfaceRead *);

enum : uint32_t {
    NativeAdrenoSurfaceProfileMissingStart = 1u << 0,
    NativeAdrenoSurfaceProfileNoMatchingWrites = 1u << 1,
    NativeAdrenoSurfaceProfileWriteHistoryExpired = 1u << 2,
    NativeAdrenoSurfaceProfileSubmitHistoryExpired = 1u << 3,
    NativeAdrenoSurfaceProfileReleasedBacking = 1u << 4,
    NativeAdrenoSurfaceProfileOutputTruncated = 1u << 5,
    NativeAdrenoSurfaceProfileFallbackAccess = 1u << 6,
    NativeAdrenoSurfaceProfileCompletionMissing = 1u << 7,
    NativeAdrenoSurfaceProfileMatchLimit = 1u << 8,
    NativeAdrenoSurfaceProfileSegmentsUnavailable = 1u << 9,
    NativeAdrenoSurfaceProfileSubmissionCapacity = 64,
};

/* NativeAdrenoSurfaceProfile records submissions that declare writes to the
 * displayed surface's exact imported backing after composition starts. The
 * shared ring fence limits the search; it does not establish frame membership.
 * Intermediate render targets remain outside this direct-writer list.
 *
 * Every timestamp uses mach_continuous_time converted to microseconds.
 * completeObservedUs is the latest first CPU observation of a matching GPU
 * fence completion. It bounds completion from above. Display owns the native
 * composition timestamp, source descriptor lifetime and DPU presentation. */
struct NativeAdrenoSurfaceProfile {
    uint64_t compositionStartUs = 0;
    uint64_t capturedUs = 0;
    uint64_t firstSubmitUs = 0;
    uint64_t lastSubmitUs = 0;
    uint64_t completeObservedUs = 0;
    uint32_t capturedFence = 0;
    uint32_t firstFence = 0;
    uint32_t lastFence = 0;
    uint32_t submissions = 0;
    uint32_t exactSubmissions = 0;
    uint32_t fallbackSubmissions = 0;
    uint32_t flags = 0;
    uint32_t storedSubmissions = 0;
    uint32_t matchCacheHit = 0;
    uint32_t matchSegments = 0;
    uint64_t matchUs = 0;
    uint32_t fences[NativeAdrenoSurfaceProfileSubmissionCapacity]{};
    uint32_t sequences[NativeAdrenoSurfaceProfileSubmissionCapacity]{};
};

/* The caller supplies the prepared source descriptor while its read fence is
 * held. The first encounter compares full physical extents against imported
 * writers; later captures use retained descriptor identity and BO generations.
 * Cache eviction or the last matching BO unmap releases that descriptor.
 * A busy GPU lock returns timeout without affecting presentation readiness. */
extern "C" IOReturn native_adreno_surface_profile_capture(IOMemoryDescriptor *,
    uint32_t captured_fence, uint64_t composition_start_continuous_us,
    NativeAdrenoSurfaceProfile *);

/* The display captures once after all producer submissions and polls that same
 * value until completion. Each call tries the GPU lock once and returns timeout
 * if it is busy. Capture returns zero when the GPU is unpublished. The caller
 * must keep the surface's read fence active throughout capture, polling and
 * scanout. A failed capture has no usable fence value. */
extern "C" IOReturn native_adreno_capture_surface_producer(uint32_t *captured_fence);

/* Polling performs one hardware observation without sleeping. A completed fence
 * still retires completed jobs and orders imported memory before success;
 * this CPU work is not bounded by a zero hardware-wait timeout. */
extern "C" IOReturn native_adreno_poll_surface_producer(uint32_t captured_fence);

/* The display caller must receive the surface after its producer has submitted
 * every command buffer, and retain it without further writes until scanout ends.
 * This call captures the shared ring's newest fence, waits for that fence, and
 * retires completed imports with an ordering barrier. It may sleep without
 * holding the GPU lock. The display owns CPU-write publication and scanout.
 * An unpublished GPU succeeds with fence zero. Faults and timeouts return an
 * error; the caller must keep the previous scanout and the pending surface. */
extern "C" IOReturn native_adreno_wait_surface_producer(uint32_t timeout_us,
    uint32_t *captured_fence);

#endif
