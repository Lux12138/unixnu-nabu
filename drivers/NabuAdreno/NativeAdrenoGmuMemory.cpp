/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuMemory.h"
#include <IOKit/IOLib.h>
enum : size_t { PageBytes=0x1000 };
NativeAdrenoGmuSizes NativeAdrenoGmuDefaultSizes(size_t table_bytes)
{
    /* HFI has four queue headers and four 4 KiB queue slots, page rounded. */
    return {0x3c000,0x1000,0x3c000,0x4000,0x5000,table_bytes};
}
IOReturn NativeAdrenoGmuAllocator::allocate(const NativeAdrenoGmuSizes &sizes,unsigned address_bits)
{
    if(ready_ || icache_.bytes())return kIOReturnBusy;
    const uint64_t requested[]={sizes.icache,sizes.dummy,sizes.debug,sizes.log,
        sizes.hfi,sizes.tables};
    for(uint64_t value:requested)
        if(!value || (value&(PageBytes-1)) || value>uint64_t(SIZE_MAX))
            return kIOReturnBadArgument;
    /* kgsl_gmu.c gmu_vma: icache 0x4000 and dcache 0x44000 up to 256 KiB - 16 KiB;
     * uncached entries 1 MiB apart from 0x60000000: dummy, HFI, then log. */
    if(sizes.icache>GmuCacheBytesMax || sizes.debug>GmuCacheBytesMax ||
        sizes.dummy>GmuUncachedStride || sizes.hfi>GmuUncachedStride ||
        sizes.log>GmuUncachedStride)return kIOReturnBadArgument;
    struct Selection {NativeAdrenoMemory *memory;uint64_t bytes;uint64_t *iova;uint64_t at;};
    const Selection selections[]={
        {&icache_,sizes.icache,&layout_.icache_iova,GmuIcacheIova},
        {&dummy_,sizes.dummy,&layout_.dummy_iova,GmuUncachedBase},
        {&debug_,sizes.debug,&layout_.debug_iova,GmuDcacheIova},
        {&log_,sizes.log,&layout_.log_iova,GmuUncachedBase+2*GmuUncachedStride},
        {&hfi_,sizes.hfi,&layout_.hfi_iova,GmuUncachedBase+GmuUncachedStride},
        {&tables_,sizes.tables,&layout_.tables_iova,0}};
    for(const Selection &selection:selections) {
        IOLog("NativeAdrenoGmuMemory: allocate iova=%llx bytes=%llx tables=%u\n",
            selection.at,selection.bytes,selection.memory==&tables_);
        IOReturn status=selection.memory->allocate(static_cast<size_t>(selection.bytes),
            address_bits,true,static_cast<size_t>(selection.bytes),false,false,true);
        if(status) {
            (void)releaseAll();
            return status;
        }
        *selection.iova=selection.at;
    }
    layout_.tables_bytes=static_cast<size_t>(sizes.tables);
    layout_.end_iova=GmuUncachedBase+2*GmuUncachedStride+sizes.log;
    ready_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoGmuAllocator::mapInto(NativeAdrenoVm *vm)
{
    if(!ready_ || !vm)return kIOReturnBadArgument;
    for(uint64_t handle:handles_)if(handle)return kIOReturnBusy;
    /* The table pool itself is never mapped: its physical pages are the
     * translation storage and the VM refuses such an overlap. */
    NativeAdrenoMemory *objects[]={&icache_,&dummy_,&debug_,&log_,&hfi_};
    const uint64_t iovas[]={layout_.icache_iova,layout_.dummy_iova,layout_.debug_iova,
        layout_.log_iova,layout_.hfi_iova};
    for(size_t index=0;index<5;++index) {
        /* kgsl_gmu.c: icache/dcache/dummy/log are privileged; HFI is unprivileged.
         * Only the icache is executable. This is the GMU's privilege model;
         * the A640 CP bootstrap uses unprivileged GPU mappings separately. */
        const unsigned permissions=ArmSmmu4KRead|ArmSmmu4KWrite|
            (index==4?0u:ArmSmmu4KPrivileged)|(index==0?ArmSmmu4KExecute:0u);
        const IOReturn status=vm->map(objects[index],0,iovas[index],permissions,0,2,&handles_[index]);
        if(status) {
            for(size_t previous=0;previous<index;++previous)
                if(handles_[previous]) {vm->unmap(handles_[previous]);handles_[previous]=0;}
            return status;
        }
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoGmuAllocator::unmapFrom(NativeAdrenoVm *vm)
{
    if(!vm)return kIOReturnBadArgument;
    IOReturn status=kIOReturnSuccess;
    for(size_t index=5;index--;)if(handles_[index]) {
        const IOReturn unmapped=vm->unmap(handles_[index]);
        if(unmapped)status=unmapped;else handles_[index]=0;
    }
    return status;
}
IOReturn NativeAdrenoGmuAllocator::releaseAll()
{
    NativeAdrenoMemory *objects[]={&icache_,&dummy_,&debug_,&log_,&hfi_,&tables_};
    IOReturn status=kIOReturnSuccess;
    for(NativeAdrenoMemory *object:objects)
        if(object->bytes() && object->release()!=kIOReturnSuccess)status=kIOReturnIOError;
    return status;
}
IOReturn NativeAdrenoGmuAllocator::release()
{
    if(!ready_)return kIOReturnNotReady;
    const IOReturn status=releaseAll();
    if(status)return status;
    ready_=false;layout_={};
    return kIOReturnSuccess;
}
