/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSmmuContext.h"
#include <IOKit/IOLib.h>
IOReturn NativeAdrenoSmmuContext::prepare(NativeAdrenoMemory *tables,unsigned upper,uint64_t iova,
    size_t bytes,size_t leaf_budget,unsigned bank,unsigned asid,unsigned poll_budget,unsigned output_bits,
    bool continueAfterFault)
{
    if(prepared_ || poisoned_)return kIOReturnBusy;
    if(!tables || upper>1 || !bytes || (iova&4095) || !leaf_budget || leaf_budget>MaxLeaves ||
        bank>63 || asid>0xffff || !poll_budget || poll_budget>4096)
        return kIOReturnBadArgument;
    const IOReturn prepared=NativeAdrenoPrepareSmmuMemory(tables,32,output_bits,iova,bytes,upper,leaf_budget,
        &arena_,&state_,&visibility_);
    if(prepared!=kIOReturnSuccess)return prepared==kIOReturnNoResources?prepared:kIOReturnBadArgument;
    /* nabu uses Normal NC table storage and non-cacheable SMMU walks;
     * publication barriers order stores without assuming SM8150 CTTW. */
    if(!tables->writeCombined())return kIOReturnUnsupported;
    config_=(ArmSmmu4KContextConfig){
        .lower=upper?nullptr:&arena_.config,
        .upper=upper?&arena_.config:nullptr,
        .lower_tables=upper?nullptr:&state_.tables,
        .upper_tables=upper?&state_.tables:nullptr,
        .id0=0,
        .id2=0,
        .asid=asid,
        .coherent_walk=0,
        .stall_on_fault=0,
        .continue_after_fault=unsigned(continueAfterFault),
        .stream_shareability=ArmSmmu4KShareabilityNonShareable,
        .bypass_shareability=ArmSmmu4KShareabilityNonShareable
    };
    tables_=tables;bank_=bank;poll_budget_=poll_budget;prepared_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::program(const ArmSmmu4KIo *io)
{
    if(!prepared_ || programmed_ || poisoned_ || !io)return kIOReturnBadArgument;
    const ArmSmmu4KStatus status=ArmSmmu4KProgramDisabledContext(io,bank_,&config_,
        poll_budget_,&program_);
    program_status_=status;
    if(status!=ArmSmmu4KSuccess) {poisoned_=true;return kIOReturnIOError;}
    /* SM8150's qsmmuv500 context setup applies ACTLR 0x303 to every GPU SID,
     * then invalidates the bank's cached translations before attaching it. */
    const size_t actlrOffset=size_t(program_.context_offset)+4;
    if(io->write32(io->cookie,actlrOffset,0x303) || io->barrier(io->cookie) ||
        io->read32(io->cookie,actlrOffset,&actlr_)) {
        actlr_status_=ArmSmmu4KIoError;
    } else if(actlr_!=0x303) {
        actlr_status_=ArmSmmu4KStateMismatch;
    } else {
        ArmSmmu4KProgramResult sync{};
        actlr_status_=ArmSmmu4KSyncContextTlb(io,program_.context_offset,config_.asid,1,poll_budget_,&sync);
    }
    if(actlr_status_!=ArmSmmu4KSuccess) {
        poisoned_=true;
        return kIOReturnIOError;
    }
    io_=*io;programmed_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::attach(const ArmSmmu4KStream *routes,size_t count)
{
    if(!programmed_ || attached_ || poisoned_ || !routes || !count || count>8)
        return kIOReturnBadArgument;
    const ArmSmmu4KStatus status=ArmSmmu4KAttachStreams(&io_,&streams_,routes,count,
        bank_,&config_,&routes_);
    attach_status_=status;
    if(status!=ArmSmmu4KSuccess) {poisoned_=true;return kIOReturnIOError;}
    attached_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSmmuContext::observe() const
{
    if(!programmed_ || poisoned_)return kIOReturnBadArgument;
    ArmSmmu4KProgramResult result{};
    return ArmSmmu4KObserveOwnedContext(&io_,bank_,&config_,&result)==ArmSmmu4KSuccess?
        kIOReturnSuccess:kIOReturnIOError;
}
IOReturn NativeAdrenoSmmuContext::disable()
{
    if(!attached_ || poisoned_)return kIOReturnBadArgument;
    ArmSmmu4KProgramResult result{};
    if(ArmSmmu4KDisableOwnedContext(&io_,bank_,&config_,poll_budget_,&result)!=ArmSmmu4KSuccess) {
        poisoned_=true;
        return kIOReturnIOError;
    }
    attached_=false;programmed_=false;
    return kIOReturnSuccess;
}
