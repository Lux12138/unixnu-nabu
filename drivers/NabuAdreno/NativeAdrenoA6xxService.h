/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A6XX_SERVICE_H
#define NATIVE_ADRENO_A6XX_SERVICE_H
#include <stdint.h>
#include <stddef.h>
#include <IOKit/IOTypes.h>

/* Each window contains a physical range supplied by the board's ADT node. */
struct NativeAdrenoA6xxWindow {
    uint64_t physical;
    uint64_t bytes;
};

/* nabu (SM8150, Adreno 640v2): the board reads /nabu-adreno, which carries the
 * windows of the unit's own Android device tree and its vendor firmware. Compared
 * with the SM7325 A642L board, the A640 has its RSCC inside the GMU window, a
 * host-loaded PDC sequencer (pdcSeq), an APCS IPC doorbell instead of IPCC for
 * the AOP mailbox and no LPAC. Its four GPU SIDs share one context bank. */
struct NativeAdrenoA6xxResources {
    NativeAdrenoA6xxWindow gpu,gmu,smmu,gpucc,gcc,pdc,rscc;
    NativeAdrenoA6xxWindow commandDb,rpmh,aop,aopDoorbell,pdcSeq;
    NativeAdrenoA6xxWindow zap;
    NativeAdrenoA6xxWindow smmuTcu;
    NativeAdrenoA6xxWindow smmuTbu[2];
    NativeAdrenoA6xxWindow smmuTbuStatus[2];
    uint32_t smmuTbuSidStart[2];
    uint32_t smmuTbuSidCount[2];
    uint32_t smmuGlobalInterrupt; // Raw GIC INTID, including the SPI base of 32.
    uint32_t gpuInterrupt; // CP CACHE_FLUSH_TS and GPU fault interrupt.
    uint32_t smmuContextInterrupts[8]; // Hardware context-bank order.
    uint32_t smmuContextInterruptCount;
    uint32_t chipId,gmemBytes,frequencyHz,highestBankBit,minAccessLength,ubwcMode;
    uint32_t gpuBank,gpuAsid,gpuSlot,gpuSid,gpuSidMask;
    uint32_t gmuBank,gmuAsid,gmuSlot,gmuSid,gmuSidMask;
    uint32_t aopDoorbellOffset,aopDoorbellValue,zapPasId;
    const uint8_t *gmuFirmware,*sqeFirmware,*zapMetadata,*zapShader;
    size_t gmuFirmwareBytes,sqeFirmwareBytes,zapMetadataBytes,zapShaderBytes;
};

extern "C" int native_adreno_a6xx_resources(NativeAdrenoA6xxResources *);
/* Registration keeps the line disabled. nabu has no GIC SPI client API, so the
 * board polls each enabled callback on the hardware worker at a bounded interval;
 * callbacks must tolerate being called with no pending source. The callback and
 * context remain owned by the caller for the boot lifetime. */
extern "C" IOReturn native_adreno_smmu_register_irq(uint32_t interrupt,
    void (*callback)(void *), void *context);
extern "C" IOReturn native_adreno_smmu_enable_irq(uint32_t interrupt);
extern "C" IOReturn native_adreno_smmu_disable_irq(uint32_t interrupt);
extern "C" void native_adreno_poll_irqs(void);
/* Wait at most 5 s for the current capture end to be committed. A nonzero
 * result blocks the next hardware step; this candidate requires logging. */
extern "C" int native_adreno_checkpoint(const char *phase);
extern "C" void native_adreno_a6xx_start(void);
extern "C" IOReturn native_adreno_a6xx_request_start(void);

#endif
