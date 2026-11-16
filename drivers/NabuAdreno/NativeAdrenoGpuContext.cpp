/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGpuContext.h"
#include <IOKit/IOLib.h>

static const uint64_t kIovas[GpuBufferCount]={GpuSqeIova,GpuRingIova,GpuMemptrsIova,GpuPwrupIova,GpuScratchIova};
static const size_t kBytes[GpuBufferCount]={GpuSqeBytes,GpuRingBytes,GpuPageBytes,GpuPageBytes,GpuPageBytes};
IOReturn NativeAdrenoGpuContext::bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,
    const unsigned slots[4],const uint16_t sids[4],void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    if(!io || !slots || !sids || bank>7 || !asid || asid>0xffff)return kIOReturnBadArgument;
    const uint16_t expected[4]={0,1,0x400,0x401};
    for(unsigned i=0;i<4;++i) {
        if(slots[i]>127 || sids[i]!=expected[i])return kIOReturnBadArgument;
        for(unsigned j=0;j<i;++j)if(slots[i]==slots[j])return kIOReturnBadArgument;
    }
    used_=true;
    report->bank=bank;report->asid=asid;report->stream_count=4;
    for(unsigned i=0;i<4;++i)report->streams[i]={slots[i],sids[i]};
    auto step=[&](Step value) {report->step=value;if(progress)progress(value);};
    step(Allocate);
    IOReturn status=kIOReturnSuccess;
    static const uint8_t widths[6]={32,36,40,42,44,48};
    if(io->read32(io->cookie,0x28,&report->id2))status=kIOReturnIOError;
    else {
        const unsigned code=(report->id2>>4)&15;
        report->output_bits=code<6?widths[code]:0;
        if(report->output_bits>40)report->output_bits=40;
        if(!report->output_bits)status=kIOReturnUnsupported;
    }
    if(!status) {
        report->allocation_bytes=TablePages*4096;
        status=tables_.allocate(TablePages*4096,report->output_bits,true,TablePages*4096,false,false,true);
        report->allocation_status=status;
    }
    for(unsigned i=0;!status && i<GpuBufferCount;++i) {
        report->allocation_index=i+1;report->allocation_bytes=kBytes[i];
        status=buffers_[i].allocate(kBytes[i],report->output_bits,true,kBytes[i],false,false,true);
        report->allocation_status=status;
    }
    if(!status) {
        step(Prepare);
        status=smmu_.prepare(&tables_,0,0,GpuApertureBytes,NativeAdrenoSmmuContext::MaxLeaves,
            bank,asid,1000,report->output_bits,false);
        if(!status)smmu_.setAliasScratch(alias_scratch_,sizeof(alias_scratch_)/sizeof(*alias_scratch_));
    }
    if(!status) {
        step(Ownership);
        report->ownership_status=ArmSmmu4KCheckFaultedStreams(io,report->streams,4,bank,
            smmu_.configuration()->stream_shareability,&report->ownership);
        if(report->ownership_status!=ArmSmmu4KSuccess)status=kIOReturnIOError;
    }
    if(!status) {step(Program);status=smmu_.program(io);}
    if(!status) {step(Attach);status=smmu_.attach(report->streams,4);}
    if(!status) {
        step(VmAttach);
        NativeAdrenoVmRegion lower{&tables_,smmu_.arenaHandle(),smmu_.stateHandle()};
        // Metal clients share the bounded VM pool; diagnostics need only five bootstrap BOs.
        status=vm_.attach(io,smmu_.configHandle(),smmu_.streamsHandle(),&lower,nullptr,
            NabuGpuVmObjectCapacity(NativeAdrenoVmMaxObjects),262144);
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
        /* IOSurface and no-copy Metal buffers are CPU-cached user pages; the service admitted only
         * chip 0x06040001 before bring-up. The diagnostic profile has no user imports. */
        if(!status) {
            status=vm_.enableCachedCoherentImports(0x06040001);
        }
#endif
    }
    if(!status) {
        step(Map);
        const unsigned permissions[GpuBufferCount]={
            ArmSmmu4KRead|ArmSmmu4KExecute,ArmSmmu4KRead,
            ArmSmmu4KRead|ArmSmmu4KWrite,ArmSmmu4KRead|ArmSmmu4KWrite,
            ArmSmmu4KRead|ArmSmmu4KWrite
        };
        for(unsigned i=0;!status && i<GpuBufferCount;++i)
            status=vm_.map(&buffers_[i],0,kIovas[i],permissions[i],0,2,&handles_[i]);
        report->spans=uint32_t(vm_.spanCount());
    }
    if(!status) {step(Ready);ready_=true;}
    report->status=status;
    NativeAdrenoCaptureSmmuReport(io,&smmu_,report);
    return status;
}
