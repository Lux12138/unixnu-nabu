/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NATIVE_ADRENO_A6XX_SMMU_H
#define NATIVE_ADRENO_A6XX_SMMU_H

#include "NativeAdrenoA6xxService.h"
#include "ArmSmmu4KProgram.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>

/* NativeAdrenoA6xxSmmu owns the QSMMUv500 global setup, fault interrupts and
 * TBU translation requests for the GPU and GMU contexts. The service supplies
 * powered register windows and two reserved, disabled banks while the masters
 * are stopped. Context objects retain ownership of their page tables and SID
 * routes. This object and its mappings remain alive for the entire boot. */
class NativeAdrenoA6xxSmmu final {
    struct Interrupt {
        NativeAdrenoA6xxSmmu *owner;
        unsigned bank; // UINT32_MAX selects the global fault registers.
        uint32_t number;
        bool installed;
        bool enabled;
    };

    struct Fault {
        unsigned bank;
        uint32_t status;
        uint32_t syndrome[3];
        uint64_t address;
    };

    NativeAdrenoA6xxResources resources_{};
    ArmSmmu4KIo io_{};
    OSSharedPtr<IOMemoryMap> tcu_;
    OSSharedPtr<IOMemoryMap> tbu_[2];
    OSSharedPtr<IOMemoryMap> tbuStatus_[2];
    Interrupt interrupts_[3]{};
    Fault firstFault_{};
    uint32_t faultClaimed_=0;
    uint32_t faultReady_=0;
    uint32_t version_=0;
    size_t contextBase_=0;
    size_t contextStride_=0;
    bool prepared_=false;
    bool faultPrinted_=false;

    static void interrupt(void *);
    void capture(unsigned bank);
    size_t contextOffset(unsigned bank) const;
    IOReturn translate(unsigned bank,uint16_t sid,uint64_t address,
        bool privileged,bool instruction,bool write,uint64_t *physical);

public:
    IOReturn prepare(const NativeAdrenoA6xxResources &,const ArmSmmu4KIo &);
    IOReturn checkTranslation(unsigned bank,uint16_t sid,uint64_t address,
        uint64_t physical,bool privileged,bool instruction,bool write);
    IOReturn observe();
    void disableInterrupts();
};

#endif
