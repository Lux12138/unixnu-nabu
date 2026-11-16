/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GMU_SERVICE_H
#define NATIVE_ADRENO_GMU_SERVICE_H
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "NativeAdrenoBoard.h"

class NativeAdrenoGmuService : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGmuService)
    OSSharedPtr<IOMemoryMap> maps_[NativeAdrenoMaxWindows];
    NativeAdrenoWindow windows_[NativeAdrenoMaxWindows]{};
    size_t count_=0;
public:
    IOService *probe(IOService *provider,SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    uint64_t windowBase(size_t index) const
    {return index<count_?windows_[index].base:0;}
    uint64_t windowBytes(size_t index) const
    {return index<count_?windows_[index].bytes:0;}
    size_t windowCount() const {return count_;}
    /* Mapped window address for a caller that already trusts this service. */
    volatile uint8_t *windowAddress(size_t index) const
    {return index<count_ && maps_[index] ?
        reinterpret_cast<volatile uint8_t *>(maps_[index]->getVirtualAddress()):nullptr;}
};

#endif
