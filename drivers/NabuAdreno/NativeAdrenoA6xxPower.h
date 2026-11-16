/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NATIVE_ADRENO_A6XX_POWER_H
#define NATIVE_ADRENO_A6XX_POWER_H

#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

enum NativeAdrenoA6xxDomain : uint32_t {
    NativeAdrenoA6xxGpu,
    NativeAdrenoA6xxGmuRegisters,
    NativeAdrenoA6xxGpuCc,
    NativeAdrenoA6xxGcc,
    NativeAdrenoA6xxPdc,
    NativeAdrenoA6xxRscc,
    NativeAdrenoA6xxCommandDb,
    NativeAdrenoA6xxRpmh,
    NativeAdrenoA6xxAop,
    NativeAdrenoA6xxAopDoorbell,
    NativeAdrenoA6xxPdcSeq,
    NativeAdrenoA6xxDomains
};

/* NativeAdrenoA6xxIo connects the SM8150 register procedures to mappings owned
 * by NativeAdrenoService. Offsets are bytes within the selected window. The
 * callbacks reject an unaligned access or an access beyond the mapped window.
 * The service retains every mapping and all DMA allocations after a failure. */
struct NativeAdrenoA6xxIo {
    void *cookie;
    int (*read32)(void *, uint32_t domain, uint32_t byteOffset, uint32_t *);
    int (*write32)(void *, uint32_t domain, uint32_t byteOffset, uint32_t);
    void (*delayUs)(void *, uint32_t);
};

struct NativeAdrenoA6xxReport {
    uint32_t phase;
    uint32_t domain;
    uint32_t offset;
    uint32_t value;
    uint32_t expected;
    uint32_t mask;
    IOReturn status;
    uint32_t firmwareCore;
    uint32_t hfiVersion;
    uint32_t hfiMessage;
    uint32_t hfiError;
    uint32_t acknowledged;
    uint32_t hfiQueue;
    uint32_t hfiHeader;
    char hfiErrorText[65];
    uint32_t gmuInitResult;
    uint32_t gmuInterrupts;
    uint32_t gmuAoInterrupts;
    uint32_t gmuFenceStatus;
};

/* NativeAdrenoA6xxTables contains the A6xx HFI v2 wire payloads (the vendor
 * kgsl_hfi.h hfi_dcvstable_cmd and hfi_bwtable_cmd without their headers). GPU
 * entry zero requests the off level; entry one requests 257 MHz at LOW_SVS, the
 * lowest qcom,gpu-pwrlevel of both nabu speed bins. GMU entry one requests 200
 * MHz at MIN_SVS (gpucc-sm8150 gpu_cc_gmu_clk_src). The bandwidth table has off
 * and active DDR levels (1799000 KB/s, 257 MHz usecase 3); CNOC is the unit's
 * SH2/SN15/SN3 path at 100 KB/s. BCM widths, units and VCDs come from command-db.
 * Power reads the ARC indexes and BCM addresses from the running SM8150 command
 * database before changing any power register. */
struct NativeAdrenoA6xxTables {
    uint32_t performance[58];
    uint32_t bandwidth[159];
    uint32_t cxAddress;
    uint32_t mxAddress;
    uint32_t gfxAddress;
    uint32_t xoAddress;
    uint32_t cxIndex;
    uint32_t mxIndex;
    uint32_t gfxIndex;
};

/* NativeAdrenoA6xxPower owns the host votes that keep the GMU and SMMU alive.
 * enableCx reads command-db, opens the AOP QMP link and turns ACD off, raises
 * the parent rails through an unused APPS DRV2 active TCS, enables the SM8150
 * GCC GPU branches, asks AOP to power the GPU CX GDSC (the vendor
 * "{class: clock, res: gpu_noc_wa}" request) and enables the GMU clock and the
 * GPU CC branches. The GMU subsequently controls the GPU clock through HFI.
 * enableGx observes the GX power, GMU fence and GX clock state after the GMU
 * wake request. stop requires the caller to have stopped GPU and GMU DMA
 * first; host votes and mappings remain owned throughout this one-shot run. */
class NativeAdrenoA6xxPower final {
    enum BranchRule : unsigned { BranchHalt, BranchDelay, BranchSkip, BranchVoted };

    NativeAdrenoA6xxIo io_{};
    NativeAdrenoA6xxTables tables_{};
    NativeAdrenoA6xxReport report_{};
    uint32_t doorbellOffset_ = 0;
    uint32_t doorbellValue_ = 0;
    uint32_t mailboxOffset_ = 0;
    uint32_t mailboxBytes_ = 0;
    bool linkUp_ = false;
    bool cxReady_ = false;
    bool gxReady_ = false;

    IOReturn read(uint32_t, uint32_t, uint32_t *);
    IOReturn write(uint32_t, uint32_t, uint32_t);
    IOReturn update(uint32_t, uint32_t, uint32_t, uint32_t);
    IOReturn poll(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    IOReturn ring();
    IOReturn buildTables();
    IOReturn openQmp();
    IOReturn sendQmp(const char *);
    IOReturn voteRails();
    IOReturn enableCxDomain();
    IOReturn branch(uint32_t, uint32_t, uint32_t, BranchRule, uint32_t = 0);
    IOReturn setSource(uint32_t, uint32_t);

public:
    enum Phase : uint32_t {
        None, CommandDatabase, AopAcd, ParentVotes, GccClocks, CxDomain,
        GmuClock, HubClock, GpuCcClocks, CxReady, GxDomain, GxReady, Stopped
    };

    /* doorbell is the AOP IPC register (byte offset, value) in the doorbell window. */
    IOReturn enableCx(const NativeAdrenoA6xxIo &, uint32_t doorbellOffset, uint32_t doorbellValue);
    IOReturn enableGx();
    IOReturn stop(bool dmaStopped);
    const NativeAdrenoA6xxTables &tables() const { return tables_; }
    const NativeAdrenoA6xxReport &report() const { return report_; }
};

#endif
