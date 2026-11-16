/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSmmuMemory.h"
#include <IOKit/IOLib.h>
static int publishTable(void *cookie,volatile uint64_t *page,size_t bytes)
{
    auto *memory=static_cast<NativeAdrenoMemory *>(cookie);
    if(!memory || !page || bytes!=4096 || ((uintptr_t)page&4095) ||
        (uintptr_t)page<(uintptr_t)memory->mapping())return 1;
    uintptr_t offset=(uintptr_t)page-(uintptr_t)memory->mapping();
    if(offset>memory->bytes() || bytes>memory->bytes()-offset)return 1;
    return memory->publishForDevice(offset,bytes)==kIOReturnSuccess?0:1;
}
IOReturn NativeAdrenoBindSmmuVisibility(NativeAdrenoMemory *memory,ArmSmmu4KLiveVisibility *visibility)
{
    if(!visibility)return kIOReturnBadArgument;*visibility={};
    if(!memory || memory->quarantined() || !memory->deviceReferences() || !memory->mapping() ||
        memory->spanCount()!=1 || memory->spans()[0].offset ||
        memory->spans()[0].bytes!=memory->bytes())return kIOReturnBadArgument;
    *visibility={memory,publishTable};return kIOReturnSuccess;
}
IOReturn NativeAdrenoPrepareSmmuMemory(NativeAdrenoMemory *memory,unsigned input_bits,
    unsigned output_bits,uint64_t iova,uint64_t bytes,unsigned upper,size_t budget,
    ArmSmmu4KLiveArena *arena,ArmSmmu4KLiveState *state,ArmSmmu4KLiveVisibility *visibility)
{
    if(!memory || !arena || !state || !visibility)return kIOReturnBadArgument;
    *visibility={};
    if(state->prepared || state->poisoned || memory->quarantined() || memory->deviceReferences())
        return kIOReturnBusy;
    if(!memory->mapping() || memory->spanCount()!=1 ||
        memory->spans()[0].offset || memory->spans()[0].bytes!=memory->bytes() ||
        !memory->bytes() || (memory->bytes()&4095))return kIOReturnUnsupported;
    *arena={{input_bits,output_bits,memory->spans()[0].physical,budget},iova,bytes,upper,
        static_cast<volatile uint64_t *>(memory->mapping()),memory->bytes()/4096,nullptr,0};
    ArmSmmu4KStatus status=ArmSmmu4KLivePrepare(arena,state);
    if(status!=ArmSmmu4KSuccess)return status==ArmSmmu4KCapacity?kIOReturnNoResources:kIOReturnBadArgument;
    ArmSmmu4KLiveResult audit;
    status=ArmSmmu4KLiveAudit(arena,state,nullptr,0,&audit);
    if(status!=ArmSmmu4KSuccess) {
        IOLog("nabu-smmu: live-audit status=%d\n",(int)status);
        state->poisoned=1;return kIOReturnIOError;
    }
    IOReturn result=memory->publishForDevice(0,state->tables.table_pages*4096);
    if(result==kIOReturnSuccess)result=memory->holdForDevice();
    if(result!=kIOReturnSuccess) {
        IOLog("nabu-smmu: publish status=0x%x\n",(unsigned)result);
        state->poisoned=1;return result;
    }
    return NativeAdrenoBindSmmuVisibility(memory,visibility);
}
