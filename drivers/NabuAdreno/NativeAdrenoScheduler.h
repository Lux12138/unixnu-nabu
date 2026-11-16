/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SCHEDULER_H
#define NATIVE_ADRENO_SCHEDULER_H
#include "NativeAdrenoSubmit.h"
#include <libkern/tree.h>

class IOMemoryDescriptor;
struct NativeAdrenoSurfaceBacking;

struct NativeAdrenoSchedulerSample {
    uint64_t completion=0,acceptedUs=0,issuedUs=0,completedUs=0;
    uint64_t aliasQueries=0,aliasHits=0;
    uint32_t queue=0,hardware=0,resources=0,waits=0,signals=0;
    uint32_t gpuDependencies=0,dpuReaders=0,status=0;
};

/* NativeAdrenoScheduler keeps accepted jobs, timeline points and shared-memory
 * accesses until their concrete consumers finish. It copies validated IBs and
 * ranges, retains their mappings and queues each job behind its queue's prior
 * work. A dependency on this ring becomes runnable when its producer is issued;
 * host signals and DPU readers must finish before dependent work is issued.
 *
 * NativeAdrenoSubmit owns the eight hardware jobs and their PM4 fences. The
 * publication service serializes this scheduler with VM changes, sleeps CPU
 * waiters and invokes display callbacks after dropping that lock. Faulted GPU
 * jobs retain their mappings until the service confirms that DMA has stopped. */
class NativeAdrenoScheduler final {
public:
    struct Reservation {
        uint64_t handle = 0;
        size_t first = 0, end = 0, subtreeEnd = 0;
        uint32_t owner = 0;
        bool display = false;
        Reservation *ownedNext = nullptr;
        RB_ENTRY(Reservation) link;
        void augment();
    };
private:
    struct AccessTree {
        RB_HEAD(Root, Reservation) root{};
        void augment(Reservation *);
        static int compare(Reservation *, Reservation *);
#pragma push_macro("RB_AUGMENT")
#undef RB_AUGMENT
#define RB_AUGMENT(node) augment(node)
        RB_GENERATE(Root, Reservation, link, compare)
#pragma pop_macro("RB_AUGMENT")
    };
    struct Job;
    struct Point;
    struct Sync {
        uint64_t handle = 0, value = 0;
        uint32_t owner = 0, references = 0;
        bool timeline = false, leased = false;
        Point *pending = nullptr;
    };
    struct Point {
        Sync *sync = nullptr;
        uint64_t value = 0;
        Job *job = nullptr;
        Point *next = nullptr;
    };
    struct Job {
        uint64_t completion = 0;
        uint64_t producers = 0, readers = 0;
        uint32_t queue = 0, hardware = 0, fallback = 0;
        NativeAdrenoSubmitBuffer *buffers = nullptr;
        NativeAdrenoSubmitResource *resources = nullptr;
        Reservation *accesses = nullptr;
        size_t bufferCount = 0, resourceCount = 0;
        Point *waits = nullptr, *signals = nullptr;
        uint32_t waitCount = 0, signalCount = 0;
        uint32_t waitCapacity = 0, signalCapacity = 0;
        Job *next = nullptr;
        bool accepted = false;
    };
    struct Query {
        NativeAdrenoScheduler *scheduler;
        uint64_t *producers, *readers;
        bool writes;
    };
    NativeAdrenoVm *vm_ = nullptr;
    NativeAdrenoSubmit *submit_ = nullptr;
    NativeAdrenoRing *ring_ = nullptr;
    AccessTree *access_ = nullptr;
    Sync *syncs_ = nullptr;
    Job jobs_[NativeAdrenoAsyncMaxJobs]{};
    Job *heads_[256]{}, *tails_[256]{};
    NativeAdrenoSurfaceRead *reads_[64]{};
    NativeAdrenoSurfaceBacking *backings_[64]{};
    uint64_t nextCompletion_ = 1, nextSync_ = 1;
    uint64_t epoch_ = 1;
    uint32_t liveJobs_ = 0;
    IOReturn failure_ = kIOReturnSuccess;
    bool advancing_ = false;
    NativeAdrenoSchedulerSample samples_[256]{};
    Sync *sync(uint64_t, uint32_t, bool leased = true);
    void putSync(Sync *);
    bool pointReady(const Point &, bool pending) const;
    bool dependenciesReady(const Job &) const;
    void destroyJob(Job &, bool completed);
    void finishJob(Job &);
    void insert(Reservation &);
    void remove(Reservation &);
    static void findConflicts(const Reservation *, size_t, size_t, Query &);
    static bool queryAlias(void *, uint64_t, size_t, size_t);
    static bool addSurfaceAlias(void *, uint64_t, size_t, size_t);
    IOReturn addReadRange(NativeAdrenoSurfaceRead *, uint64_t, size_t, size_t);
    IOReturn addBackingRange(NativeAdrenoSurfaceBacking *,uint64_t,size_t,size_t);
    IOReturn acquireBacking(IOMemoryDescriptor *,NativeAdrenoSurfaceBacking **);
    void releaseBacking(NativeAdrenoSurfaceBacking *);
public:
    IOReturn attach(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *);
    IOReturn detach();
    IOReturn createSync(uint32_t, const NativeAdrenoAbiSyncCreate &, uint64_t *);
    IOReturn releaseSync(uint32_t, uint64_t);
    IOReturn signalSync(uint32_t, const NativeAdrenoAbiSyncPoint &);
    IOReturn querySync(uint32_t, uint64_t, uint64_t *);
    IOReturn waitSyncs(uint32_t, const NativeAdrenoAbiSyncWait &);
    IOReturn accept(uint32_t owner, uint32_t queue,
        const NativeAdrenoSubmitBuffer *, size_t,
        const NativeAdrenoSubmitResource *, size_t, uint32_t fallback,
        const NativeAdrenoAbiSyncPoint *, uint32_t,
        const NativeAdrenoAbiSyncPoint *, uint32_t, uint64_t *);
    IOReturn advance();
    IOReturn submitLegacy(uint32_t queue,const NativeAdrenoSubmitBuffer *,size_t,
        const NativeAdrenoSubmitResource *,size_t,uint32_t,uint32_t *);
    IOReturn completion(uint64_t) const;
    bool capacity() const;
    bool rangeOutstanding(uint32_t, uint32_t) const;
    void closeOwner(uint32_t);
    void fault(IOReturn, bool dmaStopped);
    uint64_t epoch() const {
        return epoch_;
    }
    IOReturn acquireRead(IOMemoryDescriptor *, void (*)(void *, IOReturn),
        void *, NativeAdrenoSurfaceRead **);
    IOReturn readStatus(NativeAdrenoSurfaceRead *, uint32_t *) const;
    void cancelRead(NativeAdrenoSurfaceRead *);
    bool callbackRunning(NativeAdrenoSurfaceRead *) const;
    void releaseRead(NativeAdrenoSurfaceRead *);
    NativeAdrenoSurfaceRead *takeCallback();
    static void invokeCallback(NativeAdrenoSurfaceRead *);
    void finishCallback(NativeAdrenoSurfaceRead *);
    IOReturn mappingAdded(uint64_t);
    void mappingRemoved(uint64_t);
    void copySamples(NativeAdrenoSchedulerSample *) const;
};
#endif
