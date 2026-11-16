/* SPDX-License-Identifier: MIT */
#include "NabuGpuScanout.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" uint64_t gPhysBase,gPhysSize;
extern "C" {
volatile uint64_t nabu_gpu_copy_frames,nabu_gpu_copy_bytes,nabu_gpu_copy_us;
volatile uint64_t nabu_gpu_copy_max_us,nabu_gpu_copy_fallbacks;
volatile uint32_t nabu_gpu_copy_ready,nabu_gpu_copy_faults;
}

IOReturn NativeAdrenoMemory::importFirmwareScanout(IOMemoryDescriptor *memory,unsigned bits)
{
    if(backed() || storage_.spans || quarantined()) return kIOReturnBusy;
    PE_Video video={};
    if(!memory || !memory->metaCast("IODeviceMemory") || PE_current_console(&video) ||
        video.v_depth!=32 || video.v_rotate ||
        !nabu_gpu_copy_rows(video.v_width,video.v_height,video.v_rowBytes,memory->getLength()) ||
        video.v_rowBytes*video.v_height!=memory->getLength() ||
        !nabu_gpu_firmware_range(video.v_baseAddr,memory->getLength(),gPhysBase,gPhysSize,bits))
        return kIOReturnUnsupported;
    IOByteCount contiguous=0;
    if(memory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone)!=video.v_baseAddr ||
        contiguous<memory->getLength()) return kIOReturnUnsupported;
    NativeAdrenoCpuState cpu={};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);
    if(status) return status;
    storage_.spans=IONewData(NativeAdrenoPhysicalSpan,1);
    if(!storage_.spans) return kIOReturnNoMemory;
    storage_.span_capacity=1;
    imported_=memory; imported_->retain();
    status=imported_->prepare();
    if(status) { (void)release(); return status; }
    storage_.prepared=true; storage_.bytes=memory->getLength(); storage_.output_bits=bits;
    storage_.diagnostic_source=NativeAdrenoMemorySource::Imported;
    storage_.spans[0]={0,video.v_baseAddr,storage_.bytes,0}; storage_.span_count=1;
    cpu_cache_mode_=NativeAdrenoCpuCacheMode::FirmwareUncached;
    return kIOReturnSuccess;
}

IOReturn NabuGpuScanout::prepare(NativeAdrenoVm *vm,unsigned bits)
{
    if(vm_ || !vm) return kIOReturnBadArgument;
    PE_Video video={};
    if(PE_current_console(&video) || video.v_depth!=32 || video.v_rotate ||
        !nabu_gpu_copy_rows(video.v_width,video.v_height,video.v_rowBytes,NabuGpuCopyMaxBytes))
        return kIOReturnUnsupported;
    const uint64_t packed=uint64_t(video.v_width)*video.v_height*4;
    const uint64_t bytes=uint64_t(video.v_rowBytes)*video.v_height;
    if(packed>NabuGpuCopyMaxBytes || bytes>NabuGpuCopyMaxBytes || (packed&4095) || (bytes&4095))
        return kIOReturnUnsupported;
    /* Refuse excessive packet lists before allocating or mutating page tables. */
    if(!nabu_gpu_copy_plan(words_,NabuGpuCopyMaxWords,StagingIova,packed,
        ScanoutIova,bytes,video.v_rowBytes,video.v_width,video.v_height)) return kIOReturnUnsupported;
    IODeviceMemory *memory=IODeviceMemory::withRange(video.v_baseAddr,bytes);
    if(!memory) return kIOReturnNoMemory;
    IOReturn status=scanout_.importFirmwareScanout(memory,bits);
    memory->release();
    if(status) return status;
    status=staging_.allocate(packed,bits,false,packed,false,false,true);
    if(status) { (void)scanout_.release(); return status; }
    vm_=vm; physical_=video.v_baseAddr; stride_=video.v_rowBytes;
    width_=video.v_width; height_=video.v_height;
    status=vm_->map(&staging_,0,StagingIova,ArmSmmu4KRead,0,2,&stagingHandle_);
    if(!status) status=vm_->map(&scanout_,0,ScanoutIova,ArmSmmu4KRead|ArmSmmu4KWrite,0,2,&scanoutHandle_);
    if(status) return status;
    IOLog("NabuGPU: copy-mapped %ux%u stride=%llu staging=%llx scanout=%llx physical=%llx bytes=%llu\n",
        width_,height_,stride_,StagingIova,ScanoutIova,physical_,bytes);
    return kIOReturnSuccess;
}

