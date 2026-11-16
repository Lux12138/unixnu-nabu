/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GPU_PROFILE_H
#define NATIVE_ADRENO_GPU_PROFILE_H

#include "NativeAdrenoRing.h"

/* NativeAdrenoGpuProfileAccess connects the recorder to the service worker's
 * GMU ownership. acquire waits for the performance-counter OOB acknowledgement;
 * release clears that request after every registered job has retired. */
struct NativeAdrenoGpuProfileAccess {
    IOReturn (*acquire)(void *);
    IOReturn (*release)(void *);
    bool (*accessible)(void *);
};

/* NativeAdrenoGpuProfile retains GPU register samples and completed job
 * timestamps for one bounded USB-requested window. The service configures its
 * counters while CP is stopped and calls poll from its existing worker. USB
 * requests a window; that worker acquires GMU permission before enabling jobs
 * or sampling, then retains permission until sampling and pending jobs finish.
 * The ring records CP arrival and RB completion in its coherent fence allocation;
 * the submission owner delivers those values after observing the normal fence.
 * USB exports the fixed arrays after sampling and pending jobs have finished. */
class NativeAdrenoGpuProfile final {
public:
    static IOReturn initialize(const NativeAdrenoRingIo *, const NativeAdrenoGpuProfileAccess &);
    static IOReturn poll();
    static void fault();
    /* The service calls this only after GMU reset has revoked hardware access. */
    static void hardwareStopped();
    static bool active();
    static uint32_t beginJob();
    static void finishJob(uint32_t window, uint32_t fence, uint32_t queue,
        uint32_t issue, uint32_t cp, uint32_t rb, bool valid, bool issueValid);
};

extern "C" uint32_t native_adreno_gpu_profile_start(uint32_t duration_ms);
extern "C" void native_adreno_gpu_profile_stop(void);
extern "C" void native_adreno_gpu_profile_dump(void);

#endif
