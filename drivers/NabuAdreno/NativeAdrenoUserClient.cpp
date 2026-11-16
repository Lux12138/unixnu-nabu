/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoScheduler.h"
extern "C" volatile uint32_t native_adreno_trace_stage;
/* 0 keeps user BOs cacheable and maintained before every submission (int-070..097); the board sets it from the sheng-gpu-wc boot argument. */
/* The board diagnostic ABI retains its inactive WC control field. */
extern "C" {volatile uint32_t native_adreno_wc_enabled;}
/* BoImport record: [0] calls, [1] refused, [2..5] last refusal (status, stage, bytes, address), [6..9] first refusal. Stages 1..3 are this file's
 * checks, 10..19 NativeAdrenoMemory::importTask's. */
extern "C" {volatile uint64_t native_adreno_import_diag[10];}
extern "C" volatile uint32_t native_adreno_import_stage;
/* Imports and their release (int-233/234: Core Animation's render thread waited in vm_deallocate for pages a kernel wiring held): [0] imports
 * alive, [1] frees of an import refused, [2] last refusal status; then the 8 newest imports as {address, pid<<40|bytes, handle, state}, state
 * 1 alive, 2 released, 3 free refused. */
extern "C" {volatile uint64_t native_adreno_import_live[3+8*4];}
extern "C" int proc_selfpid(void);
static unsigned gImportNext;
static void noteImportState(uint64_t handle,uint64_t state)
{
    for(unsigned i=0;i<8;++i)if(native_adreno_import_live[3+i*4+2]==handle) {native_adreno_import_live[3+i*4+3]=state;return;}
}
/* Per kernel method: [method][0] calls, [1] calls that returned non-zero, [2] last status, [3] total time (us, 32-bit). Method numbers are the ABI's (1..16). */
extern "C" {extern uint64_t (*native_adreno_vm_now_us)(void);volatile uint32_t native_adreno_method_stats[20][4];volatile uint32_t native_adreno_map_fail[7][2];}
extern "C" void native_adreno_profile_map(uint32_t pages,uint64_t us);
static void noteImport(IOReturn status,uint32_t stage,uint64_t bytes,uint64_t address)
{
    if(!status)return;
    native_adreno_import_diag[1]=native_adreno_import_diag[1]+1;
    native_adreno_import_diag[2]=(uint32_t)status;native_adreno_import_diag[3]=stage;native_adreno_import_diag[4]=bytes;native_adreno_import_diag[5]=address;
    if(native_adreno_import_diag[1]==1) {
        native_adreno_import_diag[6]=(uint32_t)status;native_adreno_import_diag[7]=stage;native_adreno_import_diag[8]=bytes;native_adreno_import_diag[9]=address;
    }
}
#include <IOKit/IOLib.h>
#include <string.h>
extern "C" {volatile uint32_t native_adreno_window_overflow[4][3];}   /* {count, last size KiB, last end MiB} per client window */
namespace {
constexpr unsigned HandleIndexBits=13;
constexpr uint64_t HandleIndexMask=(UINT64_C(1)<<HandleIndexBits)-1;
constexpr uint64_t HandleGenerationLimit=UINT64_MAX>>HandleIndexBits;
}

NativeAdrenoUserClient::Entry *NativeAdrenoUserClient::find(uint64_t handle)
{
    return const_cast<Entry *>(static_cast<const NativeAdrenoUserClient *>(this)->find(handle));
}

const NativeAdrenoUserClient::Entry *NativeAdrenoUserClient::find(uint64_t handle) const
{
    const uint64_t slot=handle&HandleIndexMask;
    if(!entries_ || !slot || slot>entry_count_) {
        return nullptr;
    }
    const Entry &entry=entries_[slot-1];
    return entry.used && entry.handle==handle?&entry:nullptr;
}

void NativeAdrenoUserClient::returnEntry(Entry &entry)
{
    entry.handle=0;
    entry.mapped=0;
    entry.iova=0;
    entry.used=false;
    entry.mapped_state=false;
    entry.unreleasable=false;
    entry.imported=false;
    entry.metadata_bytes=0;
    entry.token=0;
    entry.mapping_leases=0;
    entry.controlled_mapping_leases=0;

    entry.next_free=free_head_;
    free_head_=static_cast<uint32_t>(&entry-entries_);
    --live_count_;
}

NativeAdrenoUserClient::Entry *NativeAdrenoUserClient::takeEntry()
{
    while(free_head_<entry_count_) {
        const uint32_t index=free_head_;
        Entry &entry=entries_[index];
        free_head_=entry.next_free;
        entry.next_free=NativeAdrenoClientMaxObjects;
        if(entry.generation==HandleGenerationLimit) {
            continue;
        }
        ++entry.generation;
        entry.handle=(entry.generation<<HandleIndexBits)|(uint64_t(index)+1);
        entry.token=index+1;
        entry.used=true;
        ++live_count_;
        return &entry;
    }
    return nullptr;
}

