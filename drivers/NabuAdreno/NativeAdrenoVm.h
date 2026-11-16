/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_VM_H
#define NATIVE_ADRENO_VM_H
#include "NativeAdrenoSmmuMemory.h"
#include "ArmSmmu4KStreams.h"
#include <libkern/tree.h>
struct NativeAdrenoVmRegion {
    NativeAdrenoMemory *tables;
    ArmSmmu4KLiveArena *arena;
    ArmSmmu4KLiveState *state;
};
struct NativeAdrenoVmResult {
    ArmSmmu4KStatus operation,quarantine;
    size_t completed_spans;
    bool complete;
};
/* NativeAdrenoVm owns the CPU records for BOs mapped into the GPU context.
 * Each record retains its backing memory, physical ranges and a physical-address
 * index. Generation handles locate records directly. Mapping validates the BO
 * against its GPU address neighbors, physical attributes and page-table pools,
 * then records shared physical intervals as pairs of BO offsets before asking
 * ArmSmmu4K to install its leaves. Submission follows these recorded intervals
 * to find overlapping job resources. Unmapping completes TLB synchronization
 * and removes both ends of each alias before releasing the memory reference.
 *
 * The service serializes these operations and owns the powered context, streams
 * and page-table allocations. NativeAdrenoSubmit retains job references until
 * the GPU fence completes. A failed page-table transaction keeps the records
 * and memory, faults the owned streams and requires service recovery. */
enum : size_t {
    NativeAdrenoVmMaxObjects=4096,
    NativeAdrenoAliasQueryLimit=262144,
    NativeAdrenoAliasHitLimit=1048576,
    NativeAdrenoAliasRangeLimit=262144
};
/* Board hook for sampleWord(): reads one aligned word of physical memory through a fresh uncached mapping; 0 on success. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical,uint32_t *value);
/* The board's reader through the kernel's cacheable physical aperture (null on the host: the BO's own mapping is read). */
extern "C" int (*native_adreno_cached_read32)(uint64_t physical,uint32_t *value);
/* Board hook for auditRange(): [physical, physical+bytes), whole 64-byte lines inside one page, as the cacheable physical aperture and
 * DRAM (a fresh uncached mapping) read it. A line that differs is cleaned and invalidated and DRAM read again: when DRAM then holds the
 * cached copy the line was dirty (*dirty), when DRAM is unchanged it was a stale clean copy (*stale). 0 on success. */
