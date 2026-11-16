/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoVm.h"
#include "NativeAdrenoVmAlias.h"
extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {int (*native_adreno_dram_read32)(uint64_t,uint32_t *);int (*native_adreno_cached_read32)(uint64_t,uint32_t *);
    int (*native_adreno_line_audit)(uint64_t,size_t,uint32_t *,uint32_t *);}
#include <string.h>
#include <IOKit/IOLib.h>
static bool overlap(uint64_t a,uint64_t ab,uint64_t b,uint64_t bb)
{return a<b+bb && b<a+ab;}
/* Mappings of one cache/shareability/privilege class may share pages; only pages shared across classes refuse. */
static unsigned aliasClass(unsigned permissions,unsigned writeback,unsigned shareability)
{return writeback|(shareability<<2)|((permissions&ArmSmmu4KPrivileged)?16u:0u);}
static bool validBinding(const NativeAdrenoVmRegion &r,unsigned upper)
{
    return r.tables && r.arena && r.state && !r.tables->quarantined() &&
        r.tables->deviceReferences() && r.tables->spanCount()==1 &&
        r.tables->spans()[0].bytes==r.tables->bytes() && !r.tables->spans()[0].offset &&
        r.arena->mapping==r.tables->mapping() && r.arena->upper_region==upper &&
        r.arena->config.table_physical==r.tables->spans()[0].physical &&
        r.arena->capacity_pages==r.tables->bytes()/4096 && r.state->prepared &&
        !r.state->poisoned && !r.state->tables.leaf_pages;
}
ArmSmmu4KLiveRegion NativeAdrenoVm::region(unsigned upper) const
{
    return {regions_[upper].arena,regions_[upper].state,nullptr,0};
}
/* Board hook and per-phase time of the two table transactions, for the log: [0] map, [1] unmap; each row is count, the phases in order, the slowest call
 * and the bytes mapped (int-134: BoMap averaged 135 ms and the desktop crawled). */