size_t NativeAdrenoUserClient::liveObjects() const
{
    return live_count_;
}
bool NativeAdrenoUserClient::mapped(uint64_t handle)
{
    Entry *entry=find(handle);return entry && entry->mapped_state;
}
NativeAdrenoMemory *NativeAdrenoUserClient::memoryOf(uint64_t handle)
{
    Entry *entry=find(handle);return entry && !entry->unreleasable?&entry->memory:nullptr;
}
IOReturn NativeAdrenoUserClient::open(uint32_t abi_version,NativeAdrenoVm *vm,
    NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,uint32_t input_bits,uint32_t output_bits)
{
    if(open_ || vm_ || submit_)return kIOReturnBusy;
    if(abi_version!=NativeAdrenoAbiVersion)return kIOReturnUnsupported;
    if(!vm || !submit || !ring || input_bits<31 || input_bits>48 ||
        output_bits<32 || output_bits>48 || vm->poisoned() || submit->poisoned() ||
        ring->poisoned())return kIOReturnBadArgument;
    entries_=IONewData(Entry,NativeAdrenoClientMaxObjects);
    submit_resources_=IONewData(NativeAdrenoSubmitResource,NativeAdrenoSubmitMaxResources);
    submit_buffers_=IONewData(NativeAdrenoSubmitBuffer,NativeAdrenoSubmitMaxIbs);
    if(!entries_ || !submit_resources_ || !submit_buffers_) {
        if(entries_) {
            IODeleteData(entries_,Entry,NativeAdrenoClientMaxObjects);
            entries_=nullptr;
        }
        if(submit_resources_) {
            IODeleteData(submit_resources_,NativeAdrenoSubmitResource,NativeAdrenoSubmitMaxResources);
            submit_resources_=nullptr;
        }
        if(submit_buffers_) {
            IODeleteData(submit_buffers_,NativeAdrenoSubmitBuffer,NativeAdrenoSubmitMaxIbs);
            submit_buffers_=nullptr;
        }
        return kIOReturnNoMemory;
    }
    memset(entries_,0,sizeof(Entry)*NativeAdrenoClientMaxObjects);
    entry_count_=NativeAdrenoClientMaxObjects;
    for(size_t index=0;index<entry_count_;++index) {
        entries_[index].next_free=static_cast<uint32_t>(index+1);
    }
    free_head_=0;
    live_count_=0;
    vm_=vm;submit_=submit;ring_=ring;input_bits_=input_bits;output_bits_=output_bits;
    vm_->addClient();
    open_=true;
    queues_[0]={0,true};
    /* Each client is a new GPU context for the CP: its first submission clears the BR/BV synchronization state. */
    ring_->requestContextReset();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::setScope(uint32_t queue_base,uint64_t window_start,uint64_t window_bytes)
{
    if(!open_)return kIOReturnNotReady;
    if(queue_base%NativeAdrenoAbiMaxQueues || queue_base>UINT32_MAX-NativeAdrenoAbiMaxQueues || (window_start&4095) || (window_bytes&4095) ||
        (window_bytes && (window_start+window_bytes<window_start || window_start+window_bytes>(UINT64_C(1)<<input_bits_))))
        return kIOReturnBadArgument;
    /* A client that owns no range yet has none to drain: its queue_base_ is the default 0, the render server's range, and the render server
     * nearly always has a job in flight, so every other process was refused here with kIOReturnBusy (int-200: WidgetRenderer_Default). */
    if(liveObjects() || ownRangeOutstanding() || submit_->rangeOutstanding(queue_base,NativeAdrenoAbiMaxQueues))return kIOReturnBusy;
    queue_base_=queue_base;scoped_=true;window_start_=window_start;window_bytes_=window_bytes;window_peak_=0;
    return kIOReturnSuccess;
}
bool NativeAdrenoUserClient::ownRangeOutstanding() const
{
    return (scoped_ || used_) && submit_->rangeOutstanding(queue_base_,NativeAdrenoAbiMaxQueues);
}
void NativeAdrenoUserClient::beginClose()
{
    if(open_) {
        submit_->scheduler()->closeOwner(queue_base_);
    }
}
IOReturn NativeAdrenoUserClient::close()
{
    if(!open_)return kIOReturnNotReady;
    if(liveObjects() || ownRangeOutstanding())return kIOReturnBusy;
    submit_->scheduler()->closeOwner(queue_base_);
    for(size_t i=1;i<NativeAdrenoAbiMaxQueues;++i)if(queues_[i].active)return kIOReturnBusy;
    (void)NativeAdrenoMemory::trimBackingCache();
    IODeleteData(entries_,Entry,entry_count_);
    IODeleteData(submit_resources_,NativeAdrenoSubmitResource,NativeAdrenoSubmitMaxResources);
    IODeleteData(submit_buffers_,NativeAdrenoSubmitBuffer,NativeAdrenoSubmitMaxIbs);
    submit_resources_=nullptr;
    submit_buffers_=nullptr;
    vm_->removeClient();
    entries_=nullptr;
    entry_count_=0;
    live_count_=0;
    free_head_=NativeAdrenoClientMaxObjects;
    queue_base_=0;scoped_=used_=false;window_start_=window_bytes_=window_peak_=0;
    open_=false;for(auto &queue:queues_)queue=Queue{};vm_=nullptr;submit_=nullptr;ring_=nullptr;timer_=nullptr;status_=nullptr;info_=nullptr;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::releaseAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!open_)return kIOReturnNotReady;
    if(!clock)return kIOReturnBadArgument;
    IOReturn status=(scoped_ || used_)?submit_->waitRange(queue_base_,NativeAdrenoAbiMaxQueues,clock,timeout_us):kIOReturnSuccess;
    if(status)return status;
    for(size_t index=0;index<entry_count_;++index) {
        Entry &entry=entries_[index];
        if(!entry.used)continue;
        entry.mapping_leases=0;
        entry.controlled_mapping_leases=0;
        NativeAdrenoAbiBoFree request{entry.handle};
        status=freeBo(&request);
        if(status)return status;
    }
    for(size_t i=1;i<NativeAdrenoAbiMaxQueues;++i)queues_[i]=Queue{};
    return close();
}
NativeAdrenoMemory *NativeAdrenoUserClient::memoryForToken(uint32_t token)
{
    if(!open_ || !token || token>entry_count_)return nullptr;
    Entry &entry=entries_[token-1];
    return entry.used && !entry.unreleasable && entry.token==token && entry.mapping_leases?&entry.memory:nullptr;
}
uint32_t NativeAdrenoUserClient::mappingLeases(uint32_t token) const
{
    if(!open_ || !token || token>entry_count_)return 0;
    const Entry &entry=entries_[token-1];
    return entry.used && entry.token==token?entry.mapping_leases:0;
}
bool NativeAdrenoUserClient::hasMappingLease(uint64_t handle,uint32_t token) const
{
    const Entry *entry=find(handle);
    return open_ && entry && !entry->unreleasable && entry->token==token && entry->mapping_leases;
}
IOReturn NativeAdrenoUserClient::query(NativeAdrenoAbiParams *params)
{
    if(!params)return kIOReturnBadArgument;
    *params={};
    if(!open_)return kIOReturnNotReady;
    params->abi_version=NativeAdrenoAbiVersion;
    params->input_bits=input_bits_;params->output_bits=output_bits_;
    params->flags=NativeAdrenoParamsHostImport|NativeAdrenoParamsBoProfile|
        NativeAdrenoParamsCpuMapV2|NativeAdrenoParamsBoProfileBudget|
        NativeAdrenoParamsSubmitV2|NativeAdrenoParamsResourceBudget;
    params->max_bo_bytes=UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift;
    return kIOReturnSuccess;
}
/* Per client slot (4, the kernel's own client last): [0] live objects now, [1] their peak, [2] refused BoAllocs, [3] last refusal status,
 * [4] bytes of the last refused request, [5] the window's highest mapped end in MiB above its start. Turnip turns any refusal into
 * VK_ERROR_OUT_OF_DEVICE_MEMORY, which its command streams do not check (int-237: backboardd wrote through a NULL stream pointer). */