extern "C" int (*native_adreno_line_audit)(uint64_t physical,size_t bytes,uint32_t *dirty,uint32_t *stale);
class NativeAdrenoVm final {
    struct PhysicalRange;
    struct MappingAlias;
    struct Mapping {
        NativeAdrenoMemory *memory=nullptr;
        ArmSmmu4KRange *ranges=nullptr;
        uint32_t *physical_order=nullptr;
        PhysicalRange *physical_ranges=nullptr;
        MappingAlias *aliases=nullptr; /* All live intervals involving this mapping. */
        MappingAlias *alias_storage=nullptr; /* One allocation for intervals discovered by this map. */
        size_t alias_count=0; /* Allocated slots, including intervals whose other mapping was removed. */
        size_t physical_count=0;
        uint64_t handle=0,iova=0;
        size_t spans=0,mapped_spans=0;
        unsigned permissions=0,writeback=0,shareability=0,jobs=0;
        size_t next_free=0;
        RB_ENTRY(Mapping) address_link;
        Mapping *previous=nullptr,*next=nullptr;
    };
    struct AddressTree {
        RB_HEAD(Root,Mapping) root{};
        static int compare(Mapping *left,Mapping *right) {
            if(left->iova<right->iova) {
                return -1;
            }
            return left->iova>right->iova?1:0;
        }
        RB_GENERATE(Root,Mapping,address_link,compare)
    } addresses_;
    struct PhysicalRange {
        Mapping *mapping=nullptr;
        uint64_t physical=0,end=0,subtree_end=0;
        size_t offset=0;
        RB_ENTRY(PhysicalRange) link;
        void augment() {
            subtree_end=end;
            const PhysicalRange *left=RB_LEFT(this,link),*right=RB_RIGHT(this,link);
            if(left && left->subtree_end>subtree_end) {
                subtree_end=left->subtree_end;
            }
            if(right && right->subtree_end>subtree_end) {
                subtree_end=right->subtree_end;
            }
        }
    };
    /* PhysicalTree contains each mapped span once. Augmented ends let a byte
     * interval find overlapping mappings without scanning other BOs. */
    struct PhysicalTree {
        RB_HEAD(PhysicalRoot,PhysicalRange) root{};
        void augment(PhysicalRange *range) {
            while(range) {
                range->augment();
                range=PhysicalRoot_RB_GETPARENT(range);
            }
        }
        static int compare(PhysicalRange *left,PhysicalRange *right) {
            if(left->physical!=right->physical) {
                return left->physical<right->physical?-1:1;
            }
            if(left==right) {
                return 0;
            }
            return uintptr_t(left)<uintptr_t(right)?-1:1;
        }
#pragma push_macro("RB_AUGMENT")
#undef RB_AUGMENT
#define RB_AUGMENT(node) augment(node)
        RB_GENERATE(PhysicalRoot,PhysicalRange,link,compare)
#pragma pop_macro("RB_AUGMENT")
    } physical_;
    /* MappingAlias records equal physical bytes at two BO offsets. Its newer
     * mapping owns the storage; both mappings link the record until either is
     * unmapped. Adjacent equal-offset translations share one record. */
    struct MappingAlias {
        Mapping *mapping[2]{};
        size_t offset[2]{},bytes=0;
        MappingAlias *next[2]{},*previous[2]{};
    };
    struct AliasMergeState {
        uint64_t generation=0;
        size_t offset=0,other_offset=0,bytes=0,index=0;
    };
    struct AliasBuild {
        Mapping *owner;
        MappingAlias *output;
        size_t count,limit;
        uint64_t generation,hits;
        IOReturn status;
    };
    Mapping *mappings_=nullptr;
    AliasMergeState *alias_merge_=nullptr;
    size_t alias_storage_count_=0;
    uint64_t alias_build_generation_=0;
    Mapping *first_=nullptr,*last_=nullptr;
    NativeAdrenoVmRegion regions_[2]{};
    ArmSmmu4KLiveVisibility visibility_[2]{};
    ArmSmmu4KIo io_{};
    ArmSmmu4KContextConfig *context_=nullptr;
    ArmSmmu4KStreamState *streams_=nullptr;
    size_t lower_count_=0,upper_count_=0,objects_=0,free_head_=0,mapping_capacity_=0;
    size_t reserved_objects_=0;
    uint32_t clients_=0;
    uint64_t budget_generation_=1;
    size_t class_objects_[32]{};
    unsigned budget_=0;
    uint64_t next_handle_=1;
    bool attached_=false,poisoned_=false;
    bool cached_coherent_imports_=false;
    bool map_busy_alias_=false,map_alias_limit_=false;
    void (*backing_release_observer_)(uint64_t)=nullptr;
    IOReturn (*mapping_added_)(void *,uint64_t)=nullptr;
    void (*mapping_removed_)(void *,uint64_t)=nullptr;
    void *mapping_observer_context_=nullptr;
    NativeAdrenoVmResult result_{};
    ArmSmmu4KLiveRegion region(unsigned) const;
    ArmSmmu4KStatus observeEmptyTables() const;
    IOReturn fail(ArmSmmu4KStatus);
    Mapping *find(uint64_t) const;
    const Mapping *findAddressRecord(uint64_t) const;
    Mapping *lowerBound(uint64_t) const;
    void insert(Mapping *,unsigned);
    void erase(Mapping *,unsigned);
    IOReturn preflight(const NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned) const;
    bool activeAlias(const NativeAdrenoMemory *,uint64_t excluded) const;
    bool collectPhysicalAliases(const PhysicalRange *,uint64_t,uint64_t,size_t,AliasBuild &);
    IOReturn buildMappingAliases(Mapping *,const NativeAdrenoMemory *,PhysicalRange *,MappingAlias **,size_t *);
    static void linkMappingAlias(MappingAlias &);
    static void unlinkMappingAlias(MappingAlias &);
    static bool visitPhysical(const PhysicalRange *,uint64_t,uint64_t,
        bool (*)(void *,uint64_t,size_t,size_t),void *);
public:
    NativeAdrenoVm()=default;
    ~NativeAdrenoVm()=default;
    NativeAdrenoVm(const NativeAdrenoVm &)=delete;
    NativeAdrenoVm &operator=(const NativeAdrenoVm &)=delete;
    IOReturn attach(const ArmSmmu4KIo *,ArmSmmu4KContextConfig *,ArmSmmu4KStreamState *,
        const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
        size_t max_objects,unsigned poll_budget);
    /* Frees CPU metadata only, with no BOs remaining. Never releases roots,
     * disables a bank or returns the platform lease; board lifecycle owns it. */
    IOReturn detach();
    /* A642L permits coherent cached mappings. Each backing records its CPU
     * page attributes before the VM installs the corresponding GPU leaves. */
    IOReturn enableCachedCoherentImports(uint32_t chip_id);
    bool cachedCoherentImports() const {return cached_coherent_imports_;}
    IOReturn findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *iova) const;
    IOReturn map(NativeAdrenoMemory *,unsigned upper,uint64_t iova,
        unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle);
    IOReturn unmap(uint64_t handle);
    IOReturn holdJob(uint64_t handle);
    bool resourceBusy(uint64_t handle) const;
    bool resourceHasUncachedImport(uint64_t handle) const;
    bool resourceImported(uint64_t handle) const;
    /* The borrowed backing remains valid while the service holds its GPU lock.
     * Generation handles reject an unmapped or reused record. */
    const NativeAdrenoMemory *resourceBacking(uint64_t handle) const;
    /* The observer releases diagnostic descriptor references before a mapped
     * backing loses its VM reference. The service serializes the callback. */
    void setBackingReleaseObserver(void (*observer)(uint64_t)) {
        backing_release_observer_=observer;
    }
    void setMappingObserver(IOReturn (*observer)(void *,uint64_t),
        void (*removed)(void *,uint64_t),void *context) {
        mapping_added_=observer;
        mapping_removed_=removed;
        mapping_observer_context_=context;
    }
    /* Physical ranges are visited during display acquisition and map creation.
     * The callback receives the mapped handle and exact overlapping BO bytes. */
    IOReturn visitPhysicalAliases(uint64_t,size_t,
        bool (*)(void *,uint64_t,size_t,size_t),void *) const;
    /* The callback receives the same mapping and each static physical alias at
     * the translated BO interval. A conflict or exhausted query budget returns
     * Busy; the caller retries after active jobs retire. */
    IOReturn visitResourceAliases(uint64_t handle,size_t offset,size_t bytes,size_t cache_line,
        bool (*)(void *,uint64_t,size_t,size_t),void *,uint64_t *queries,uint64_t *hits) const;
    /* TLBIALL for the context bank plus TLBSYNC: drops every cached translation of the bank whatever its ASID. Unmap already
     * synchronizes by ASID; this is the stronger form the submit path uses before each submission (int-070: on sheng the CP read
     * zeros from an IB whose table entry and DRAM were both correct, after the address had been mapped to other pages before). */
    IOReturn invalidateTlb();
    /* Diagnostic, never a proof of coherence: the 32-bit word at a mapped GPU address as the kernel's cacheable alias reads it
     * and, when the board installed native_adreno_dram_read32, as DRAM reads it through an uncached mapping (else ~0). */
    IOReturn sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const;
    /* Read-only description of a live mapping; never pins or extends it. */
    IOReturn describe(uint64_t handle,uint64_t *iova,size_t *bytes,unsigned *permissions) const;
    /* Up to `count` dwords of a mapped kernel-owned BO at a GPU address (through the kernel's own mapping), for the board log. */
    IOReturn peekWords(uint64_t iova,uint32_t *out,size_t count) const;
    /* Only a scheduler's actual successful GPU completion may call this.
     * A timeout, interrupt bit alone or CPU-written timestamp is insufficient. */
    IOReturn retireJobAfterCompletion(uint64_t handle);
    bool poisoned() const {return poisoned_;}
    size_t objectCount() const {return objects_;}
    size_t objectCapacity() const {return mapping_capacity_;}
    size_t reservedObjects() const {return reserved_objects_;}
    uint32_t clientCount() const {return clients_;}
    uint64_t budgetGeneration() const {return budget_generation_;}
    bool mapBusyAlias() const {return map_busy_alias_;}
    bool mapAliasLimit() const {return map_alias_limit_;}
    void addClient();
    void removeClient();
    size_t spanCount() const {return lower_count_+upper_count_;}
    NativeAdrenoVmResult lastResult() const {return result_;}
};
#endif
