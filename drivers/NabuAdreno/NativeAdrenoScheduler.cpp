/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoScheduler.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
extern "C" uint64_t (*native_adreno_vm_now_us)(void);

static uint64_t schedulerNow()
{
    return native_adreno_vm_now_us?native_adreno_vm_now_us():0;
}

/* NativeAdrenoSurfaceRead represents one retained DPU use of a descriptor.
 * Physical segments are examined when this read or a GPU mapping is created.
 * Reservation records then connect subsequent GPU writes to this reader. */
struct NativeAdrenoSurfaceRead {
    NativeAdrenoScheduler *scheduler = nullptr;
    NativeAdrenoSurfaceBacking *backing = nullptr;
    NativeAdrenoScheduler::Reservation *accesses = nullptr;
    NativeAdrenoScheduler::Reservation *storage = nullptr;
    uint32_t storageCount=0,storageUsed=0;
    uint64_t writers = 0;
    uint32_t slot = 0, latestHardware = 0, accessCount = 0;
    IOReturn status = kIOReturnNotReady;
    void (*callback)(void *, IOReturn) = nullptr;
    void *context = nullptr;
    bool notify = false, running = false, cancelled = false;
};

/* NativeAdrenoSurfaceBacking pins one descriptor's physical allocation and
 * stores the matching GPU mapping generations and byte intervals. Active DPU
 * reads borrow these records; map and unmap callbacks maintain them. */
struct NativeAdrenoSurfaceBacking {
    struct Segment {
        uint64_t physical;
        size_t bytes;
    };
    struct Range {
        uint64_t handle;
        size_t first,bytes;
        Range *next;
    };
    NativeAdrenoScheduler *scheduler=nullptr;
    IOMemoryDescriptor *memory=nullptr;
    Segment *segments=nullptr;
    Range *ranges=nullptr;
    size_t segmentCount=0,rangeCount=0;
    uint32_t slot=0,readers=0;
    IOReturn status=kIOReturnSuccess;
    bool prepared=false;
};

void NativeAdrenoScheduler::Reservation::augment()
{
    subtreeEnd = end;
    const Reservation *left = RB_LEFT(this, link);
    const Reservation *right = RB_RIGHT(this, link);
    if (left && left->subtreeEnd > subtreeEnd) {
        subtreeEnd = left->subtreeEnd;
    }
    if (right && right->subtreeEnd > subtreeEnd) {
        subtreeEnd = right->subtreeEnd;
    }
}

void NativeAdrenoScheduler::AccessTree::augment(Reservation *record)
{
    while (record) {
        record->augment();
        record = Root_RB_GETPARENT(record);
    }
}

int NativeAdrenoScheduler::AccessTree::compare(Reservation *left, Reservation *right)
{
    if (left->first != right->first) {
        return left->first < right->first ? -1 : 1;
    }
    if (left == right) {
        return 0;
    }
    return uintptr_t(left) < uintptr_t(right) ? -1 : 1;
}

