/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A6XX_ZAP_H
#define NATIVE_ADRENO_A6XX_ZAP_H
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoMemory.h"

/* The ZAP loader authenticates the unit's own a640_zap shader (metadata and
 * loadable segment from the board ADT) through SCM PAS (pil_gpu qcom,pas-id 13).
 * Its metadata stays in owned DMA memory, and its code stays in the board's
 * reserved physical window (pil_gpu_mem) throughout the boot. The service
 * supplies a Normal non-cacheable CPU mapping; the loader copies the ELF
 * segment and zeros its remaining memory size before authenticating it. */
class NativeAdrenoA6xxZap final {
    NativeAdrenoMemory metadata_;
    uint32_t phase_=0;
    uint32_t convention_=0;
    int64_t transport_=0,result_=0;
public:
    IOReturn load(const NativeAdrenoA6xxResources &,void *reservedCpu);
    uint32_t phase() const {return phase_;}
    uint32_t convention() const {return convention_;}
    int64_t transport() const {return transport_;}
    int64_t result() const {return result_;}
};

#endif