extern "C" {uint64_t (*native_adreno_vm_now_us)(void)=nullptr;volatile uint64_t native_adreno_vm_phase_us[2][8];}
namespace {
struct PhaseClock {
    uint64_t last=native_adreno_vm_now_us?native_adreno_vm_now_us():0,began=last;
    uint64_t phases[8]{};
    void lap(unsigned row,unsigned slot) {
        (void)row;
        if(!native_adreno_vm_now_us)return;
        const uint64_t now=native_adreno_vm_now_us();phases[slot]+=now-last;last=now;
    }
    void done(unsigned row,uint64_t bytes) {
        if(!native_adreno_vm_now_us)return;
        const uint64_t total=native_adreno_vm_now_us()-began;
        for(unsigned slot=1;slot<=5;++slot) {
            native_adreno_vm_phase_us[row][slot]+=phases[slot];
        }
        native_adreno_vm_phase_us[row][0]+=1;native_adreno_vm_phase_us[row][7]+=bytes;
        if(total>native_adreno_vm_phase_us[row][6])native_adreno_vm_phase_us[row][6]=total;
    }
};
}
ArmSmmu4KStatus NativeAdrenoVm::observeEmptyTables() const
{
    ArmSmmu4KStreamResult routes;
    ArmSmmu4KStatus status=ArmSmmu4KObserveStreams(&io_,streams_,context_,&routes);if(status)return status;
    ArmSmmu4KLiveRegion lower=region(0),upper=region(1);ArmSmmu4KLiveResult audit;
    status=ArmSmmu4KLiveAuditContext(context_,lower.arena?&lower:nullptr,upper.arena?&upper:nullptr,&audit);
    if(status)return status;
    return ArmSmmu4KObserveStreams(&io_,streams_,context_,&routes);
}
IOReturn NativeAdrenoVm::fail(ArmSmmu4KStatus status)
{
    poisoned_=true;result_.operation=status;result_.complete=false;
    for(size_t i=0;mappings_ && i<mapping_capacity_;++i)if(mappings_[i].memory)mappings_[i].memory->quarantine();
    for(auto &r:regions_)if(r.arena) {r.tables->quarantine();r.state->poisoned=1;}
    ArmSmmu4KStreamResult routes;
    result_.quarantine=ArmSmmu4KFaultStreams(&io_,streams_,&routes);
    return kIOReturnIOError;
}
IOReturn NativeAdrenoVm::attach(const ArmSmmu4KIo *io,ArmSmmu4KContextConfig *context,
    ArmSmmu4KStreamState *streams,const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
    size_t max_objects,unsigned budget)
{
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    static_assert(NativeAdrenoVmMaxObjects*(sizeof(Mapping)+sizeof(AliasMergeState))<=1024*1024,
        "A640 Metal VM mapping metadata exceeds its 1 MiB budget");
#endif
    if(attached_ || mappings_ || poisoned_)return kIOReturnBusy;
    if(!io || !context || !streams || (!lower && !upper) || !max_objects || max_objects>NativeAdrenoVmMaxObjects ||
        !budget || budget>1048576 || (lower && !validBinding(*lower,0)) || (upper && !validBinding(*upper,1)))
        return kIOReturnBadArgument;
    if(!context->coherent_walk &&
        ((lower && !lower->tables->writeCombined()) ||
         (upper && !upper->tables->writeCombined())))return kIOReturnUnsupported;
    mappings_=IONewData(Mapping,max_objects);
    alias_merge_=IONewData(AliasMergeState,max_objects);
    if(!mappings_ || !alias_merge_) {
        if(mappings_) {
            IODeleteData(mappings_,Mapping,max_objects);
            mappings_=nullptr;
        }
        if(alias_merge_) {
            IODeleteData(alias_merge_,AliasMergeState,max_objects);
            alias_merge_=nullptr;
        }
        return kIOReturnNoMemory;
    }
    mapping_capacity_=max_objects;
    memset(mappings_,0,sizeof(Mapping)*mapping_capacity_);
    memset(alias_merge_,0,sizeof(AliasMergeState)*mapping_capacity_);
    for(size_t i=0;i<mapping_capacity_;++i) {
        mappings_[i].next_free=i+1;
    }
    free_head_=0;
    io_=*io;context_=context;streams_=streams;budget_=budget;
    if(lower)regions_[0]=*lower;if(upper)regions_[1]=*upper;
    for(unsigned i=0;i<2;++i)if(regions_[i].arena) {
        if(NativeAdrenoBindSmmuVisibility(regions_[i].tables,&visibility_[i])!=kIOReturnSuccess) {
            IODeleteData(mappings_,Mapping,mapping_capacity_);
            IODeleteData(alias_merge_,AliasMergeState,mapping_capacity_);
            mappings_=nullptr;
            alias_merge_=nullptr;
            regions_[0]={};regions_[1]={};return kIOReturnBadArgument;
        }
    }
    attached_=true;
    ArmSmmu4KStatus status=observeEmptyTables();if(status)return fail(status);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::detach()
{
    if(poisoned_ || objects_ || spanCount())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    IODeleteData(mappings_,Mapping,mapping_capacity_);
    IODeleteData(alias_merge_,AliasMergeState,mapping_capacity_);
    mappings_=nullptr;
    alias_merge_=nullptr;
    mapping_capacity_=0;
    cached_coherent_imports_=false;
    attached_=false;regions_[0]={};regions_[1]={};context_=nullptr;streams_=nullptr;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::enableCachedCoherentImports(uint32_t chip_id)
{
    if(!attached_ || poisoned_) {
        return kIOReturnNotReady;
    }
    /* A642L, and the A640 of nabu: mainline msm gives 0x06040001 ADRENO_QUIRK_HAS_CACHED_COHERENT
     * (a6xx_catalog.c) and the stock SM8150 KGSL gives A640 ADRENO_IOCOHERENT, whose cached imports
     * map with IOMMU_CACHE (kgsl_iommu.c). Owned BOs keep their Normal-NC policy (NabuGpuMemoryPolicy.h). */
    if(chip_id!=0x06030500 && chip_id!=0x06040001) {
        return kIOReturnUnsupported;
    }

    cached_coherent_imports_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *out) const
{
    if(!out)return kIOReturnBadArgument;*out=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(upper>1 || !regions_[upper].arena || !bytes || (bytes&4095) || alignment<4096 ||
        (alignment&(alignment-1)))return kIOReturnBadArgument;
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    uint64_t offset=0;
    for(const Mapping *record=lowerBound(a->iova);;record=record->next) {
        uint64_t rem=(a->iova+offset)&(alignment-1),padding=rem?alignment-rem:0;
        if(padding>a->bytes-offset)return kIOReturnNoResources;offset+=padding;
        if(bytes>a->bytes-offset)return kIOReturnNoResources;
        if(!record || record->iova-a->iova>=a->bytes ||
            (record->iova-a->iova>=offset && bytes<=record->iova-a->iova-offset))
            {*out=a->iova+offset;return kIOReturnSuccess;}
        uint64_t end=record->iova-a->iova+record->memory->bytes();
        if(end>offset)offset=end;
    }
}
IOReturn NativeAdrenoVm::preflight(const NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability) const
{
    if(!memory || upper>1 || !regions_[upper].arena || !memory->backed() || memory->quarantined() ||
        !memory->bytes() || !memory->spanCount() || !(permissions&ArmSmmu4KRead) || (permissions&~15u) ||
        writeback>2 || (shareability!=2 && shareability!=3))return kIOReturnBadArgument;
    const bool firmware=memory->cpuCacheMode()==NativeAdrenoCpuCacheMode::FirmwareUncached;
    if(firmware && (!memory->imported() || memory->spanCount()!=1 || memory->readOnly() ||
        permissions!=(ArmSmmu4KRead|ArmSmmu4KWrite) || writeback!=0 ||
        memory->spans()[0].writeback!=0)) return kIOReturnUnsupported;
    if(memory->cpuCacheMode()==NativeAdrenoCpuCacheMode::Unknown || shareability!=2) {
        /* l747: cached(-mixed) imports no longer depend on the
         * cached_coherent_imports_ flag. The board supports them (the A640
         * IOCOHERENT quirk; bring-up hardcodes enableCachedCoherentImports)
         * and the flag only gated on a bring-up ordering race - CA's
         * Cached-class render targets were refused with NotRepresentable
         * while the same class rendered in final's boots. */
        return kIOReturnUnsupported;
    }
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    if((iova&4095) || iova<a->iova || iova-a->iova>=a->bytes || memory->bytes()>a->bytes-(iova-a->iova))
        return kIOReturnBadArgument;
    if(objects_+reserved_objects_>=mapping_capacity_) {
        return kIOReturnNoSpace;
    }
    const Mapping *next=lowerBound(iova);
    const Mapping *previous=next?next->previous:last_;
    if(next && next->iova-iova<memory->bytes()) {
        return kIOReturnBusy;
    }
    if(previous && iova-previous->iova<previous->memory->bytes()) {
        return kIOReturnBusy;
    }
    if(a->config.output_bits<32 || a->config.output_bits>48)return kIOReturnBadArgument;
    uint64_t offset=0,limit=UINT64_C(1)<<a->config.output_bits;
    for(size_t j=0;j<memory->spanCount();++j) {
        const auto &span=memory->spans()[j];
        if(span.offset!=offset || !span.physical || !span.bytes || ((span.physical|span.bytes)&4095) ||
            span.bytes>memory->bytes()-offset || span.physical>=limit || span.bytes>limit-span.physical)
            return kIOReturnBadArgument;
        for(const auto &r:regions_)if(r.arena && overlap(span.physical,span.bytes,
            r.arena->config.table_physical,r.arena->capacity_pages*UINT64_C(4096)))return kIOReturnBadArgument;
        offset+=span.bytes;
    }
    if(offset!=memory->bytes())return kIOReturnBadArgument;
    /* The class counts skip physical comparisons when all BOs use the same
     * attributes. Conflicting objects keep a sorted physical index, so a small
     * new BO queries that index without visiting the object's other pages. */
    const unsigned mine=aliasClass(permissions,writeback,shareability);
    if(writeback!=2 && class_objects_[mine]==objects_) {
        return kIOReturnSuccess;
    }
    uint32_t *order=a->alias_scratch;
    size_t new_count=0;
    for(const Mapping *record=first_;record;record=record->next) {
        const Mapping &other=*record;
        if(writeback!=2 && aliasClass(other.permissions,other.writeback,other.shareability)==mine) {
            continue;
        }
        const bool attributes_differ=shareability!=other.shareability ||
            ((permissions^other.permissions)&ArmSmmu4KPrivileged);
        if(memory->spanCount()<=other.spans || !order || memory->spanCount()>a->alias_scratch_entries) {
            NativeAdrenoPhysicalIndex index(other.memory->spans(),other.physical_order,other.physical_count,true);
            for(size_t j=0;j<memory->spanCount();++j) {
                const auto &span=memory->spans()[j];
                if(attributes_differ?index.overlaps(span.physical,span.bytes):
                    index.conflictingCache(span.physical,span.bytes,span.writeback)) {
                    return kIOReturnBadArgument;
                }
            }
        } else {
            if(!new_count) {
                for(size_t j=0;j<memory->spanCount();++j) {
                    order[j]=(uint32_t)j;
                }
                NativeAdrenoPhysicalIndex index(memory->spans(),order,memory->spanCount());
                new_count=index.count();
            }
            NativeAdrenoPhysicalIndex index(memory->spans(),order,new_count,true);
            for(size_t j=0;j<other.spans;++j) {
                const auto &span=other.memory->spans()[j];
                if(attributes_differ?index.overlaps(span.physical,span.bytes):
                    index.conflictingCache(span.physical,span.bytes,span.writeback)) {
                    return kIOReturnBadArgument;
                }
            }
        }
    }
    return kIOReturnSuccess;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t handle) const
{
    if(!handle || !mappings_) {
        return nullptr;
    }
    const size_t slot=handle&(NativeAdrenoVmMaxObjects-1);
    if(slot>=mapping_capacity_) {
        return nullptr;
    }
    Mapping &record=mappings_[slot];
    return record.handle==handle && record.memory?&record:nullptr;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::lowerBound(uint64_t iova) const
{
    Mapping *record=RB_ROOT(&addresses_.root),*next=nullptr;
    while(record) {
        if(record->iova<iova) {
            record=RB_RIGHT(record,address_link);
        } else {
            next=record;
            record=RB_LEFT(record,address_link);
        }
    }
    return next;
}
const NativeAdrenoVm::Mapping *NativeAdrenoVm::findAddressRecord(uint64_t iova) const
{
    const Mapping *next=lowerBound(iova);
    if(next && next->iova==iova) {
        return next;
    }
    const Mapping *previous=next?next->previous:last_;
    if(previous && iova-previous->iova<previous->memory->bytes()) {
        return previous;
    }
    return nullptr;
}
void NativeAdrenoVm::insert(Mapping *record,unsigned upper)
{
    record->next=lowerBound(record->iova);
    record->previous=record->next?record->next->previous:last_;
    if(record->next) {
        record->next->previous=record;
    } else {
        last_=record;
    }
    if(record->previous) {
        record->previous->next=record;
    } else {
        first_=record;
    }
    addresses_.Root_RB_INSERT(&addresses_.root,record);
    ++objects_;
    ++class_objects_[aliasClass(record->permissions,record->writeback,record->shareability)];
    if(upper) {
        upper_count_+=record->spans;
    } else {
        lower_count_+=record->spans;
    }
}
void NativeAdrenoVm::erase(Mapping *record,unsigned upper)
{
    addresses_.Root_RB_REMOVE(&addresses_.root,record);
    if(record->next) {
        record->next->previous=record->previous;
    } else {
        last_=record->previous;
    }
    if(record->previous) {
        record->previous->next=record->next;
    } else {
        first_=record->next;
    }
    --objects_;
    --class_objects_[aliasClass(record->permissions,record->writeback,record->shareability)];
    if(upper) {
        upper_count_-=record->spans;
    } else {
        lower_count_-=record->spans;
    }
}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle)
{
    if(!handle)return kIOReturnBadArgument;*handle=0;result_={};
    map_busy_alias_=false;
    map_alias_limit_=false;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!memory || writeback>1 || (writeback && !memory->cpuCacheCoherent())) {
        return kIOReturnBadArgument;
    }
    writeback=memory->cpuCacheMode()==NativeAdrenoCpuCacheMode::Mixed?2u:
        memory->cpuCacheCoherent()?1u:0u;
    PhaseClock clock;
    IOReturn checked=preflight(memory,upper,iova,permissions,writeback,shareability);if(checked)return checked;
    if(next_handle_>(UINT64_MAX>>12) || free_head_>=mapping_capacity_) {
        return kIOReturnNoResources;
    }
    clock.lap(0,5);

    const size_t spans=memory->spanCount();
    ArmSmmu4KRange *changes=IONewData(ArmSmmu4KRange,spans);
    uint32_t *physical_order=IONewData(uint32_t,spans);
    PhysicalRange *physical_ranges=IONewData(PhysicalRange,spans);
    if(!changes || !physical_order || !physical_ranges) {
        if(changes) {
            IODeleteData(changes,ArmSmmu4KRange,spans);
        }
        if(physical_order) {
            IODeleteData(physical_order,uint32_t,spans);
        }
        if(physical_ranges) {
            IODeleteData(physical_ranges,PhysicalRange,spans);
        }
        return kIOReturnNoMemory;
    }
    for(size_t i=0;i<spans;++i) {
        const auto &span=memory->spans()[i];
        changes[i]={iova+span.offset,span.physical,span.bytes,permissions,span.writeback,shareability};
        physical_order[i]=(uint32_t)i;
    }
    NativeAdrenoPhysicalIndex index(memory->spans(),physical_order,spans);
    Mapping *record=&mappings_[free_head_];
    MappingAlias *aliases=nullptr;
    size_t alias_count=0;
    checked=buildMappingAliases(record,memory,physical_ranges,&aliases,&alias_count);
    if(checked) {
        map_alias_limit_=checked==kIOReturnNoResources;
        IODeleteData(changes,ArmSmmu4KRange,spans);
        IODeleteData(physical_order,uint32_t,spans);
        IODeleteData(physical_ranges,PhysicalRange,spans);
        return checked;
    }
    checked=memory->holdForDevice();
    if(checked) {
        IODeleteData(changes,ArmSmmu4KRange,spans);
        IODeleteData(physical_order,uint32_t,spans);
        IODeleteData(physical_ranges,PhysicalRange,spans);
        if(aliases) {
            IODeleteData(aliases,MappingAlias,alias_count);
        }
        return checked;
    }

    free_head_=record->next_free;
    ++reserved_objects_;
    ++budget_generation_;
    *record={};
    record->memory=memory;
    record->ranges=changes;
    record->physical_order=physical_order;
    record->physical_ranges=physical_ranges;
    record->alias_storage=aliases;
    record->alias_count=alias_count;
    alias_storage_count_+=alias_count;
    record->physical_count=index.count();
    record->handle=(next_handle_++<<12)|size_t(record-mappings_);
    record->iova=iova;
    record->spans=spans;
    record->permissions=permissions;
    record->writeback=writeback;
    record->shareability=shareability;
    clock.lap(0,1);
    __asm__ volatile("dsb st" ::: "memory");
    clock.lap(0,2);
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    arm_smmu_4k_live_now_us=native_adreno_vm_now_us;
    const ArmSmmu4KStatus status=ArmSmmu4KLiveUpdateContextRanges(&io_,streams_->bank,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],changes,spans,1,budget_,&update);
    if(status)return fail(status);
    clock.lap(0,3);
    insert(record,upper);record->mapped_spans=spans;result_.completed_spans=spans;
    for(size_t at=0;at<spans;++at) {
        const auto &span=memory->spans()[at];
        PhysicalRange &range=physical_ranges[at];
        range={};
        range.mapping=record;
        range.physical=span.physical;
        range.end=span.physical+span.bytes;
        range.subtree_end=range.end;
        range.offset=span.offset;
        physical_.PhysicalRoot_RB_INSERT(&physical_.root,&range);
    }
    for(size_t at=0;at<alias_count;++at) {
        linkMappingAlias(aliases[at]);
    }
    --reserved_objects_;
    if(mapping_added_) {
        const IOReturn observed=mapping_added_(mapping_observer_context_,record->handle);
        if(observed) {
            poisoned_=true;
            return observed;
        }
    }
    clock.lap(0,4);clock.done(0,memory->bytes());
    *handle=record->handle;result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::unmap(uint64_t handle)
{
    native_adreno_trace_stage=10;
    result_={};if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs || activeAlias(record->memory,record->handle)) {
        return kIOReturnBusy;
    }
    PhaseClock clock;
    native_adreno_trace_stage=11;
    unsigned upper=regions_[1].arena && record->iova>=regions_[1].arena->iova;
    __asm__ volatile("dsb sy" ::: "memory");
    clock.lap(1,2);
    /* The record retains exactly the ranges installed by map(). */
    const size_t spans=record->spans;
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    native_adreno_trace_stage=12;
    arm_smmu_4k_live_now_us=native_adreno_vm_now_us;
    const ArmSmmu4KStatus status=ArmSmmu4KLiveUpdateContextRanges(&io_,streams_->bank,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],record->ranges,spans,0,budget_,&update);
    native_adreno_trace_stage=13;
    if(status)return fail(status);
    clock.lap(1,3);
    erase(record,upper);record->mapped_spans=0;result_.completed_spans=spans;
    for(size_t at=0;at<spans;++at) {
        physical_.PhysicalRoot_RB_REMOVE(&physical_.root,&record->physical_ranges[at]);
    }
    while(record->aliases) {
        unlinkMappingAlias(*record->aliases);
    }
    native_adreno_trace_stage=14;
    clock.lap(1,4);
    if(backing_release_observer_) {
        backing_release_observer_(handle);
    }
    if(mapping_removed_) {
        mapping_removed_(mapping_observer_context_,handle);
    }
    if(record->memory->retireAfterQuiescence()!=kIOReturnSuccess)return fail(ArmSmmu4KStateMismatch);
    IODeleteData(record->ranges,ArmSmmu4KRange,spans);
    IODeleteData(record->physical_order,uint32_t,spans);
    IODeleteData(record->physical_ranges,PhysicalRange,spans);
    if(record->alias_storage) {
        IODeleteData(record->alias_storage,MappingAlias,record->alias_count);
        alias_storage_count_-=record->alias_count;
    }
    clock.lap(1,5);clock.done(1,record->memory->bytes());
    *record={};
    record->next_free=free_head_;
    free_head_=size_t(record-mappings_);
    ++budget_generation_;
    result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::describe(uint64_t handle,uint64_t *iova,size_t *bytes,
    unsigned *permissions) const
{
    if(!iova || !bytes || !permissions)return kIOReturnBadArgument;
    *iova=0;*bytes=0;*permissions=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(const Mapping *record=find(handle)) {
        const Mapping &mapping=*record;
        *iova=mapping.iova;*bytes=mapping.memory->bytes();*permissions=mapping.permissions;
        return mapping.mapped_spans==mapping.spans?kIOReturnSuccess:kIOReturnNotReady;
    }
    return kIOReturnBadArgument;
}
/* TLBIALL of the bank before a submission. Where a table update has observed the bank's image (each region's whole-verification
 * update remembers its register offset), the invalidation alone: the image and faults were observed there and every 64th table
 * operation since (int-159: a submission spent 1.3 ms in the kernel, most of it reading SMMU registers). */