IOReturn NativeAdrenoScheduler::attach(NativeAdrenoVm *vm,
    NativeAdrenoSubmit *submit, NativeAdrenoRing *ring)
{
    access_ = IONewData(AccessTree, NativeAdrenoVmMaxObjects);
    syncs_ = IONewData(Sync, NativeAdrenoAsyncMaxSyncs);
    if (!access_ || !syncs_) {
        if (access_) {
            IODeleteData(access_, AccessTree, NativeAdrenoVmMaxObjects);
        }
        if (syncs_) {
            IODeleteData(syncs_, Sync, NativeAdrenoAsyncMaxSyncs);
        }
        access_ = nullptr;
        syncs_ = nullptr;
        return kIOReturnNoMemory;
    }
    memset(access_, 0, sizeof(AccessTree) * NativeAdrenoVmMaxObjects);
    memset(syncs_, 0, sizeof(Sync) * NativeAdrenoAsyncMaxSyncs);
    vm_ = vm;
    submit_ = submit;
    ring_ = ring;
    vm_->setMappingObserver([](void *context, uint64_t handle) -> IOReturn {
        return static_cast<NativeAdrenoScheduler *>(context)->mappingAdded(handle);
    },[](void *context,uint64_t handle) {
        static_cast<NativeAdrenoScheduler *>(context)->mappingRemoved(handle);
    }, this);
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoScheduler::detach()
{
    if (liveJobs_) {
        return kIOReturnBusy;
    }
    for (NativeAdrenoSurfaceRead *read : reads_) {
        if (read) {
            return kIOReturnBusy;
        }
    }
    vm_->setMappingObserver(nullptr, nullptr, nullptr);
    for(NativeAdrenoSurfaceBacking *backing:backings_) {
        if(backing) {
            releaseBacking(backing);
        }
    }
    IODeleteData(access_, AccessTree, NativeAdrenoVmMaxObjects);
    IODeleteData(syncs_, Sync, NativeAdrenoAsyncMaxSyncs);
    access_ = nullptr;
    syncs_ = nullptr;
    return kIOReturnSuccess;
}

NativeAdrenoScheduler::Sync *NativeAdrenoScheduler::sync(uint64_t handle,
    uint32_t owner, bool leased)
{
    Sync *object = &syncs_[handle & (NativeAdrenoAsyncMaxSyncs - 1)];
    if (!handle || object->handle != handle || object->owner != owner ||
        (leased && !object->leased)) {
        return nullptr;
    }
    return object;
}

void NativeAdrenoScheduler::putSync(Sync *object)
{
    if (object && !--object->references) {
        *object = {};
    }
}

IOReturn NativeAdrenoScheduler::createSync(uint32_t owner,
    const NativeAdrenoAbiSyncCreate &request, uint64_t *handle)
{
    if (!handle || request.reserved || (request.flags & ~uint32_t(NativeAdrenoAsyncTimeline)) ||
        (!(request.flags & NativeAdrenoAsyncTimeline) && request.initial_value > 1)) {
        return kIOReturnBadArgument;
    }
    if (failure_) {
        return failure_;
    }
    for (uint32_t at = 0; at < NativeAdrenoAsyncMaxSyncs; ++at) {
        Sync &object = syncs_[at];
        if (object.references) {
            continue;
        }
        if (nextSync_ > (UINT64_MAX >> 12)) {
            return kIOReturnNoResources;
        }
        object.handle = (nextSync_++ << 12) | at;
        object.owner = owner;
        object.references = 1;
        object.leased = true;
        object.timeline = (request.flags & NativeAdrenoAsyncTimeline) != 0;
        object.value = request.initial_value;
        *handle = object.handle;
        return kIOReturnSuccess;
    }
    return kIOReturnNoResources;
}

IOReturn NativeAdrenoScheduler::releaseSync(uint32_t owner, uint64_t handle)
{
    Sync *object = sync(handle, owner);
    if (!object) {
        return kIOReturnBadArgument;
    }
    object->leased = false;
    putSync(object);
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoScheduler::signalSync(uint32_t owner,
    const NativeAdrenoAbiSyncPoint &point)
{
    Sync *object = sync(point.handle, owner);
    if (!object || !point.value || (!object->timeline && point.value != 1) ||
        point.value < object->value) {
        return kIOReturnBadArgument;
    }
    object->value = point.value;
    return advance();
}

IOReturn NativeAdrenoScheduler::querySync(uint32_t owner, uint64_t handle, uint64_t *value)
{
    Sync *object = sync(handle, owner);
    if (!object || !value) {
        return kIOReturnBadArgument;
    }
    if (failure_) {
        return failure_;
    }
    *value = object->value;
    return kIOReturnSuccess;
}

bool NativeAdrenoScheduler::pointReady(const Point &point, bool pending) const
{
    if (point.sync->value >= point.value) {
        return true;
    }
    for (const Point *producer = point.sync->pending; producer; producer = producer->next) {
        if (producer->value >= point.value && (pending || producer->job->hardware)) {
            return true;
        }
    }
    return false;
}

IOReturn NativeAdrenoScheduler::waitSyncs(uint32_t owner,
    const NativeAdrenoAbiSyncWait &request)
{
    if (request.version != NativeAdrenoAsyncVersion || request.epoch != epoch_ ||
        request.reserved || request.count > NativeAdrenoAsyncMaxWaits ||
        (request.flags & ~uint32_t(NativeAdrenoAsyncWaitAny | NativeAdrenoAsyncWaitPending))) {
        return kIOReturnBadArgument;
    }
    if (failure_) {
        return failure_;
    }
    bool ready = !request.count || !(request.flags & NativeAdrenoAsyncWaitAny);
    for (uint32_t at = 0; at < request.count; ++at) {
        Sync *object = sync(request.points[at].handle, owner);
        if (!object || (!object->timeline && request.points[at].value != 1)) {
            return kIOReturnBadArgument;
        }
        const bool passed = (request.flags & NativeAdrenoAsyncWaitPending) ?
            pointReady({object, request.points[at].value}, true) :
            object->value >= request.points[at].value;
        ready = (request.flags & NativeAdrenoAsyncWaitAny) ? ready || passed : ready && passed;
    }
    return ready ? kIOReturnSuccess : kIOReturnTimeout;
}

void NativeAdrenoScheduler::insert(Reservation &access)
{
    AccessTree &tree = access_[access.handle & (NativeAdrenoVmMaxObjects - 1)];
    access.subtreeEnd = access.end;
    tree.Root_RB_INSERT(&tree.root, &access);
}

void NativeAdrenoScheduler::remove(Reservation &access)
{
    AccessTree &tree = access_[access.handle & (NativeAdrenoVmMaxObjects - 1)];
    tree.Root_RB_REMOVE(&tree.root, &access);
}

void NativeAdrenoScheduler::findConflicts(const Reservation *access,
    size_t first, size_t end, Query &query)
{
    if (!access || access->subtreeEnd <= first) {
        return;
    }
    findConflicts(RB_LEFT(access, link), first, end, query);
    if (access->first >= end) {
        return;
    }
    if (access->end > first) {
        if (access->display) {
            if (query.writes) {
                *query.readers |= UINT64_C(1) << access->owner;
            }
        } else {
            const Job &job = query.scheduler->jobs_[access->owner];
            const size_t index = size_t(access - job.accesses);
            if (query.writes || (job.resources[index].access & NativeAdrenoResourceWrite)) {
                *query.producers |= UINT64_C(1) << access->owner;
            }
        }
    }
    findConflicts(RB_RIGHT(access, link), first, end, query);
}

bool NativeAdrenoScheduler::queryAlias(void *context, uint64_t handle,
    size_t first, size_t bytes)
{
    Query &query = *static_cast<Query *>(context);
    const AccessTree &tree = query.scheduler->access_[handle & (NativeAdrenoVmMaxObjects - 1)];
    findConflicts(RB_ROOT(&tree.root), first, first + bytes, query);
    return false;
}

bool NativeAdrenoScheduler::dependenciesReady(const Job &job) const
{
    if (job.readers) {
        return false;
    }
    uint64_t producers = job.producers;
    while (producers) {
        const uint32_t at = uint32_t(__builtin_ctzll(producers));
        producers &= producers - 1;
        if (jobs_[at].accepted && !jobs_[at].hardware) {
            return false;
        }
    }
    for (uint32_t at = 0; at < job.waitCount; ++at) {
        if (!pointReady(job.waits[at], false)) {
            return false;
        }
    }
    return true;
}

IOReturn NativeAdrenoScheduler::accept(uint32_t owner, uint32_t queue,
    const NativeAdrenoSubmitBuffer *buffers, size_t bufferCount,
    const NativeAdrenoSubmitResource *resources, size_t resourceCount, uint32_t fallback,
    const NativeAdrenoAbiSyncPoint *waits, uint32_t waitCount,
    const NativeAdrenoAbiSyncPoint *signals, uint32_t signalCount, uint64_t *completion)
{
    if (!completion || queue >= 256 || bufferCount > NativeAdrenoSubmitMaxIbs ||
        resourceCount > NativeAdrenoSubmitMaxResources || waitCount > NativeAdrenoAsyncMaxWaits ||
        signalCount > NativeAdrenoAsyncMaxSignals) {
        return kIOReturnBadArgument;
    }
    if (failure_) {
        return failure_;
    }
    /* advance() faults the whole scheduler when submitResources() refuses a buffer, so refuse the
     * same range checks here first: one bad client IB must stay that client's argument error. */
    if (bufferCount && !vm_) {
        return kIOReturnNotReady;
    }
    for (size_t at = 0; at < bufferCount; ++at) {
        uint64_t iova = 0;
        size_t bytes = 0;
        unsigned permissions = 0;
        const NativeAdrenoSubmitBuffer &buffer = buffers[at];
        if (vm_->describe(buffer.handle, &iova, &bytes, &permissions) || !buffer.bytes || (buffer.bytes & 3) ||
            buffer.bytes > 0xfffffu * 4u || !(permissions & ArmSmmu4KRead) || buffer.iova < iova ||
            buffer.iova - iova > bytes || buffer.bytes > bytes - (buffer.iova - iova)) {
            return kIOReturnBadArgument;
        }
    }
    if(nextCompletion_ > INT32_MAX) {
        return kIOReturnNoResources;
    }
    if (!capacity()) {
        return kIOReturnBusy;
    }
    Job *job = nullptr;
    for (Job &candidate : jobs_) {
        if (!candidate.accepted) {
            job = &candidate;
            break;
        }
    }
    if (!job) {
        return kIOReturnBusy;
    }
    job->buffers = bufferCount ? IONewData(NativeAdrenoSubmitBuffer, bufferCount) : nullptr;
    job->resources = resourceCount ? IONewData(NativeAdrenoSubmitResource, resourceCount) : nullptr;
    job->accesses = resourceCount ? IONewData(Reservation, resourceCount) : nullptr;
    job->waits = waitCount ? IONewData(Point, waitCount) : nullptr;
    job->signals = signalCount ? IONewData(Point, signalCount) : nullptr;
    job->bufferCount = bufferCount;
    job->resourceCount = resourceCount;
    job->waitCapacity = waitCount;
    job->signalCapacity = signalCount;
    if (job->resources) {
        memset(job->resources, 0, resourceCount * sizeof(*resources));
    }
    if ((bufferCount && !job->buffers) ||
        (resourceCount && (!job->resources || !job->accesses)) ||
        (waitCount && !job->waits) || (signalCount && !job->signals)) {
        destroyJob(*job, false);
        return kIOReturnNoMemory;
    }
    if (bufferCount) {
        memcpy(job->buffers, buffers, bufferCount * sizeof(*buffers));
    }
    if (resourceCount) {
        memcpy(job->resources, resources, resourceCount * sizeof(*resources));
        memset(job->accesses, 0, resourceCount * sizeof(Reservation));
        for (size_t at = 0; at < resourceCount; ++at) {
            job->resources[at].owns_reference = false;
        }
    }
    IOReturn status = kIOReturnSuccess;
    for (uint32_t which = 0; which < 2 && !status; ++which) {
        const NativeAdrenoAbiSyncPoint *input = which ? signals : waits;
        const uint32_t count = which ? signalCount : waitCount;
        Point *output = which ? job->signals : job->waits;
        for (uint32_t at = 0; at < count; ++at) {
            Sync *object = sync(input[at].handle, owner);
            if (!object || (!object->timeline && input[at].value != 1) ||
                (which && (!input[at].value || input[at].value <= object->value))) {
                status = kIOReturnBadArgument;
                break;
            }
            output[at] = {object, input[at].value};
            if(which && object->timeline) {
                for(Point *prior=object->pending;prior;prior=prior->next) {
                    if(prior->value>=input[at].value) {
                        status=kIOReturnBadArgument;
                        break;
                    }
                    job->producers|=UINT64_C(1)<<size_t(prior->job-jobs_);
                }
                if(status) {
                    break;
                }
            }
            ++object->references;
            if (which) {
                ++job->signalCount;
            } else {
                ++job->waitCount;
            }
        }
    }
    uint64_t queries = 0, hits = 0;
    for (size_t at = 0; at < resourceCount && !status; ++at) {
        NativeAdrenoSubmitResource &resource = job->resources[at];
        Query query{this, &job->producers, &job->readers,
            bool(resource.access & NativeAdrenoResourceWrite)};
        status = vm_->visitResourceAliases(resource.handle, resource.offset,
            resource.bytes, 1, queryAlias, &query, &queries, &hits);
        if (!status) {
            status = vm_->holdJob(resource.handle);
            resource.owns_reference = !status;
        }
    }
    if (status) {
        destroyJob(*job, false);
        return status;
    }

    job->queue = queue;
    job->fallback = fallback;
    job->completion = nextCompletion_++;
    NativeAdrenoSchedulerSample &sample=samples_[(job->completion-1)%256];
    sample={};
    sample.completion=job->completion;
    sample.acceptedUs=schedulerNow();
    sample.aliasQueries=queries;
    sample.aliasHits=hits;
    sample.queue=queue;
    sample.resources=uint32_t(resourceCount);
    sample.waits=waitCount;
    sample.signals=signalCount;
    sample.gpuDependencies=uint32_t(__builtin_popcountll(job->producers));
    sample.dpuReaders=uint32_t(__builtin_popcountll(job->readers));
    job->accepted = true;
    ++liveJobs_;
    for (uint32_t at = 0; at < job->signalCount; ++at) {
        Point &signal = job->signals[at];
        signal.job = job;
        signal.next = signal.sync->pending;
        signal.sync->pending = &signal;
    }
    for (size_t at = 0; at < resourceCount; ++at) {
        const NativeAdrenoSubmitResource &resource = job->resources[at];
        Reservation &access = job->accesses[at];
        access.handle = resource.handle;
        access.first = resource.offset;
        access.end = resource.offset + resource.bytes;
        access.owner = uint32_t(job - jobs_);
        insert(access);
    }
    if (tails_[queue]) {
        tails_[queue]->next = job;
    } else {
        heads_[queue] = job;
    }
    tails_[queue] = job;
    *completion = job->completion;
    return advance();
}

void NativeAdrenoScheduler::destroyJob(Job &job, bool completed)
{
    for (size_t at = 0; job.resources && at < job.resourceCount; ++at) {
        if (job.accepted) {
            remove(job.accesses[at]);
        }
        if (job.resources[at].owns_reference) {
            vm_->retireJobAfterCompletion(job.resources[at].handle);
        }
    }
    for (uint32_t at = 0; at < job.waitCount; ++at) {
        putSync(job.waits[at].sync);
    }
    for (uint32_t at = 0; at < job.signalCount; ++at) {
        Point &point = job.signals[at];
        if (job.accepted) {
            Point **link = &point.sync->pending;
            while (*link && *link != &point) {
                link = &(*link)->next;
            }
            if (*link) {
                *link = point.next;
            }
        }
        if (completed && point.value > point.sync->value) {
            point.sync->value = point.value;
        }
        putSync(point.sync);
    }
    if (job.buffers) {
        IODeleteData(job.buffers, NativeAdrenoSubmitBuffer, job.bufferCount);
    }
    if (job.resources) {
        IODeleteData(job.resources, NativeAdrenoSubmitResource, job.resourceCount);
    }
    if (job.accesses) {
        IODeleteData(job.accesses, Reservation, job.resourceCount);
    }
    if (job.waits) {
        IODeleteData(job.waits, Point, job.waitCapacity);
    }
    if (job.signals) {
        IODeleteData(job.signals, Point, job.signalCapacity);
    }
    if (job.accepted) {
        --liveJobs_;
    }
    job = {};
}

void NativeAdrenoScheduler::finishJob(Job &job)
{
    NativeAdrenoSchedulerSample &sample=samples_[(job.completion-1)%256];
    if(sample.completion==job.completion) {
        sample.completedUs=schedulerNow();
    }
    const uint64_t bit = UINT64_C(1) << size_t(&job - jobs_);
    for (Job &other : jobs_) {
        other.producers &= ~bit;
    }
    for (NativeAdrenoSurfaceRead *read : reads_) {
        if (!read || !(read->writers & bit)) {
            continue;
        }
        read->writers &= ~bit;
        if (job.hardware > read->latestHardware) {
            read->latestHardware = job.hardware;
        }
        if (!read->writers && read->status==kIOReturnNotReady) {
            read->status = kIOReturnSuccess;
            read->notify = !read->cancelled;
        }
    }
    destroyJob(job, true);
}

IOReturn NativeAdrenoScheduler::advance()
{
    if (failure_) {
        return failure_;
    }
    if (advancing_) {
        return kIOReturnSuccess;
    }
    advancing_ = true;
    IOReturn status = submit_->retireCompleted();
    if (!status) {
        for (Job &job : jobs_) {
            if (job.accepted && job.hardware &&
                ring_->observeFence(job.hardware, nullptr, 0, nullptr) == NativeAdrenoRingFenceComplete) {
                finishJob(job);
            }
        }
        while (submit_->outstanding() < 8) {
            Job *selected=nullptr;
            for (Job &job : jobs_) {
                if (!job.accepted || job.hardware || heads_[job.queue] != &job ||
                    !dependenciesReady(job)) {
                    continue;
                }
                if(!selected || job.completion<selected->completion) {
                    selected=&job;
                }
            }
            if(!selected) {
                break;
            }
            Job &job=*selected;
            status = submit_->submitResources(job.buffers, job.bufferCount,
                job.resources, job.resourceCount, job.queue, job.fallback, &job.hardware);
            if(status==kIOReturnBusy) {
                status=kIOReturnSuccess;
                break;
            }
            if (status) {
                break;
            }
            heads_[job.queue] = job.next;
            if (!job.next) {
                tails_[job.queue] = nullptr;
            }
            job.next = nullptr;
            NativeAdrenoSchedulerSample &sample=samples_[(job.completion-1)%256];
            if(sample.completion==job.completion) {
                sample.hardware=job.hardware;
                sample.issuedUs=schedulerNow();
            }
        }
    }
    advancing_ = false;
    if (status) {
        fault(status, false);
    }
    return status;
}

IOReturn NativeAdrenoScheduler::submitLegacy(uint32_t queue,
    const NativeAdrenoSubmitBuffer *buffers,size_t bufferCount,
    const NativeAdrenoSubmitResource *resources,size_t resourceCount,
    uint32_t fallback,uint32_t *hardware)
{
    if(!hardware || queue>=256) {
        return kIOReturnBadArgument;
    }
    if(heads_[queue] || submit_->outstanding()>=8) {
        return kIOReturnBusy;
    }
    uint64_t receipt=0;
    const IOReturn status=accept(queue&~uint32_t(NativeAdrenoAbiMaxQueues-1),queue,
        buffers,bufferCount,resources,resourceCount,fallback,nullptr,0,nullptr,0,&receipt);
    if(status) {
        return status;
    }
    for(Job &job:jobs_) {
        if(!job.accepted || job.completion!=receipt) {
            continue;
        }
        if(job.hardware) {
            *hardware=job.hardware;
            return kIOReturnSuccess;
        }
        heads_[queue]=nullptr;
        tails_[queue]=nullptr;
        destroyJob(job,false);
        return kIOReturnBusy;
    }
    return kIOReturnIOError;
}

IOReturn NativeAdrenoScheduler::completion(uint64_t completion) const
{
    if (!completion || completion >= nextCompletion_) {
        return kIOReturnBadArgument;
    }
    if (failure_) {
        return failure_;
    }
    for (const Job &job : jobs_) {
        if (job.accepted && job.completion == completion) {
            return kIOReturnTimeout;
        }
    }
    return kIOReturnSuccess;
}

bool NativeAdrenoScheduler::capacity() const
{
    return liveJobs_ < NativeAdrenoAsyncMaxJobs;
}

bool NativeAdrenoScheduler::rangeOutstanding(uint32_t first, uint32_t count) const
{
    for (const Job &job : jobs_) {
        if (job.accepted && job.queue - first < count) {
            return true;
        }
    }
    return false;
}

void NativeAdrenoScheduler::closeOwner(uint32_t owner)
{
    for(Job &job:jobs_) {
        if(!job.accepted || job.hardware || job.queue-owner>=NativeAdrenoAbiMaxQueues) {
            continue;
        }
        const uint64_t bit=UINT64_C(1)<<size_t(&job-jobs_);
        for(Job &other:jobs_) {
            other.producers&=~bit;
        }
        for(NativeAdrenoSurfaceRead *read:reads_) {
            if(read && (read->writers&bit)) {
                read->writers&=~bit;
                read->status=kIOReturnAborted;
                read->notify=!read->cancelled;
            }
        }
        destroyJob(job,false);
    }
    for(uint32_t queue=owner;queue<owner+NativeAdrenoAbiMaxQueues && queue<256;++queue) {
        heads_[queue]=nullptr;
        tails_[queue]=nullptr;
    }
    for (uint32_t at = 0; at < NativeAdrenoAsyncMaxSyncs; ++at) {
        Sync &object = syncs_[at];
        if (object.references && object.leased && object.owner == owner) {
            object.leased = false;
            putSync(&object);
        }
    }
    advance();
}

void NativeAdrenoScheduler::fault(IOReturn status, bool dmaStopped)
{
    failure_ = status ? status : kIOReturnIOError;
    for(const Job &job:jobs_) {
        if(job.accepted) {
            NativeAdrenoSchedulerSample &sample=samples_[(job.completion-1)%256];
            if(sample.completion==job.completion) {
                sample.status=uint32_t(failure_);
            }
        }
    }
    for (NativeAdrenoSurfaceRead *read : reads_) {
        if (read && !read->cancelled && read->status == kIOReturnNotReady) {
            read->status = failure_;
            read->notify = true;
        }
    }
    if (!dmaStopped) {
        return;
    }
    for (Job &job : jobs_) {
        if (job.accepted) {
            destroyJob(job, false);
        }
    }
    memset(heads_, 0, sizeof(heads_));
    memset(tails_, 0, sizeof(tails_));
}

IOReturn NativeAdrenoScheduler::addReadRange(NativeAdrenoSurfaceRead *read,
    uint64_t handle, size_t first, size_t bytes)
{
    if (read->accessCount == NativeAdrenoAliasRangeLimit) {
        return kIOReturnNoResources;
    }
    const bool stored=read->storageUsed<read->storageCount;
    Reservation *access = stored?&read->storage[read->storageUsed++]:IONew(Reservation, 1);
    if (!access) {
        return kIOReturnNoMemory;
    }
    const IOReturn status = vm_->holdJob(handle);
    if (status) {
        if(!stored) {
            IODelete(access, Reservation, 1);
        }
        return status;
    }
    *access = {};
    access->handle = handle;
    access->first = first;
    access->end = first + bytes;
    access->owner = read->slot;
    access->display = true;
    uint64_t ignored = 0;
    Query query{this, &read->writers, &ignored, false};
    queryAlias(&query, handle, first, bytes);
    insert(*access);
    access->ownedNext = read->accesses;
    read->accesses = access;
    ++read->accessCount;
    return kIOReturnSuccess;
}

bool NativeAdrenoScheduler::addSurfaceAlias(void *context, uint64_t handle,
    size_t first, size_t bytes)
{
    auto *backing=static_cast<NativeAdrenoSurfaceBacking *>(context);
    const IOReturn status=backing->scheduler->addBackingRange(backing,handle,first,bytes);
    if (status) {
        backing->status = status;
    }
    return status != kIOReturnSuccess;
}

IOReturn NativeAdrenoScheduler::addBackingRange(NativeAdrenoSurfaceBacking *backing,
    uint64_t handle,size_t first,size_t bytes)
{
    if(backing->rangeCount==NativeAdrenoAliasRangeLimit) {
        return kIOReturnNoResources;
    }
    auto *range=IONew(NativeAdrenoSurfaceBacking::Range,1);
    if(!range) {
        return kIOReturnNoMemory;
    }
    *range={handle,first,bytes,backing->ranges};
    backing->ranges=range;
    ++backing->rangeCount;
    for(NativeAdrenoSurfaceRead *read:reads_) {
        if(read && read->backing==backing && !read->cancelled) {
            const IOReturn status=addReadRange(read,handle,first,bytes);
            if(status) {
                return status;
            }
        }
    }
    return kIOReturnSuccess;
}

void NativeAdrenoScheduler::releaseBacking(NativeAdrenoSurfaceBacking *backing)
{
    while(backing->ranges) {
        auto *range=backing->ranges;
        backing->ranges=range->next;
        IODelete(range,NativeAdrenoSurfaceBacking::Range,1);
    }
    if(backing->segments) {
        IODeleteData(backing->segments,NativeAdrenoSurfaceBacking::Segment,backing->segmentCount);
    }
    if(backing->prepared) {
        backing->memory->complete(kIODirectionIn);
    }
    backing->memory->release();
    backings_[backing->slot]=nullptr;
    IODelete(backing,NativeAdrenoSurfaceBacking,1);
}

IOReturn NativeAdrenoScheduler::acquireBacking(IOMemoryDescriptor *memory,
    NativeAdrenoSurfaceBacking **out)
{
    uint32_t slot=64;
    for(uint32_t at=0;at<64;++at) {
        if(backings_[at] && backings_[at]->memory==memory) {
            *out=backings_[at];
            return kIOReturnSuccess;
        }
        if(!backings_[at] && slot==64) {
            slot=at;
        }
    }
    if(slot==64) {
        for(uint32_t at=0;at<64;++at) {
            if(!backings_[at]->readers) {
                releaseBacking(backings_[at]);
                slot=at;
                break;
            }
        }
    }
    if(slot==64) {
        return kIOReturnNoResources;
    }
    auto *backing=IONew(NativeAdrenoSurfaceBacking,1);
    if(!backing) {
        return kIOReturnNoMemory;
    }
    *backing={};
    backing->scheduler=this;
    backing->slot=slot;
    backing->memory=memory;
    memory->retain();
    backings_[slot]=backing;
    IOReturn status=memory->prepare(kIODirectionIn);
    backing->prepared=!status;
    for(IOByteCount at=0;!status && at<memory->getLength();) {
        IOByteCount bytes=0;
        const addr64_t physical=memory->getPhysicalSegment(at,&bytes);
        if(!physical || !bytes || bytes>memory->getLength()-at ||
            backing->segmentCount==NativeAdrenoAliasRangeLimit) {
            status=kIOReturnBadArgument;
            break;
        }
        ++backing->segmentCount;
        at+=bytes;
    }
    if(!status) {
        backing->segments=IONewData(NativeAdrenoSurfaceBacking::Segment,backing->segmentCount);
        if(!backing->segments) {
            status=kIOReturnNoMemory;
        }
    }
    IOByteCount offset=0;
    for(size_t at=0;!status && at<backing->segmentCount;++at) {
        IOByteCount bytes=0;
        const addr64_t physical=memory->getPhysicalSegment(offset,&bytes);
        if(!physical || !bytes || bytes>memory->getLength()-offset) {
            status=kIOReturnBadArgument;
            break;
        }
        backing->segments[at]={physical,size_t(bytes)};
        status=vm_->visitPhysicalAliases(physical,size_t(bytes),addSurfaceAlias,backing);
        offset+=bytes;
    }
    if(status) {
        releaseBacking(backing);
        return status;
    }
    *out=backing;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoScheduler::acquireRead(IOMemoryDescriptor *memory,
    void (*callback)(void *, IOReturn), void *context, NativeAdrenoSurfaceRead **out)
{
    if (!memory || !out) {
        return kIOReturnBadArgument;
    }
    *out = nullptr;
    if (failure_) {
        return failure_;
    }
    uint32_t slot = 0;
    while (slot < 64 && reads_[slot]) {
        ++slot;
    }
    if (slot == 64) {
        return kIOReturnNoResources;
    }
    NativeAdrenoSurfaceBacking *backing=nullptr;
    IOReturn status=acquireBacking(memory,&backing);
    if(status) {
        return status;
    }
    NativeAdrenoSurfaceRead *read = IONew(NativeAdrenoSurfaceRead, 1);
    if (!read) {
        return kIOReturnNoMemory;
    }
    *read = {};
    read->scheduler = this;
    read->slot = slot;
    read->backing = backing;
    read->callback = callback;
    read->context = context;
    read->status = kIOReturnNotReady;
    read->storageCount=uint32_t(backing->rangeCount);
    read->storage=read->storageCount?IONewData(Reservation,read->storageCount):nullptr;
    if(read->storageCount && !read->storage) {
        IODelete(read,NativeAdrenoSurfaceRead,1);
        return kIOReturnNoMemory;
    }
    ++backing->readers;
    reads_[slot] = read;
    for(auto *range=backing->ranges;range && !status;range=range->next) {
        status=addReadRange(read,range->handle,range->first,range->bytes);
    }
    if (status) {
        releaseRead(read);
        return status;
    }
    read->status = read->writers ? kIOReturnNotReady : kIOReturnSuccess;
    *out = read;
    return read->status;
}

IOReturn NativeAdrenoScheduler::mappingAdded(uint64_t handle)
{
    if (!vm_->resourceBacking(handle)) {
        return kIOReturnBadArgument;
    }
    for (NativeAdrenoSurfaceBacking *backing : backings_) {
        if (!backing) {
            continue;
        }
        for(size_t at=0;at<backing->segmentCount;++at) {
            const auto &segment=backing->segments[at];
            struct AddedMapping {
                NativeAdrenoScheduler *scheduler;
                NativeAdrenoSurfaceBacking *backing;
                uint64_t handle;
                IOReturn status;
            } query{this,backing,handle,kIOReturnSuccess};
            const IOReturn status=vm_->visitPhysicalAliases(segment.physical,segment.bytes,
                [](void *context,uint64_t mapped,size_t offset,size_t length)->bool {
                    auto &query=*static_cast<AddedMapping *>(context);
                    if(mapped==query.handle) {
                        query.status=query.scheduler->addBackingRange(query.backing,mapped,offset,length);
                    }
                    return query.status!=kIOReturnSuccess;
                },&query);
            if(status) {
                return query.status?query.status:status;
            }
        }
    }
    return kIOReturnSuccess;
}

void NativeAdrenoScheduler::mappingRemoved(uint64_t handle)
{
    for(NativeAdrenoSurfaceBacking *backing:backings_) {
        if(!backing) {
            continue;
        }
        auto **link=&backing->ranges;
        while(*link) {
            auto *range=*link;
            if(range->handle!=handle) {
                link=&range->next;
                continue;
            }
            *link=range->next;
            --backing->rangeCount;
            IODelete(range,NativeAdrenoSurfaceBacking::Range,1);
        }
        if(!backing->readers && !backing->rangeCount) {
            releaseBacking(backing);
        }
    }
}

IOReturn NativeAdrenoScheduler::readStatus(NativeAdrenoSurfaceRead *read,
    uint32_t *latestHardware) const
{
    if (!read || read->slot >= 64 || reads_[read->slot] != read || read->cancelled) {
        return kIOReturnBadArgument;
    }
    if (latestHardware) {
        *latestHardware = read->latestHardware;
        uint64_t writers = read->writers;
        while (writers) {
            const uint32_t at = uint32_t(__builtin_ctzll(writers));
            writers &= writers - 1;
            if (jobs_[at].hardware > *latestHardware) {
                *latestHardware = jobs_[at].hardware;
            }
        }
    }
    return read->status;
}

void NativeAdrenoScheduler::cancelRead(NativeAdrenoSurfaceRead *read)
{
    read->cancelled = true;
    read->notify = false;
}

bool NativeAdrenoScheduler::callbackRunning(NativeAdrenoSurfaceRead *read) const
{
    return read->running;
}

void NativeAdrenoScheduler::releaseRead(NativeAdrenoSurfaceRead *read)
{
    cancelRead(read);
    const uint64_t bit = UINT64_C(1) << read->slot;
    for (Job &job : jobs_) {
        job.readers &= ~bit;
    }
    while (read->accesses) {
        Reservation *access = read->accesses;
        read->accesses = access->ownedNext;
        remove(*access);
        vm_->retireJobAfterCompletion(access->handle);
        const uintptr_t address=reinterpret_cast<uintptr_t>(access);
        const uintptr_t start=reinterpret_cast<uintptr_t>(read->storage);
        if(!read->storage || address<start || address>=start+read->storageCount*sizeof(Reservation)) {
            IODelete(access, Reservation, 1);
        }
    }
    if(read->storage) {
        IODeleteData(read->storage,Reservation,read->storageCount);
    }
    NativeAdrenoSurfaceBacking *backing=read->backing;
    --backing->readers;
    if(!backing->readers && !backing->rangeCount) {
        releaseBacking(backing);
    }
    reads_[read->slot] = nullptr;
    IODelete(read, NativeAdrenoSurfaceRead, 1);
    advance();
}

NativeAdrenoSurfaceRead *NativeAdrenoScheduler::takeCallback()
{
    for (NativeAdrenoSurfaceRead *read : reads_) {
        if (read && read->notify && !read->cancelled && !read->running) {
            read->notify = false;
            read->running = true;
            return read;
        }
    }
    return nullptr;
}

void NativeAdrenoScheduler::invokeCallback(NativeAdrenoSurfaceRead *read)
{
    if (read->callback) {
        read->callback(read->context, read->status);
    }
}

void NativeAdrenoScheduler::finishCallback(NativeAdrenoSurfaceRead *read)
{
    read->running = false;
}

void NativeAdrenoScheduler::copySamples(NativeAdrenoSchedulerSample *output) const
{
    memcpy(output,samples_,sizeof(samples_));
}