extern "C" {volatile uint32_t native_adreno_client_stats[5][6];}
static volatile uint32_t *clientStats(uint32_t queue_base,uint64_t window_bytes) {
    const uint32_t slot=window_bytes?queue_base/NativeAdrenoAbiMaxQueues:4;
    return native_adreno_client_stats[slot<4?slot:4];
}
static void noteLive(uint32_t queue_base,uint64_t window_bytes,size_t live) {
    volatile uint32_t *stats=clientStats(queue_base,window_bytes);
    stats[0]=(uint32_t)live;if(live>stats[1])stats[1]=(uint32_t)live;
}
IOReturn NativeAdrenoUserClient::allocBo(const NativeAdrenoAbiBoAlloc *request,
    NativeAdrenoAbiBoAllocReply *reply)
{
    const IOReturn status=allocBoInner(request,reply);
    if(status && request) {
        volatile uint32_t *stats=clientStats(queue_base_,window_bytes_);
        stats[2]=stats[2]+1;stats[3]=(uint32_t)status;stats[4]=(uint32_t)request->bytes;
    }
    if(entries_)noteLive(queue_base_,window_bytes_,liveObjects());
    return status;
}
IOReturn NativeAdrenoUserClient::allocBoInner(const NativeAdrenoAbiBoAlloc *request,
    NativeAdrenoAbiBoAllocReply *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->handle=0;
    if(!open_)return kIOReturnNotReady;
    const uint64_t limit=UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift;
    if(!request->bytes || (request->bytes&4095) || request->bytes>limit ||
        request->align_log2<12 || request->align_log2>24 ||
        (request->allocation_flags&~uint32_t(NativeAdrenoBoAllocContiguous)))
        return kIOReturnBadArgument;
    Entry *entry=takeEntry();
    if(!entry)return kIOReturnNoResources;
    /* The GPU VM maps scattered pages into the caller's contiguous IOVA range.
     * Physical contiguity is requested only when the ABI caller requires it. */
    IOReturn status = entry->memory.allocate(static_cast<size_t>(request->bytes), output_bits_,
        (request->allocation_flags&NativeAdrenoBoAllocContiguous)!=0,static_cast<size_t>(request->bytes),true,true);
    if(status) {
        /* A failed allocation can leave quarantine behind; never reuse it. */
        if(entry->memory.release()!=kIOReturnSuccess) {
            entry->unreleasable=true;
        } else {
            returnEntry(*entry);
        }
        return status==kIOReturnUnsupported?kIOReturnUnsupported:status;
    }
    entry->mapped=0;entry->iova=0;entry->mapped_state=false;
    reply->handle=entry->handle;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::importBo(const NativeAdrenoAbiBoImport *request,void *task,
    NativeAdrenoAbiBoAllocReply *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->handle=0;
    if(!open_)return kIOReturnNotReady;
    native_adreno_import_diag[0]=native_adreno_import_diag[0]+1;
    const uint64_t limit=UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift;
    if(!task || !request->address || ((request->address|request->bytes)&4095) || !request->bytes || request->bytes>limit) {
        noteImport(kIOReturnBadArgument,1,request->bytes,request->address);
        return kIOReturnBadArgument;
    }
    Entry *entry=takeEntry();
    if(!entry) {noteImport(kIOReturnNoResources,2,request->bytes,request->address);return kIOReturnNoResources;}
    const IOReturn status=entry->memory.importTask(task,request->address,(size_t)request->bytes,output_bits_,(size_t)request->bytes);
    if(status) {
        noteImport(status,native_adreno_import_stage,request->bytes,request->address);
        /* l745: a failed import is never published (handle stays 0), so its
         * entry must always go back to the pool. release() may fail (Busy or
         * a quarantined teardown) and the old unreleasable=true path leaked
         * the slot for good: takeEntry() then ran dry and answered
         * kIOReturnNoResources to every later import AND allocation - the
         * Metal bridge's fallback buffers died with it and the compositor
         * stopped drawing (zero commands). Force the release and free. */
        (void)entry->memory.release();
        returnEntry(*entry);
        return status;
    }
    entry->mapped=0;entry->iova=0;entry->mapped_state=false;entry->imported=true;
    reply->handle=entry->handle;
    native_adreno_import_live[0]=native_adreno_import_live[0]+1;
    {
        const unsigned slot=3+(gImportNext++%8)*4;
        native_adreno_import_live[slot]=request->address;native_adreno_import_live[slot+1]=(uint64_t)proc_selfpid()<<40|request->bytes;
        native_adreno_import_live[slot+2]=entry->handle;native_adreno_import_live[slot+3]=1;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::mapBo(const NativeAdrenoAbiBoMap *request,
    NativeAdrenoAbiBoMapReply *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->iova=0;
    if(!open_)return kIOReturnNotReady;
    if(!(request->permissions&1) || (request->permissions&~3u) || request->writeback>1 ||
        (request->shareability!=2 && request->shareability!=3) || request->reserved ||
        (request->iova&4095))return kIOReturnBadArgument;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    if(entry->mapped_state)return kIOReturnBusy;
    const uint64_t end=request->iova+entry->memory.bytes();
    if(window_bytes_ && (request->iova<window_start_ || end<request->iova || end>window_start_+window_bytes_)) {
        /* Per window: how often a mapping fell outside it, and the last one's size and end (MiB), for the boot log's [gpu windows] row. */
        const uint32_t slot=queue_base_/NativeAdrenoAbiMaxQueues;
        if(slot<4) {
            native_adreno_window_overflow[slot][0]=native_adreno_window_overflow[slot][0]+1;
            native_adreno_window_overflow[slot][1]=uint32_t(entry->memory.bytes()>>10);
            native_adreno_window_overflow[slot][2]=uint32_t(end>>20);
        }
        return kIOReturnBadArgument;
    }
    uint64_t handle=0;
    /* Memory the owner maps read-only could only be pinned for reading: the GPU gets the read half of what was asked. */
    const unsigned permissions=entry->memory.readOnly()?(request->permissions&~2u):request->permissions;
    /* The platform and backing choose coherence. KGSL's A642L IOMMU_CACHE
     * mapping uses Normal WB/WA and Outer Shareable for reads and writes. */
    const bool coherent=vm_->cachedCoherentImports() && entry->memory.cpuCacheCoherent();
    if(request->writeback && !coherent) {
        return kIOReturnUnsupported;
    }
    const IOReturn completed=submit_->retireCompleted();
    if(completed) {
        return completed;
    }
    const uint64_t mapBegan=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
    const IOReturn status=vm_->map(&entry->memory,0,request->iova,permissions,
        0u,2u,&handle);
    if(mapBegan && native_adreno_vm_now_us) {
        const uint64_t mapEnded=native_adreno_vm_now_us();
        const uint64_t pages=(entry->memory.bytes()+4095)/4096;
        native_adreno_profile_map(uint32_t(pages>0xffffffffu?0xffffffffu:pages),mapEnded>mapBegan?mapEnded-mapBegan:0);
    }
    if(status) {
        map_failure_.last_failure_handle=request->handle;
        map_failure_.last_failure_iova=request->iova;
        map_failure_.last_failure_status=status;
        map_failure_.last_failure_reason=status==kIOReturnNoSpace?NativeAdrenoMapFailureCapacity:
            status==kIOReturnBusy && vm_->mapBusyAlias()?NativeAdrenoMapFailureActiveAlias:
            status==kIOReturnBadArgument || status==kIOReturnBusy?NativeAdrenoMapFailureAddress:
            status==kIOReturnNoResources && !vm_->mapAliasLimit()?NativeAdrenoMapFailureTables:
            NativeAdrenoMapFailureOther;
        return status;
    }
    entry->mapped=handle;entry->iova=request->iova;entry->mapped_state=true;
    ++mapped_count_;
    if(end>window_peak_)window_peak_=end;
    if(window_bytes_ && window_peak_>window_start_)clientStats(queue_base_,window_bytes_)[5]=(uint32_t)((window_peak_-window_start_)>>20);
    reply->iova=request->iova;
    return kIOReturnSuccess;
}
static void noteFreeRefused(bool imported,uint64_t handle,IOReturn status)
{
    if(!imported)return;
    native_adreno_import_live[1]=native_adreno_import_live[1]+1;native_adreno_import_live[2]=(uint32_t)status;noteImportState(handle,3);
}
IOReturn NativeAdrenoUserClient::freeBo(const NativeAdrenoAbiBoFree *request)
{
    native_adreno_trace_stage=1;
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    Entry *entry=find(request->handle);
    if(!entry)return kIOReturnBadArgument;
    if(entry->unreleasable) {noteFreeRefused(entry->imported,entry->handle,kIOReturnBusy);return kIOReturnBusy;}
    if(entry->mapping_leases || entry->controlled_mapping_leases) {
        noteFreeRefused(entry->imported,entry->handle,kIOReturnBusy);
        return kIOReturnBusy;
    }
    native_adreno_trace_stage=2;
    if(entry->mapped_state) {
        /* Completed jobs must not pin the buffer: retire what the CP has passed before the unmap decides.
         * The scheduler job holds its own reference until advance() finishes it, which the completion
         * worker may not have done yet, so advance here as closeOwner() does. */
        NativeAdrenoScheduler *scheduler=submit_->scheduler();
        if(scheduler)scheduler->advance(); /* a faulted scheduler refuses and the unmap below stays refused too */
        else submit_->retireCompleted(); /* a poisoned execbuf refuses and the unmap below stays refused too */
        native_adreno_trace_stage=3;
        const IOReturn unmap=vm_->unmap(entry->mapped);
        native_adreno_trace_stage=4;
        if(unmap) {noteFreeRefused(entry->imported,entry->handle,unmap);return unmap;}
        entry->mapped_state=false;entry->mapped=0;entry->iova=0;
        --mapped_count_;
    }
    const IOReturn release=entry->memory.release(true);
    if(release) {noteFreeRefused(entry->imported,entry->handle,release);return release;}
    if(entry->imported) {native_adreno_import_live[0]=native_adreno_import_live[0]-1;noteImportState(entry->handle,2);}
    returnEntry(*entry);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::submit(const NativeAdrenoAbiSubmit *request,
    NativeAdrenoAbiSubmitReply *reply,const NativeAdrenoRingClock *clock)
{
    if(!request || !reply)return kIOReturnBadArgument;reply->fence=0;
    if(!open_)return kIOReturnNotReady;
    if(!request->buffer_count || request->buffer_count>NativeAdrenoAbiMaxBuffers ||
        request->reserved)return kIOReturnBadArgument;
    if(request->queue>=NativeAdrenoAbiMaxQueues || !queues_[request->queue].active)
        return kIOReturnBadArgument;
    if(request->wait_fence) {
        if(request->wait_fence>UINT32_MAX) {
            return kIOReturnBadArgument;
        }
        const IOReturn observed=submit_->observeFence(uint32_t(request->wait_fence),nullptr,0);
        if(observed && observed!=kIOReturnTimeout) {
            return observed;
        }
    }
    NativeAdrenoSubmitBuffer buffers[NativeAdrenoAbiMaxBuffers];
    for(uint32_t i=0;i<request->buffer_count;++i) {
        const NativeAdrenoAbiSubmitBuffer &wire=request->buffers[i];
        if(!wire.bytes || (wire.bytes&3))return kIOReturnBadArgument;
        Entry *entry=find(wire.handle);
        if(!entry || !entry->mapped_state)return kIOReturnBadArgument;
        buffers[i]={entry->mapped,wire.iova,(size_t)wire.bytes};
    }
    size_t resource_count=0;
    for(size_t at=0;at<entry_count_;++at) {
        const Entry &entry=entries_[at];
        if(!entry.used || !entry.mapped_state) {
            continue;
        }
        uint64_t iova=0;
        size_t bytes=0;
        unsigned permissions=0;
        const IOReturn described=vm_->describe(entry.mapped,&iova,&bytes,&permissions);
        if(described) {
            return described;
        }
        submit_resources_[resource_count++]={entry.mapped,0,bytes,permissions&3u,false};
    }
    uint32_t fence=0;
    used_=true;
    const IOReturn status=submit_->scheduler()->submitLegacy(
        queue_base_+static_cast<uint32_t>(request->queue),buffers,request->buffer_count,
        submit_resources_,resource_count,NativeAdrenoFallbackLegacySubmit,&fence);
    if(status)return status;
    reply->fence=fence;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::submitCaps(NativeAdrenoAbiSubmitCaps *reply)
{
    if(!reply || !open_) {
        return kIOReturnNotReady;
    }
    *reply={NativeAdrenoSubmitVersion,NativeAdrenoSubmitMaxPacketBytes,
        NativeAdrenoSubmitMaxIbs,NativeAdrenoSubmitMaxResources,
        NativeAdrenoSubmitHardwareCoherent,0,status_?status_->suspends()+1:1};
    if(vm_->cachedCoherentImports()) {
        reply->flags|=NativeAdrenoSubmitCachedCoherentImports;
    }
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoUserClient::resourceBudget(NativeAdrenoAbiResourceBudget *reply) const
{
    if(!reply || !open_) {
        return kIOReturnNotReady;
    }
    *reply=map_failure_;
    reply->version=NativeAdrenoResourceBudgetVersion;
    reply->flags=NativeAdrenoBudgetMapNoSpace;
    reply->capacity=uint32_t(vm_->objectCapacity());
    reply->used=uint32_t(vm_->objectCount());
    reply->reserved=uint32_t(vm_->reservedObjects());
    reply->connection_used=uint32_t(mapped_count_);
    reply->active_connections=vm_->clientCount();
    reply->connection_target=reply->capacity/(reply->active_connections?reply->active_connections:1);
    reply->generation=vm_->budgetGeneration();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoUserClient::submitV2(const void *input,size_t input_size,
    NativeAdrenoAbiSubmitV2Reply *reply,const NativeAdrenoAbiSubmitAsync *async)
{
    if(!open_ || !reply) {
        return kIOReturnNotReady;
    }
    *reply={};
    if(!input || input_size<sizeof(NativeAdrenoAbiSubmitV2) || input_size>NativeAdrenoSubmitMaxPacketBytes) {
        native_adreno_submit_failure(1,kIOReturnBadArgument,0,0,0,0,0);
        return kIOReturnBadArgument;
    }
    const auto &header=*static_cast<const NativeAdrenoAbiSubmitV2 *>(input);
    const uint32_t allowed_reasons=NativeAdrenoFallbackIncompleteTracking|NativeAdrenoFallbackDeviceAddress|
        NativeAdrenoFallbackDescriptorBuffer|NativeAdrenoFallbackLegacySubmit;
    const uint64_t epoch=status_?status_->suspends()+1:1;
    if(header.version!=NativeAdrenoSubmitVersion || header.header_bytes!=sizeof(header) ||
        header.total_bytes!=input_size || (header.flags&~NativeAdrenoSubmitAllMapped) ||
        header.reserved || header.reserved2 || (!async && !header.ib_count) || header.ib_count>NativeAdrenoSubmitMaxIbs ||
        (!async && !header.resource_count) || header.resource_count>NativeAdrenoSubmitMaxResources ||
        (header.fallback_reason&~allowed_reasons) ||
        bool(header.flags&NativeAdrenoSubmitAllMapped)!=bool(header.fallback_reason) ||
        header.queue>=NativeAdrenoAbiMaxQueues || !queues_[header.queue].active) {
        native_adreno_submit_failure(1,kIOReturnBadArgument,0,0,0,header.ib_count,header.resource_count);
        return kIOReturnBadArgument;
    }
    if(header.epoch!=epoch) {
        return kIOReturnNotReady;
    }
    const size_t ib_bytes=size_t(header.ib_count)*sizeof(NativeAdrenoAbiSubmitIbV2);
    const size_t resource_bytes=size_t(header.resource_count)*sizeof(NativeAdrenoAbiSubmitResourceV2);
    if((header.ib_offset&7) || (header.resource_offset&7) ||
        header.ib_offset<sizeof(header) || header.ib_offset>input_size || ib_bytes>input_size-header.ib_offset ||
        header.resource_offset<sizeof(header) || header.resource_offset>input_size ||
        resource_bytes>input_size-header.resource_offset ||
        (header.ib_offset<header.resource_offset+resource_bytes && header.resource_offset<header.ib_offset+ib_bytes)) {
        native_adreno_submit_failure(1,kIOReturnBadArgument,0,0,0,header.ib_count,header.resource_count);
        return kIOReturnBadArgument;
    }

    const auto *packet=static_cast<const uint8_t *>(input);
    const auto *resources=reinterpret_cast<const NativeAdrenoAbiSubmitResourceV2 *>(packet+header.resource_offset);
    const auto *ibs=reinterpret_cast<const NativeAdrenoAbiSubmitIbV2 *>(packet+header.ib_offset);
    size_t resource_count=0;
    for(uint32_t at=0;at<header.resource_count;++at) {
        const auto &resource=resources[at];
        Entry *entry=find(resource.handle);
        uint64_t iova=0;
        size_t bytes=0;
        unsigned permissions=0;
        IOReturn described=kIOReturnNotReady;
        bool vm_queried=false;
        NativeAdrenoResourceReject reason=NativeAdrenoResourceReject::None;
        if(!entry) {
            reason=NativeAdrenoResourceReject::HandleMissing;
        } else if(!entry->mapped_state) {
            reason=NativeAdrenoResourceReject::HandleUnmapped;
        } else if(entry->unreleasable) {
            reason=NativeAdrenoResourceReject::HandleUnreleasable;
        } else if(resource.reserved || !resource.access || (resource.access&~3u)) {
            reason=NativeAdrenoResourceReject::AccessFormat;
        } else {
            described=vm_->describe(entry->mapped,&iova,&bytes,&permissions);
            vm_queried=true;
            if(described) {
                reason=NativeAdrenoResourceReject::VmDescribe;
            } else if(resource.access&~permissions) {
                reason=NativeAdrenoResourceReject::Permissions;
            } else if(!resource.bytes || resource.offset>bytes || resource.bytes>bytes-resource.offset) {
                reason=NativeAdrenoResourceReject::Range;
            }
        }
        if(reason!=NativeAdrenoResourceReject::None) {
            NativeAdrenoSubmitResourceFailure failure;
            failure.reason=reason;
            failure.access=resource.access;
            failure.reserved=resource.reserved;
            const uint64_t slot=resource.handle&HandleIndexMask;
            const Entry *slot_entry=entries_ && slot && slot<=entry_count_?&entries_[slot-1]:nullptr;
            if(slot_entry) {
                failure.slot_present=1;
                failure.used=slot_entry->used;
                failure.mapped=slot_entry->mapped_state;
                failure.unreleasable=slot_entry->unreleasable;
                failure.entry_handle=slot_entry->handle;
                failure.mapped_handle=slot_entry->mapped;
                failure.memory_bytes=slot_entry->memory.bytes();
                failure.read_only=slot_entry->memory.readOnly();
            }
            if(!vm_queried && entry && entry->mapped_state) {
                described=vm_->describe(entry->mapped,&iova,&bytes,&permissions);
                vm_queried=true;
            }
            failure.vm_queried=vm_queried;
            failure.vm_status=described;
            failure.permissions=permissions;
            failure.vm_iova=iova;
            failure.vm_bytes=bytes;
            native_adreno_submit_failure(2,kIOReturnBadArgument,resource.handle,resource.offset,at,
                header.ib_count,header.resource_count,resource.bytes,&failure);
            return kIOReturnBadArgument;
        }

        const size_t first=size_t(resource.offset);
        const size_t end=first+size_t(resource.bytes);
        if(resource_count) {
            NativeAdrenoSubmitResource &previous=submit_resources_[resource_count-1];
            const size_t previous_end=previous.offset+previous.bytes;
            if(previous.handle==entry->mapped && previous.access==resource.access &&
                first<=previous_end && previous.offset<=end) {
                const size_t merged_first=previous.offset<first?previous.offset:first;
                const size_t merged_end=previous_end>end?previous_end:end;
                previous.offset=merged_first;
                previous.bytes=merged_end-merged_first;
                continue;
            }
        }
        if(resource_count==NativeAdrenoSubmitMaxResources) {
            return kIOReturnNoResources;
        }
        submit_resources_[resource_count++]={entry->mapped,first,size_t(resource.bytes),resource.access,false};
    }

    for(uint32_t at=0;at<header.ib_count;++at) {
        const auto &ib=ibs[at];
        if(ib.reserved || ib.resource_index>=header.resource_count) {
            native_adreno_submit_failure(3,kIOReturnBadArgument,0,ib.offset,at,header.ib_count,header.resource_count);
            return kIOReturnBadArgument;
        }
        const auto &resource=resources[ib.resource_index];
        Entry *entry=find(resource.handle);
        if(!(resource.access&NativeAdrenoResourceRead) || !ib.bytes || ib.bytes>0xfffffu*4u ||
            ((ib.offset|ib.bytes)&3) ||
            ib.offset<resource.offset || ib.offset-resource.offset>resource.bytes ||
            ib.bytes>resource.bytes-(ib.offset-resource.offset)) {
            native_adreno_submit_failure(3,kIOReturnBadArgument,resource.handle,ib.offset,at,
                header.ib_count,header.resource_count,ib.bytes);
            return kIOReturnBadArgument;
        }
        submit_buffers_[at]={entry->mapped,entry->iova+ib.offset,size_t(ib.bytes)};
    }

    if(header.flags&NativeAdrenoSubmitAllMapped) {
        resource_count=0;
        for(size_t at=0;at<entry_count_;++at) {
            const Entry &entry=entries_[at];
            if(!entry.used || !entry.mapped_state) {
                continue;
            }
            uint64_t iova=0;
            size_t bytes=0;
            unsigned permissions=0;
            const IOReturn described=vm_->describe(entry.mapped,&iova,&bytes,&permissions);
            if(described) {
                return described;
            }
            if(resource_count==NativeAdrenoSubmitMaxResources) {
                return kIOReturnNoResources;
            }
            submit_resources_[resource_count++]={entry.mapped,0,bytes,permissions&3u,false};
        }
    }
    uint32_t fence=0;
    used_=true;
    if(async) {
        const auto *packet=reinterpret_cast<const uint8_t *>(async);
        const auto *waits=reinterpret_cast<const NativeAdrenoAbiSyncPoint *>(packet+async->wait_offset);
        const auto *signals=reinterpret_cast<const NativeAdrenoAbiSyncPoint *>(packet+async->signal_offset);
        const IOReturn result=submit_->scheduler()->accept(queue_base_,queue_base_+uint32_t(header.queue),
            submit_buffers_,header.ib_count,submit_resources_,resource_count,header.fallback_reason,
            waits,async->wait_count,signals,async->signal_count,&reply->fence);
        reply->epoch=submit_->scheduler()->epoch();
        return result;
    }
    const IOReturn result=submit_->scheduler()->submitLegacy(queue_base_+uint32_t(header.queue),
        submit_buffers_,header.ib_count,submit_resources_,resource_count,header.fallback_reason,&fence);
    if(!result) {
        reply->fence=fence;
        reply->epoch=epoch;
    }
    return result;
}

IOReturn NativeAdrenoUserClient::submitAsync(const void *input,size_t bytes,
    NativeAdrenoAbiAsyncReply *reply)
{
    if(!open_ || !input || !reply || bytes<sizeof(NativeAdrenoAbiSubmitAsync) ||
        bytes>NativeAdrenoSubmitMaxPacketBytes) {
        return kIOReturnBadArgument;
    }
    const auto &header=*static_cast<const NativeAdrenoAbiSubmitAsync *>(input);
    if(header.version!=NativeAdrenoAsyncVersion || header.total_bytes!=bytes ||
        header.epoch!=submit_->scheduler()->epoch() ||
        header.wait_count>NativeAdrenoAsyncMaxWaits || header.signal_count>NativeAdrenoAsyncMaxSignals) {
        return kIOReturnBadArgument;
    }
    const uint64_t starts[]={header.submit_offset,header.wait_offset,header.signal_offset};
    const uint64_t lengths[]={header.submit_bytes,
        uint64_t(header.wait_count)*sizeof(NativeAdrenoAbiSyncPoint),
        uint64_t(header.signal_count)*sizeof(NativeAdrenoAbiSyncPoint)};
    for(unsigned at=0;at<3;++at) {
        if((starts[at]&7) || starts[at]<sizeof(header) || starts[at]>bytes ||
            lengths[at]>bytes-starts[at]) {
            return kIOReturnBadArgument;
        }
        for(unsigned before=0;before<at;++before) {
            if(lengths[at] && lengths[before] && starts[at]<starts[before]+lengths[before] &&
                starts[before]<starts[at]+lengths[at]) {
                return kIOReturnBadArgument;
            }
        }
    }
    NativeAdrenoAbiSubmitV2Reply result{};
    const IOReturn status=submitV2(static_cast<const uint8_t *>(input)+header.submit_offset,
        header.submit_bytes,&result,&header);
    if(!status) {
        *reply={result.fence,result.epoch};
    }
    return status;
}
IOReturn NativeAdrenoUserClient::attachTimer(const NativeAdrenoTimer *timer)
{
    if(!open_)return kIOReturnNotReady;
    if(!timer || timer_ )return kIOReturnBadArgument;
    timer_=timer;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::timestamp(NativeAdrenoAbiTimestamp *reply)
{
    if(!reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    if(!timer_)return kIOReturnUnsupported;
    return timer_->read(&reply->ticks);
}
IOReturn NativeAdrenoUserClient::attachDeviceInfo(const NativeAdrenoAbiDeviceInfo *info)
{
    if(!open_)return kIOReturnNotReady;
    if(!info || info_ || info->reserved || !info->chip_id || !info->va_size)return kIOReturnBadArgument;
    info_=info;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::deviceInfo(NativeAdrenoAbiDeviceInfo *reply)
{
    if(!reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    if(!info_)return kIOReturnUnsupported;
    *reply=*info_;
    if(window_bytes_){reply->va_start=window_start_;reply->va_size=window_bytes_;}
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::attachStatus(const NativeAdrenoStatus *status)
{
    if(!open_)return kIOReturnNotReady;
    if(!status || status_)return kIOReturnBadArgument;
    status_=status;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::status(NativeAdrenoAbiStatus *reply)
{
    if(!reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    if(!status_)return kIOReturnUnsupported;
    const NativeAdrenoStatusCounts counts=status_->counts();
    reply->faults=counts.faults;reply->suspends=counts.suspends;
    return kIOReturnSuccess;
}
size_t NativeAdrenoUserClient::inputSize(uint32_t method)
{
    switch(method) {
    case NativeAdrenoMethodQuery:
    case NativeAdrenoMethodQuerySubmitV2:
    case NativeAdrenoMethodGetResourceBudget:
    case NativeAdrenoMethodQueryAsync:
    case NativeAdrenoMethodGetTimestamp:
    case NativeAdrenoMethodGetStatus:
    case NativeAdrenoMethodGetDeviceInfo:return 0;
    case NativeAdrenoMethodBoAlloc:return sizeof(NativeAdrenoAbiBoAlloc);
    case NativeAdrenoMethodBoMap:return sizeof(NativeAdrenoAbiBoMap);
    case NativeAdrenoMethodBoImport:return sizeof(NativeAdrenoAbiBoImport);
    case NativeAdrenoMethodBoFree:return sizeof(NativeAdrenoAbiBoFree);
    case NativeAdrenoMethodSubmit:return sizeof(NativeAdrenoAbiSubmit);
    case NativeAdrenoMethodSetMetadata:
    case NativeAdrenoMethodGetMetadata:return sizeof(NativeAdrenoAbiMetadata);
    case NativeAdrenoMethodCreateQueue:
    case NativeAdrenoMethodCloseQueue:return sizeof(NativeAdrenoAbiQueue);
    case NativeAdrenoMethodBoCpuMap:return sizeof(NativeAdrenoAbiBoCpuMap);
    case NativeAdrenoMethodBoCpuUnmap:return sizeof(NativeAdrenoAbiBoCpuUnmap);
    case NativeAdrenoMethodWaitFence:return sizeof(NativeAdrenoAbiWaitFence);
    case NativeAdrenoMethodSyncCreate:return sizeof(NativeAdrenoAbiSyncCreate);
    case NativeAdrenoMethodSyncRelease:
    case NativeAdrenoMethodSyncQuery:return sizeof(NativeAdrenoAbiSyncHandle);
    case NativeAdrenoMethodSyncSignal:return sizeof(NativeAdrenoAbiSyncPoint);
    case NativeAdrenoMethodSyncWait:return sizeof(NativeAdrenoAbiSyncWait);
    case NativeAdrenoMethodWaitCompletion:return sizeof(NativeAdrenoAbiCompletionWait);
    case NativeAdrenoMethodWaitCapacity:return sizeof(NativeAdrenoAbiCapacityWait);
    case NativeAdrenoMethodWaitBoIdle:return sizeof(NativeAdrenoAbiBoWait);
    default:return SIZE_MAX;
    }
}
size_t NativeAdrenoUserClient::outputSize(uint32_t method)
{
    switch(method) {
    case NativeAdrenoMethodQuery:return sizeof(NativeAdrenoAbiParams);
    case NativeAdrenoMethodQuerySubmitV2:return sizeof(NativeAdrenoAbiSubmitCaps);
    case NativeAdrenoMethodGetResourceBudget:return sizeof(NativeAdrenoAbiResourceBudget);
    case NativeAdrenoMethodQueryAsync:return sizeof(NativeAdrenoAbiAsyncCaps);
    case NativeAdrenoMethodSyncCreate:return sizeof(NativeAdrenoAbiSyncHandle);
    case NativeAdrenoMethodSyncQuery:return sizeof(NativeAdrenoAbiSyncValue);
    case NativeAdrenoMethodSyncRelease:
    case NativeAdrenoMethodSyncSignal:
    case NativeAdrenoMethodSyncWait:
    case NativeAdrenoMethodWaitCompletion:
    case NativeAdrenoMethodWaitBoIdle:
    case NativeAdrenoMethodWaitCapacity:return 0;
    case NativeAdrenoMethodBoAlloc:return sizeof(NativeAdrenoAbiBoAllocReply);
    case NativeAdrenoMethodBoMap:return sizeof(NativeAdrenoAbiBoMapReply);
    case NativeAdrenoMethodBoFree:return 0;
    case NativeAdrenoMethodSubmit:return sizeof(NativeAdrenoAbiSubmitReply);
    case NativeAdrenoMethodWaitFence:return 0;
    case NativeAdrenoMethodGetTimestamp:return sizeof(NativeAdrenoAbiTimestamp);
    case NativeAdrenoMethodGetStatus:return sizeof(NativeAdrenoAbiStatus);
    case NativeAdrenoMethodGetDeviceInfo:return sizeof(NativeAdrenoAbiDeviceInfo);
    case NativeAdrenoMethodGetMetadata:return sizeof(NativeAdrenoAbiMetadata);
    case NativeAdrenoMethodSetMetadata:return 0;
    case NativeAdrenoMethodCreateQueue:return sizeof(NativeAdrenoAbiQueueReply);
    case NativeAdrenoMethodCloseQueue:return 0;
    case NativeAdrenoMethodBoCpuMap:return sizeof(NativeAdrenoAbiBoCpuMapReply);
    case NativeAdrenoMethodBoCpuUnmap:return 0;
    case NativeAdrenoMethodBoImport:return sizeof(NativeAdrenoAbiBoAllocReply);
    default:return SIZE_MAX;
    }
}
IOReturn NativeAdrenoUserClient::dispatch(uint32_t method,const void *input,
    size_t input_size,void *output,size_t output_size,const NativeAdrenoRingClock *clock)
{
    struct Tally {
        uint32_t method;uint64_t began;IOReturn *result;const NativeAdrenoRingClock *clock;
        Tally(uint32_t m,IOReturn *r,const NativeAdrenoRingClock *c):method(m),began(c && c->now_us?c->now_us(c->cookie):0),result(r),clock(c) {}
        ~Tally() {
            if(method>=20)return;
            const uint64_t now=clock && clock->now_us?clock->now_us(clock->cookie):0;
            native_adreno_method_stats[method][0]=native_adreno_method_stats[method][0]+1;
            if(*result)native_adreno_method_stats[method][1]=native_adreno_method_stats[method][1]+1;
            native_adreno_method_stats[method][2]=(uint32_t)*result;
            native_adreno_method_stats[method][3]=native_adreno_method_stats[method][3]+(uint32_t)(now-began);
        }
    };
    /* The same clock times the VM's table transactions (the board service's probe never ran on this path, so nothing else installed it). */
    static const NativeAdrenoRingClock *vm_clock=nullptr;
    vm_clock=clock;
    native_adreno_vm_now_us=[]()->uint64_t{return vm_clock && vm_clock->now_us?vm_clock->now_us(vm_clock->cookie):0;};
    IOReturn tally_result=kIOReturnSuccess;
    Tally tally(method,&tally_result,clock);
    if(method==NativeAdrenoMethodSubmitAsync) {
        if(!output || output_size!=sizeof(NativeAdrenoAbiAsyncReply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiAsyncReply reply{};
        const IOReturn result=submitAsync(input,input_size,&reply);
        if(!result) {
            memcpy(output,&reply,sizeof(reply));
        }
        return result;
    }
    if(method==NativeAdrenoMethodSubmitV2) {
        if(!output || output_size!=sizeof(NativeAdrenoAbiSubmitV2Reply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiSubmitV2Reply reply{};
        const IOReturn result=submitV2(input,input_size,&reply);
        if(!result) {
            memcpy(output,&reply,sizeof(reply));
        }
        return result;
    }
    const size_t expected_in=inputSize(method),expected_out=outputSize(method);
    if(expected_in==SIZE_MAX || expected_out==SIZE_MAX)return kIOReturnUnsupported;
    if(input_size!=expected_in || output_size!=expected_out)return kIOReturnBadArgument;
    if((expected_in && !input) || (expected_out && !output))return kIOReturnBadArgument;
    IOReturn status=kIOReturnSuccess;
    switch(method) {
    case NativeAdrenoMethodQueryAsync: {
        const NativeAdrenoAbiAsyncCaps reply{NativeAdrenoAsyncVersion,NativeAdrenoAsyncMaxJobs,
            NativeAdrenoAsyncMaxWaits,NativeAdrenoAsyncMaxSignals,submit_->scheduler()->epoch()};
        memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodSyncCreate: {
        NativeAdrenoAbiSyncHandle reply{};
        status=submit_->scheduler()->createSync(queue_base_,
            *static_cast<const NativeAdrenoAbiSyncCreate *>(input),&reply.handle);
        if(!status) {
            memcpy(output,&reply,sizeof(reply));
        }
        break;
    }
    case NativeAdrenoMethodSyncRelease:
        status=submit_->scheduler()->releaseSync(queue_base_,
            static_cast<const NativeAdrenoAbiSyncHandle *>(input)->handle);
        break;
    case NativeAdrenoMethodSyncSignal:
        status=submit_->scheduler()->signalSync(queue_base_,
            *static_cast<const NativeAdrenoAbiSyncPoint *>(input));
        break;
    case NativeAdrenoMethodSyncQuery: {
        NativeAdrenoAbiSyncValue reply{};
        status=submit_->scheduler()->querySync(queue_base_,
            static_cast<const NativeAdrenoAbiSyncHandle *>(input)->handle,&reply.value);
        if(!status) {
            memcpy(output,&reply,sizeof(reply));
        }
        break;
    }
    case NativeAdrenoMethodSyncWait:
        status=submit_->scheduler()->waitSyncs(queue_base_,
            *static_cast<const NativeAdrenoAbiSyncWait *>(input));
        break;
    case NativeAdrenoMethodWaitCompletion: {
        const auto &request=*static_cast<const NativeAdrenoAbiCompletionWait *>(input);
        status=request.epoch==submit_->scheduler()->epoch()?
            submit_->scheduler()->completion(request.completion):kIOReturnNotReady;
        break;
    }
    case NativeAdrenoMethodWaitCapacity: {
        const auto &request=*static_cast<const NativeAdrenoAbiCapacityWait *>(input);
        status=request.epoch!=submit_->scheduler()->epoch()?kIOReturnNotReady:
            submit_->scheduler()->capacity()?kIOReturnSuccess:kIOReturnTimeout;
        break;
    }
    case NativeAdrenoMethodWaitBoIdle: {
        const auto &request=*static_cast<const NativeAdrenoAbiBoWait *>(input);
        const Entry *entry=find(request.handle);
        status=request.epoch!=submit_->scheduler()->epoch()?kIOReturnNotReady:
            !entry?kIOReturnBadArgument:entry->mapped_state && vm_->resourceBusy(entry->mapped)?
            kIOReturnTimeout:kIOReturnSuccess;
        break;
    }
    case NativeAdrenoMethodQuerySubmitV2: {
        NativeAdrenoAbiSubmitCaps reply{};
        status=submitCaps(&reply);
        if(!status) {
            memcpy(output,&reply,sizeof(reply));
        }
        break;
    }
    case NativeAdrenoMethodGetResourceBudget: {
        NativeAdrenoAbiResourceBudget reply{};
        status=resourceBudget(&reply);
        if(!status) {
            memcpy(output,&reply,sizeof(reply));
        }
        break;
    }
    case NativeAdrenoMethodQuery: {
        NativeAdrenoAbiParams reply{};
        status=query(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoAlloc: {
        NativeAdrenoAbiBoAlloc request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoAllocReply reply{};
        status=allocBo(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoImport:
        status=kIOReturnUnsupported;
        break;
    case NativeAdrenoMethodBoMap: {
        NativeAdrenoAbiBoMap request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoMapReply reply{};
        status=mapBo(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        else {
            /* Why maps fail (int-135: 55 of 191, and the translator answers each as a device out-of-memory): status, count, and what the table
             * held and the failed request asked for the last time. */
            unsigned at=0;while(at<5 && native_adreno_map_fail[at][0] && native_adreno_map_fail[at][0]!=(uint32_t)status)++at;
            native_adreno_map_fail[at][0]=(uint32_t)status;native_adreno_map_fail[at][1]=native_adreno_map_fail[at][1]+1;
            native_adreno_map_fail[5][0]=(uint32_t)vm_->spanCount();native_adreno_map_fail[5][1]=(uint32_t)vm_->objectCount();
            native_adreno_map_fail[6][0]=(uint32_t)(request.iova>>12);native_adreno_map_fail[6][1]=(uint32_t)(request.handle);
        }
        break;
    }
    case NativeAdrenoMethodBoFree: {
        NativeAdrenoAbiBoFree request{};
        memcpy(&request,input,sizeof(request));
        status=freeBo(&request);
        break;
    }
    case NativeAdrenoMethodSubmit: {
        NativeAdrenoAbiSubmit request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiSubmitReply reply{};
        status=submit(&request,&reply,clock);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodWaitFence: {
        NativeAdrenoAbiWaitFence request{};
        memcpy(&request,input,sizeof(request));
        status=waitFence(&request,clock);
        break;
    }
    case NativeAdrenoMethodGetDeviceInfo: {
        NativeAdrenoAbiDeviceInfo reply{};
        status=deviceInfo(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodGetTimestamp: {
        NativeAdrenoAbiTimestamp reply{};
        status=timestamp(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoCpuMap: {
        NativeAdrenoAbiBoCpuMap request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoCpuMapReply reply{};
        status=cpuMap(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoCpuUnmap: {
        NativeAdrenoAbiBoCpuUnmap request{};
        memcpy(&request,input,sizeof(request));
        status=cpuUnmap(&request);
        break;
    }
    case NativeAdrenoMethodCreateQueue: {
        NativeAdrenoAbiQueueReply reply{};
        status=createQueue(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodCloseQueue: {
        NativeAdrenoAbiQueue request{};
        memcpy(&request,input,sizeof(request));
        status=closeQueue(&request);
        break;
    }
    case NativeAdrenoMethodSetMetadata: {
        NativeAdrenoAbiMetadata request{};
        memcpy(&request,input,sizeof(request));
        status=setMetadata(&request);
        break;
    }
    case NativeAdrenoMethodGetMetadata: {
        NativeAdrenoAbiMetadata request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiMetadata reply{};
        status=getMetadata(&request,&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    case NativeAdrenoMethodGetStatus: {
        NativeAdrenoAbiStatus reply{};
        status=this->status(&reply);
        if(!status)memcpy(output,&reply,sizeof(reply));
        break;
    }
    default:return kIOReturnUnsupported;
    }
    tally_result=status;
    return status;
}
IOReturn NativeAdrenoUserClient::cpuMap(const NativeAdrenoAbiBoCpuMap *request,
    NativeAdrenoAbiBoCpuMapReply *reply,bool controlled)
{
    if(!request || !reply)return kIOReturnBadArgument;*reply={};
    if(!open_)return kIOReturnNotReady;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    /* An imported BO is the caller's own memory, already mapped where it came from. */
    if(entry->memory.imported())return kIOReturnUnsupported;
    if(controlled && !entry->memory.descriptor())return kIOReturnUnsupported;
    uint32_t &leases=controlled?entry->controlled_mapping_leases:entry->mapping_leases;
    if(leases==UINT32_MAX)return kIOReturnNoResources;
    ++leases;
    reply->token=entry->token;reply->bytes=entry->memory.bytes();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::cpuUnmap(const NativeAdrenoAbiBoCpuUnmap *request,bool controlled)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(request->reserved)return kIOReturnBadArgument;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable || !entry->token)return kIOReturnBadArgument;
    if(request->token!=entry->token)return kIOReturnBadArgument;
    uint32_t &leases=controlled?entry->controlled_mapping_leases:entry->mapping_leases;
    if(!leases)return kIOReturnBusy;
    --leases;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::createQueue(NativeAdrenoAbiQueueReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    for(size_t i=1;i<NativeAdrenoAbiMaxQueues;++i)if(!queues_[i].active) {
        queues_[i]={static_cast<uint32_t>(i),true};
        reply->queue=i;
        return kIOReturnSuccess;
    }
    return kIOReturnNoResources;
}
IOReturn NativeAdrenoUserClient::closeQueue(const NativeAdrenoAbiQueue *request)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(!request->queue || request->queue>=NativeAdrenoAbiMaxQueues)return kIOReturnBadArgument;
    Queue &queue=queues_[request->queue];
    if(!queue.active)return kIOReturnBadArgument;
    /* A queue must be drained by the caller before it disappears. */
    if(submit_->queueOutstanding(queue_base_+static_cast<uint32_t>(request->queue)))
        return kIOReturnBusy;
    queue={};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::setMetadata(const NativeAdrenoAbiMetadata *request)
{
    if(!request)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(request->reserved || request->bytes>NativeAdrenoAbiMetadataBytes)return kIOReturnBadArgument;
    Entry *entry=find(request->handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    if(request->bytes)memcpy(entry->metadata,request->text,request->bytes);
    entry->metadata_bytes=request->bytes;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::getMetadata(const NativeAdrenoAbiMetadata *request,
    NativeAdrenoAbiMetadata *reply)
{
    if(!request || !reply)return kIOReturnBadArgument;
    /* Snapshot the request so an in/out alias is still well defined. */
    const uint64_t handle=request->handle;
    const uint32_t asked=request->bytes;
    const uint32_t reserved=request->reserved;
    *reply={};
    if(!open_)return kIOReturnNotReady;
    if(reserved || asked>NativeAdrenoAbiMetadataBytes)return kIOReturnBadArgument;
    Entry *entry=find(handle);
    if(!entry || entry->unreleasable)return kIOReturnBadArgument;
    if(asked<entry->metadata_bytes)return kIOReturnBadArgument;
    reply->handle=entry->handle;reply->bytes=entry->metadata_bytes;
    if(entry->metadata_bytes)memcpy(reply->text,entry->metadata,entry->metadata_bytes);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoUserClient::waitFence(const NativeAdrenoAbiWaitFence *request,
    const NativeAdrenoRingClock *clock)
{
    if(!request || !clock)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    if(!request->fence || request->fence>UINT32_MAX)return kIOReturnBadArgument;
    const uint32_t timeout_us=request->timeout_ns==0?0:
        (request->timeout_ns>UINT64_C(0xffffffff)*1000u?0xffffffffu:
        (uint32_t)(request->timeout_ns/1000));
    return submit_->wait((uint32_t)request->fence,clock,timeout_us);
}