IOReturn NabuGpuScanout::execute(NativeAdrenoRing *ring,const NativeAdrenoRingClock *clock,
    size_t count,uint32_t timeout)
{
    IOReturn status=vm_->holdJob(stagingHandle_);
    if(status) return status;
    status=vm_->holdJob(scanoutHandle_);
    if(status) { (void)vm_->retireJobAfterCompletion(stagingHandle_); return status; }
    uint32_t fence=0,observed=0;
    status=ring->submit(words_,count,&fence);
    if(!status && ring->wait(fence,clock,timeout,&observed)!=NativeAdrenoRingFenceComplete)
        status=kIOReturnTimeout;
    /* WPTR failure can have issued DMA even with fence==0. Never release its
     * references or permit a CPU fallback on this path without board stop ACKs. */
    if(status) {fault();++nabu_gpu_copy_faults;return status;}
    status=vm_->retireJobAfterCompletion(scanoutHandle_);
    if(!status) status=vm_->retireJobAfterCompletion(stagingHandle_);
    if(status) {fault();++nabu_gpu_copy_faults;}
    return status;
}

IOReturn NabuGpuScanout::probe(NativeAdrenoRing *ring,const NativeAdrenoRingClock *clock,
    NativeAdrenoMemory *scratch,uint64_t scratchIova)
{
    if(!vm_ || !stagingHandle_ || !scanoutHandle_ || !scratch || !ring || !clock ||
        scratch->bytes()<0x144 || !scratch->mapping()) return kIOReturnNotReady;
    auto *source=static_cast<uint32_t *>(staging_.mapping());
    auto *target=static_cast<volatile uint32_t *>(scratch->mapping());
    for(unsigned i=0;i<16;++i) {source[i]=0x640cc000u^(0x01010101u*i);target[64+i]=0;}
    target[63]=0xfacade01; target[80]=0xfacade02;
    IOReturn status=staging_.publishForDevice(0,64);
    if(!status) status=scratch->publishForDevice(0xfc,0x48);
    const size_t count=nabu_gpu_copy_plan(words_,NabuGpuCopyMaxWords,StagingIova,64,
        scratchIova+0x100,64,64,16,1);
    if(!status && !count) status=kIOReturnBadArgument;
    if(!status) status=execute(ring,clock,count,100000);
    if(!status) status=scratch->acquireDeviceWrites(0xfc,0x48);
    if(!status) {
        for(unsigned i=0;i<16;++i) if(target[64+i]!=source[i]) status=kIOReturnIOError;
        if(target[63]!=0xfacade01 || target[80]!=0xfacade02) status=kIOReturnIOError;
    }
    if(status) {fault();return status;}
    ready_=true; nabu_gpu_copy_ready=1;
    IOLog("NabuGPU: proof=cp-memcpy words=16 guards=valid fence=%u/%u\n",
        ring->lastSubmitted(),ring->completedFence());
    return kIOReturnSuccess;
}

IOReturn NabuGpuScanout::present(NativeAdrenoRing *ring,const NativeAdrenoRingClock *clock,
    const void *source,size_t sourceBytes,uint64_t sourceStride,uint64_t physical,
    size_t destinationBytes,uint64_t destinationStride,uint32_t width,uint32_t height)
{
    if(unsafe_) return kIOReturnIOError;
    if(!ready_) return kIOReturnNotReady;
    if(!source || !ring || !clock || physical!=physical_ || width!=width_ || height!=height_ ||
        destinationStride!=stride_ || destinationBytes!=scanout_.bytes() ||
        !nabu_gpu_copy_rows(width,height,sourceStride,sourceBytes)) return kIOReturnBadArgument;
    const size_t count=nabu_gpu_copy_plan(words_,NabuGpuCopyMaxWords,StagingIova,staging_.bytes(),
        ScanoutIova,scanout_.bytes(),stride_,width,height);
    if(!count) return kIOReturnBadArgument;
    const uint64_t begin=clock->now_us(clock->cookie);
    const size_t row=size_t(width)*4;
    auto *destination=static_cast<unsigned char *>(staging_.mapping());
    for(unsigned y=0;y<height;++y)
        memcpy(destination+size_t(y)*row,static_cast<const unsigned char *>(source)+uint64_t(y)*sourceStride,row);
    IOReturn status=staging_.publishForDevice(0,staging_.bytes());
    if(!status) status=execute(ring,clock,count,100000);
    if(!status) {
        const uint64_t us=clock->now_us(clock->cookie)-begin;
        ++nabu_gpu_copy_frames; nabu_gpu_copy_bytes+=staging_.bytes(); nabu_gpu_copy_us+=us;
        if(us>nabu_gpu_copy_max_us) nabu_gpu_copy_max_us=us;
        if(nabu_gpu_copy_frames==1 || !(nabu_gpu_copy_frames&255))
            IOLog("NabuGPU: scanout-copied frames=%llu bytes=%llu us=%llu max-us=%llu fence=%u/%u\n",
                nabu_gpu_copy_frames,nabu_gpu_copy_bytes,nabu_gpu_copy_us,nabu_gpu_copy_max_us,
                ring->lastSubmitted(),ring->completedFence());
    }
    return status;
}
