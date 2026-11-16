/* SPDX-License-Identifier: MIT */
#ifndef NABU_SMMU_MAPPER_H
#define NABU_SMMU_MAPPER_H
#include <IOKit/IOService.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoSmmuContext.h"
#include "ArmSmmu4KLive.h"
#include "ArmSmmu4KProgram.h"
#include "ArmSmmu4KStreams.h"
#include "ArmSmmu4KMmio.h"

/* NabuSmmuMapper: a real IOMapper backed by the ARM SMMUv2 hardware.
 *
 * The identity mapper (NabuIdentityMapper) can only answer requests whose
 * memory is one contiguous physical run.  IOSurface memory is usually many
 * runs, so every request was refused (l641c measured map=817 refuse=817).
 * This mapper builds real LPAE stage-1 tables with the qualified ArmSmmu4K
 * kits and programs the SMMU hardware through NativeAdrenoSmmuContext.
 *
 * The mapper owns one SMMU context bank and one stream.  It is not the
 * system mapper (fIsSystem=false) and does not touch UFS or any other
 * device's DMA path.  */
class NabuSmmuMapper final : public IOMapper {
    OSDeclareDefaultStructors(NabuSmmuMapper)
public:
    bool init(OSDictionary *parameters=nullptr) override;
    bool attach(IOService *provider) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    bool initHardware(IOService *provider) override;
    uint64_t getPageSize() const override {return 4096;}
    IOReturn iovmMapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,uint32_t,
        const IODMAMapSpecification *,IODMACommand *,const IODMAMapPageList *,
        uint64_t *mapAddress,uint64_t *mapLength) override;
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *memory,IODMACommand *,uint64_t mapAddress,
        uint64_t mapLength) override;
    IOReturn iovmInsert(uint32_t options,uint64_t mapAddress,uint64_t offset,
        uint64_t physicalAddress,uint64_t length) override;
    uint64_t mapToPhysicalAddress(uint64_t mappedAddress) override;
private:
    IOReturn prepareMapper();
    IOReturn mapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,
        uint64_t *mapAddress,uint64_t *mapLength);
    IOReturn unmapMemory(uint64_t mapAddress,uint64_t mapLength);
    void logStatus(const char *what,IOReturn status);
    // SMMU MMIO
    IOMemoryMap *smmuMap_=nullptr;
    volatile unsigned char *smmuVa_=nullptr;
    size_t smmuBytes_=0;
    ArmSmmu4KMmio mmio_{};
    ArmSmmu4KIo io_{};
    bool ioBound_=false;
    // SMMU context
    NativeAdrenoMemory tables_;
    NativeAdrenoSmmuContext context_;
    unsigned bank_=0;                 /* l645: chosen at runtime by findFreeSmmuBank */
    bool prepared_=false;
    bool programmed_=false;
    bool attached_=false;
    // l651: streams_/streamCount_ removed - attach is skipped because sid800 slot 3
    // already routes SID 0x800 to our bank via the firmware SMR/S2CR.
    // Mapping state
    /* l644: the DPU VIG source-address register is 32 bits (NativeLegacyDisplay.cpp:279
     * flip(uint32_t iova,...), :280 mdp[(vig+0x14)/4]=iova), and kBootIova=0xb8000000
     * is the display SMMU bus address (NativeUfs.cpp:132). Keep the window below 4 GiB. */
    uint64_t nextIova_=0x40000000ULL;   // 1 GiB aperture, below 4 GiB
    uint64_t iovaLimit_=0x40000000ULL + 0x20000000ULL;   /* l645: 512 MiB window (0x40000000..0x60000000) */
};
#endif
