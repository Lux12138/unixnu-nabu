/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NATIVE_ADRENO_A6XX_GMU_H
#define NATIVE_ADRENO_A6XX_GMU_H

#include "NativeAdrenoA6xxPower.h"

struct NativeAdrenoA6xxGmuBuffer {
    void *cpu;
    uint64_t iova;
    size_t bytes;
};

struct NativeAdrenoA6xxGmuConfig {
    NativeAdrenoA6xxIo io;
    const uint8_t *firmware;
    size_t firmwareBytes;
    NativeAdrenoA6xxGmuBuffer buffers[5]; /* icache, dummy, dcache, log, HFI */
    const NativeAdrenoA6xxTables *tables;
    uint32_t chipId; /* qcom,chipid, compressed for HFI_SFR_ADDR by start */
};

/* NativeAdrenoA6xxGmu boots the A640 GMU firmware (a640_gmu.bin, HFI v2) on
 * SM8150 the way the nabu vendor KGSL does (adreno_a6xx_gmu.c a6xx_gmu_fw_start,
 * _load_gmu_rpmh_ucode, load_gmu_fw; kgsl_hfi.c hfi_start). The caller first
 * powers CX, programs the GMU SMMU context, and maps the five uncached buffers.
 * start holds CM3 in reset while loading its ITCM/DTCM (DTCM at 0x40000 on this
 * part) and mapped icache/dcache segments, initializes the HFI queues, loads the
 * A640 RSCC sleep/wake program and the host-owned PDC sequencer and TCS
 * commands, then requires real firmware acknowledgements for FW_VER, the
 * performance table, bandwidth table, ECP feature control and core start. Its
 * four-header HFI table includes the disabled dispatch queue and needs at least
 * 0x5000 mapped bytes (0x40e0 used). The HFI vote selects GPU level 1
 * (257 MHz) and bandwidth level 1 from the two-entry table. GPU OOB powers GX
 * and H2F_MSG_START follows
 * as adreno_start does for A640. poll reads the GMU fault registers and drains
 * bounded response/debug queue contents while the service owns the device.
 * The caller owns GPU/SMMU setup and retains every buffer until stop has
 * halted CM3 and GPU DMA has separately stopped. */
class NativeAdrenoA6xxGmu final {
    NativeAdrenoA6xxGmuConfig config_{};
    NativeAdrenoA6xxReport report_{};
    uint32_t sequence_ = 0;
    bool attempted_ = false;
    bool running_ = false;
    bool performanceCountersRequested_ = false;

    IOReturn read(uint32_t, uint32_t, uint32_t *);
    IOReturn write(uint32_t, uint32_t, uint32_t);
    IOReturn gmuWrite(uint32_t, uint32_t);
    IOReturn poll(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    IOReturn loadFirmware();
    IOReturn initializeQueues();
    IOReturn configureRegisters();
    IOReturn configureSequencers();
    IOReturn drainQueue(unsigned, uint32_t, bool * = nullptr, uint32_t * = nullptr);
    IOReturn clearQueueInterrupts();
    IOReturn exchange(uint32_t, const uint32_t *, size_t, uint32_t * = nullptr);
    IOReturn health();
    void captureFaultQueue();
    void logGpuAccessState(const char *) const;

public:
    enum Phase : uint32_t {
        None, Reset, Firmware, Queues, Registers, FirmwareBoot, Sequencers,
        HfiBoot, HfiVersion, PerformanceTable, BandwidthTable, Features,
        CoreStart, PerformanceVote, GpuWake, Ready, Stop, Runtime
    };

    IOReturn start(const NativeAdrenoA6xxGmuConfig &);
    IOReturn keepAlive(bool);
    /* The service worker owns the request from acknowledgement through release. */
    IOReturn acquirePerformanceCounters();
    IOReturn releasePerformanceCounters();
    IOReturn poll();
    IOReturn stop();
    bool running() const { return running_; }
    const NativeAdrenoA6xxReport &report() const { return report_; }
};

#endif
