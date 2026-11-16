/* SPDX-License-Identifier: MIT */
#ifndef NABU_GPU_SCANOUT_H
#define NABU_GPU_SCANOUT_H
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoVm.h"
#include "NabuGpuCopyPlan.h"

/* All methods run under the board GPU gate. Mappings are installed before CP
 * starts, remain for the boot, and are retained on uncertain DMA completion. */
class NabuGpuScanout final {
    NativeAdrenoMemory staging_,scanout_;
    NativeAdrenoVm *vm_=nullptr;
    uint64_t stagingHandle_=0,scanoutHandle_=0;
    uint64_t physical_=0,stride_=0;
    uint32_t width_=0,height_=0;
    uint32_t words_[NabuGpuCopyMaxWords]{};
    bool ready_=false,unsafe_=false;
    IOReturn execute(NativeAdrenoRing *,const NativeAdrenoRingClock *,size_t,uint32_t);
public:
    static constexpr uint64_t StagingIova=0x01000000, ScanoutIova=0x04000000;
    IOReturn prepare(NativeAdrenoVm *,unsigned outputBits);
    IOReturn probe(NativeAdrenoRing *,const NativeAdrenoRingClock *,NativeAdrenoMemory *,uint64_t);
    IOReturn present(NativeAdrenoRing *,const NativeAdrenoRingClock *,
        const void *,size_t,uint64_t,uint64_t,size_t,uint64_t,uint32_t,uint32_t);
    bool ready() const {return ready_;}
    bool unsafe() const {return unsafe_;}
    void fault() {ready_=false;unsafe_=true;staging_.quarantine();scanout_.quarantine();}
};
#endif