IOReturn NativeAdrenoVm::invalidateTlb()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    ArmSmmu4KProgramResult r;
    uint64_t offset=0;
    for(const auto &region:regions_)if(region.state && region.state->context_offset)offset=region.state->context_offset;
    const ArmSmmu4KStatus status=offset?ArmSmmu4KSyncContextTlb(&io_,offset,context_->asid,1,budget_,&r):
        ArmSmmu4KInvalidateAllOwnedContext(&io_,streams_->bank,context_,budget_,&r);
    return status?fail(status):kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const
{
    if(!cached || !dram)return kIOReturnBadArgument;
    *cached=*dram=~0u;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(iova&3)return kIOReturnBadArgument;
    if(const Mapping *record=findAddressRecord(iova)) {
        const Mapping &m=*record;
        const size_t offset=(size_t)(iova-m.iova);
        if(m.memory->mapping())*cached=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4];
        for(size_t j=0;j<m.memory->spanCount();++j) {
            const auto &span=m.memory->spans()[j];
            if(offset<span.offset || offset-span.offset>=span.bytes)continue;
            uint32_t value=0;
            /* DRAM first: the cached reader writes its line back, so read after it DRAM would always agree (int-099). */
            if(native_adreno_dram_read32 && !native_adreno_dram_read32(span.physical+(offset-span.offset),&value))*dram=value;
            /* A write-combined BO's own mapping is non-cacheable: read the cacheable alias instead, so a dirty line is still seen. */
            if(native_adreno_cached_read32 && !native_adreno_cached_read32(span.physical+(offset-span.offset),&value))*cached=value;
            break;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::peekWords(uint64_t iova,uint32_t *out,size_t count) const
{
    if(!out || !attached_ || poisoned_)return kIOReturnBadArgument;
    for(size_t k=0;k<count;++k)out[k]=0;
    if(const Mapping *record=findAddressRecord(iova)) {
        const Mapping &m=*record;
        if(!m.memory->mapping()) {
            return kIOReturnBadArgument;
        }
        const size_t offset=(size_t)(iova-m.iova);
        for(size_t k=0;k<count && offset+4*k+4<=m.memory->bytes();++k)out[k]=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4+k];
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
bool NativeAdrenoVm::activeAlias(const NativeAdrenoMemory *memory,uint64_t excluded) const
{
    for(const Mapping *record=first_;record;record=record->next) {
        if(!record->jobs || record->handle==excluded ||
            (!memory->imported() && !record->memory->imported())) {
            continue;
        }
        NativeAdrenoPhysicalIndex index(record->memory->spans(),record->physical_order,record->physical_count,true);
        for(size_t at=0;at<memory->spanCount();++at) {
            const NativeAdrenoPhysicalSpan &span=memory->spans()[at];
            if(index.overlaps(span.physical,span.bytes)) {
                return true;
            }
        }
    }
    return false;
}

void NativeAdrenoVm::addClient()
{
    ++clients_;
    ++budget_generation_;
}

void NativeAdrenoVm::removeClient()
{
    if(clients_) {
        --clients_;
        ++budget_generation_;
    }
}


bool NativeAdrenoVm::resourceHasUncachedImport(uint64_t handle) const
{
    const Mapping *record=find(handle);
    return record && record->memory->imported() && !record->memory->cpuCacheCoherent();
}

bool NativeAdrenoVm::resourceImported(uint64_t handle) const
{
    const Mapping *record=find(handle);
    return record && record->memory->imported();
}

const NativeAdrenoMemory *NativeAdrenoVm::resourceBacking(uint64_t handle) const
{
    const Mapping *record=find(handle);
    return record && record->mapped_spans==record->spans?record->memory:nullptr;
}

bool NativeAdrenoVm::collectPhysicalAliases(const PhysicalRange *range,uint64_t first,uint64_t end,
    size_t offset,AliasBuild &build)
{
    if(!range || range->subtree_end<=first) {
        return false;
    }
    if(collectPhysicalAliases(RB_LEFT(range,link),first,end,offset,build)) {
        return true;
    }
    if(range->physical>=end) {
        return false;
    }
    if(range->end>first) {
        const uint64_t begin=range->physical>first?range->physical:first;
        const uint64_t limit=range->end<end?range->end:end;
        if(build.hits==NativeAdrenoAliasHitLimit) {
            build.status=kIOReturnNoResources;
            return true;
        }
        ++build.hits;
        const size_t source_offset=offset+size_t(begin-first);
        const size_t other_offset=range->offset+size_t(begin-range->physical);
        const size_t bytes=size_t(limit-begin);
        if(range->mapping!=build.owner || source_offset<other_offset) {
            const size_t slot=size_t(range->mapping-mappings_);
            AliasMergeState &last=alias_merge_[slot];
            if(last.generation==build.generation && last.offset+last.bytes==source_offset &&
                last.other_offset+last.bytes==other_offset) {
                last.bytes+=bytes;
                if(build.output) {
                    build.output[last.index].bytes=last.bytes;
                }
            } else {
                if(build.count==build.limit) {
                    build.status=kIOReturnNoResources;
                    return true;
                }
                last={build.generation,source_offset,other_offset,bytes,build.count};
                if(build.output) {
                    MappingAlias &alias=build.output[build.count];
                    alias={};
                    alias.mapping[0]=build.owner;
                    alias.mapping[1]=range->mapping;
                    alias.offset[0]=source_offset;
                    alias.offset[1]=other_offset;
                    alias.bytes=bytes;
                }
                ++build.count;
            }
        }
    }
    return collectPhysicalAliases(RB_RIGHT(range,link),first,end,offset,build);
}

IOReturn NativeAdrenoVm::buildMappingAliases(Mapping *owner,const NativeAdrenoMemory *memory,
    PhysicalRange *ranges,MappingAlias **out,size_t *out_count)
{
    *out=nullptr;
    *out_count=0;
    PhysicalTree own;
    const size_t spans=memory->spanCount();
    for(size_t at=0;at<spans;++at) {
        const auto &span=memory->spans()[at];
        PhysicalRange &range=ranges[at];
        range={};
        range.mapping=owner;
        range.physical=span.physical;
        range.end=span.physical+span.bytes;
        range.subtree_end=range.end;
        range.offset=span.offset;
        own.PhysicalRoot_RB_INSERT(&own.root,&range);
    }

    MappingAlias *storage=nullptr;
    size_t allocated=0;
    for(unsigned pass=0;pass<2;++pass) {
        if(++alias_build_generation_==0) {
            memset(alias_merge_,0,sizeof(AliasMergeState)*mapping_capacity_);
            alias_build_generation_=1;
        }
        AliasBuild build{owner,storage,0,NativeAdrenoAliasRangeLimit-alias_storage_count_,
            alias_build_generation_,0,kIOReturnSuccess};
        if(pass) {
            build.limit=allocated;
        }
        for(size_t at=0;at<spans && !build.status;++at) {
            const auto &span=memory->spans()[at];
            if(collectPhysicalAliases(RB_ROOT(&physical_.root),span.physical,span.physical+span.bytes,
                span.offset,build)) {
                break;
            }
            collectPhysicalAliases(RB_ROOT(&own.root),span.physical,span.physical+span.bytes,span.offset,build);
        }
        if(build.status || (pass && build.count!=allocated)) {
            if(storage) {
                IODeleteData(storage,MappingAlias,allocated);
            }
            return build.status?build.status:kIOReturnInternalError;
        }
        if(!pass) {
            allocated=build.count;
            if(!allocated) {
                return kIOReturnSuccess;
            }
            storage=IONewData(MappingAlias,allocated);
            if(!storage) {
                return kIOReturnNoMemory;
            }
        }
    }
    *out=storage;
    *out_count=allocated;
    return kIOReturnSuccess;
}

void NativeAdrenoVm::linkMappingAlias(MappingAlias &alias)
{
    for(unsigned side=0;side<2;++side) {
        Mapping *mapping=alias.mapping[side];
        if(side && mapping==alias.mapping[0]) {
            continue;
        }
        MappingAlias *head=mapping->aliases;
        alias.next[side]=head;
        if(head) {
            const unsigned head_side=head->mapping[0]==mapping?0:1;
            head->previous[head_side]=&alias;
        }
        mapping->aliases=&alias;
    }
}

void NativeAdrenoVm::unlinkMappingAlias(MappingAlias &alias)
{
    for(unsigned side=0;side<2;++side) {
        Mapping *mapping=alias.mapping[side];
        if(side && mapping==alias.mapping[0]) {
            continue;
        }
        MappingAlias *previous=alias.previous[side],*next=alias.next[side];
        if(previous) {
            const unsigned previous_side=previous->mapping[0]==mapping?0:1;
            previous->next[previous_side]=next;
        } else {
            mapping->aliases=next;
        }
        if(next) {
            const unsigned next_side=next->mapping[0]==mapping?0:1;
            next->previous[next_side]=previous;
        }
    }
    alias={};
}

IOReturn NativeAdrenoVm::visitResourceAliases(uint64_t handle,size_t offset,size_t bytes,size_t cache_line,
    bool (*visit)(void *,uint64_t,size_t,size_t),void *context,uint64_t *queries,uint64_t *hits) const
{
    const Mapping *record=find(handle);
    if(!record || !visit || !queries || !hits || !bytes || !cache_line || cache_line>4096 ||
        (cache_line&(cache_line-1)) || offset>record->memory->bytes() || bytes>record->memory->bytes()-offset) {
        return kIOReturnBadArgument;
    }
    const size_t begin=offset&~(cache_line-1);
    const size_t end=(offset+bytes+cache_line-1)&~(cache_line-1);
    if(*queries==NativeAdrenoAliasQueryLimit || *hits==NativeAdrenoAliasHitLimit) {
        return kIOReturnBusy;
    }
    ++*queries;
    ++*hits;
    if(visit(context,handle,begin,end-begin)) {
        return kIOReturnBusy;
    }
    for(const MappingAlias *alias=record->aliases;alias;) {
        const unsigned linked_side=alias->mapping[0]==record?0:1;
        const MappingAlias *next=alias->next[linked_side];
        for(unsigned side=0;side<2;++side) {
            if(alias->mapping[side]!=record) {
                continue;
            }
            if(*queries==NativeAdrenoAliasQueryLimit) {
                return kIOReturnBusy;
            }
            ++*queries;
            const size_t alias_end=alias->offset[side]+alias->bytes;
            if(begin>=alias_end || alias->offset[side]>=end) {
                continue;
            }
            const size_t first=begin>alias->offset[side]?begin:alias->offset[side];
            const size_t limit=end<alias_end?end:alias_end;
            const size_t translated=alias->offset[side^1]+first-alias->offset[side];
            if(*hits==NativeAdrenoAliasHitLimit) {
                return kIOReturnBusy;
            }
            ++*hits;
            if(visit(context,alias->mapping[side^1]->handle,translated,limit-first)) {
                return kIOReturnBusy;
            }
        }
        alias=next;
    }
    return kIOReturnSuccess;
}

bool NativeAdrenoVm::visitPhysical(const PhysicalRange *range,uint64_t first,uint64_t end,
    bool (*visit)(void *,uint64_t,size_t,size_t),void *context)
{
    if(!range || range->subtree_end<=first) {
        return false;
    }
    if(visitPhysical(RB_LEFT(range,link),first,end,visit,context)) {
        return true;
    }
    if(range->physical>=end) {
        return false;
    }
    if(range->end>first) {
        const uint64_t begin=range->physical>first?range->physical:first;
        const uint64_t limit=range->end<end?range->end:end;
        if(visit(context,range->mapping->handle,
            range->offset+size_t(begin-range->physical),size_t(limit-begin))) {
            return true;
        }
    }
    return visitPhysical(RB_RIGHT(range,link),first,end,visit,context);
}

IOReturn NativeAdrenoVm::visitPhysicalAliases(uint64_t physical,size_t bytes,
    bool (*visit)(void *,uint64_t,size_t,size_t),void *context) const
{
    if(!physical || !bytes || physical>UINT64_MAX-bytes || !visit) {
        return kIOReturnBadArgument;
    }
    return visitPhysical(RB_ROOT(&physical_.root),physical,physical+bytes,visit,context)?
        kIOReturnNoResources:kIOReturnSuccess;
}

IOReturn NativeAdrenoVm::holdJob(uint64_t handle)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs==UINT32_MAX)return kIOReturnNoResources;
    IOReturn status=record->memory->holdForDevice();if(status)return status;
    ++record->jobs;return kIOReturnSuccess;
}
bool NativeAdrenoVm::resourceBusy(uint64_t handle) const
{
    const Mapping *record=find(handle);
    return record && record->jobs;
}
IOReturn NativeAdrenoVm::retireJobAfterCompletion(uint64_t handle)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record || !record->jobs)return kIOReturnBadArgument;
    IOReturn status=record->memory->retireAfterQuiescence();if(status)return status;
    --record->jobs;return kIOReturnSuccess;
}
