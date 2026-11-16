/* SPDX-License-Identifier: GPL-2.0-only */
/* A640 firmware, RSCC/PDC and HFI procedures follow the Xiaomi nabu vendor KGSL
 * (adreno_a6xx_gmu.c, kgsl_gmu.c, kgsl_hfi.c); adapted from the SM7325 A660
 * Adreno6xx implementation.
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved. */
#include "NativeAdrenoA6xxGmu.h"
#include <IOKit/IOLib.h>

namespace {
constexpr uint32_t GmuWindowOffset = 0x6a000;
constexpr uint32_t QueueCount = 4;
constexpr uint32_t QueueTableWords = 6 + QueueCount * 12;
constexpr uint32_t QueueOffset = (QueueTableWords * 4 + 15) & ~15u;
constexpr uint32_t QueueWords = 1024;
constexpr uint32_t HfiRequiredBytes = QueueOffset + QueueCount * QueueWords * 4;
/* A640 TCMs: ITCM at GMU address 0, DTCM at 0x40000, 16 KiB each. */
constexpr uint32_t ItcmBase = 0x0;
constexpr uint32_t DtcmBase = 0x40000;
constexpr uint32_t TcmBytes = 0x4000;
/* _load_gmu_rpmh_ucode uses mx/cx/gfx ARC commands. This unit's live
 * command-db resolves gfx.lvl=0x30050; the generic vendor literal 0x30090
 * resolves xo.lvl on nabu. Never send a GPU voltage vote to the XO address. */
constexpr uint32_t PdcMxAddress = 0x30010;
constexpr uint32_t PdcCxAddress = 0x30000;
constexpr uint32_t PdcGfxAddress = 0x30050;
constexpr uint32_t RscCmdOffset = 2;
constexpr uint32_t PdcCmdOffset = 4;

uint32_t gmuOffset(uint32_t index)
{
    return index * 4 - GmuWindowOffset;
}

uint32_t little32(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
        uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

void publish()
{
    __asm__ volatile("dsb oshst" ::: "memory");
}

void acquire()
{
    __asm__ volatile("dmb oshld" ::: "memory");
}
}

IOReturn NativeAdrenoA6xxGmu::read(uint32_t domain, uint32_t offset, uint32_t *value)
{
    report_.domain = domain;
    report_.offset = offset;
    if (config_.io.read32(config_.io.cookie, domain, offset, value)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    report_.value = *value;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::write(uint32_t domain, uint32_t offset, uint32_t value)
{
    report_.domain = domain;
    report_.offset = offset;
    report_.value = value;
    if (config_.io.write32(config_.io.cookie, domain, offset, value)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    publish();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::gmuWrite(uint32_t index, uint32_t value)
{
    return write(NativeAdrenoA6xxGmuRegisters, gmuOffset(index), value);
}

IOReturn NativeAdrenoA6xxGmu::poll(uint32_t domain, uint32_t offset,
    uint32_t mask, uint32_t expected, uint32_t microseconds)
{
    report_.mask = mask;
    report_.expected = expected;
    for (uint32_t elapsed = 0; elapsed < microseconds; elapsed += 10) {
        uint32_t value;
        IOReturn result = read(domain, offset, &value);
        if (result) {
            return result;
        }
        if ((value & mask) == expected) {
            return kIOReturnSuccess;
        }
        config_.io.delayUs(config_.io.cookie, 10);
    }

    report_.status = kIOReturnTimeout;
    return report_.status;
}

IOReturn NativeAdrenoA6xxGmu::loadFirmware()
{
    // The first two persistent firmware reservations occupy the dummy buffer.
    for (unsigned index = 0; index < 5; ++index) {
        IOLog("NativeAdrenoGmu: zero buffer begin index=%u cpu=%p iova=%llx bytes=%lx\n",
            index,config_.buffers[index].cpu,config_.buffers[index].iova,config_.buffers[index].bytes);
        uint8_t *destination = static_cast<uint8_t *>(config_.buffers[index].cpu);
        for (size_t byte = 0; byte < config_.buffers[index].bytes; ++byte) {
            destination[byte] = 0;
        }
        IOLog("NativeAdrenoGmu: zero buffer end index=%u\n",index);
    }

    size_t cursor = 0;
    unsigned segments = 0;
    while (cursor < config_.firmwareBytes) {
        if (config_.firmwareBytes - cursor < 16) {
            report_.status = kIOReturnBadMedia;
            return report_.status;
        }
        const uint8_t *header = config_.firmware + cursor;
        const uint32_t address = little32(header);
        const uint32_t length = little32(header + 4);
        const uint32_t type = little32(header + 8);
        const uint32_t allocation = little32(header + 12);
        cursor += 16;
        report_.offset = address;
        report_.value = length;

        const uint32_t extent = length ? length : ((type == 1 || type == 7) ? allocation : 0);
        IOLog("NativeAdrenoGmu: firmware block header=%lx address=%08x length=%x type=%u allocation=%x extent=%x\n",
            cursor-16,address,length,type,allocation,extent);
        if (extent > 0xffffffffu - address || length > config_.firmwareBytes - cursor ||
            (address & 3) || (extent & 3)) {
            report_.status = kIOReturnBadMedia;
            return report_.status;
        }

        uint32_t tcmIndex = 0;
        if (address >= ItcmBase && address - ItcmBase < TcmBytes &&
            extent <= TcmBytes - (address - ItcmBase)) {
            tcmIndex = 0x1b400 + (address - ItcmBase) / 4;
        } else if (address >= DtcmBase && address - DtcmBase < TcmBytes &&
            extent <= TcmBytes - (address - DtcmBase)) {
            tcmIndex = 0x1c400 + (address - DtcmBase) / 4;
        }

        bool found = tcmIndex != 0 || extent == 0;
        if (tcmIndex && length) {
            IOLog("NativeAdrenoGmu: TCM write begin register=%x bytes=%x\n",tcmIndex,length);
            for (uint32_t byte = 0; byte < length; byte += 4) {
                IOReturn result = gmuWrite(tcmIndex + byte / 4,
                    little32(config_.firmware + cursor + byte));
                if (result) {
                    return result;
                }
            }
            ++segments;
            IOLog("NativeAdrenoGmu: TCM write end register=%x bytes=%x\n",tcmIndex,length);
        } else if (extent) {
            for (unsigned index = 0; index < 5; ++index) {
                const NativeAdrenoA6xxGmuBuffer &buffer = config_.buffers[index];
                if (address < buffer.iova || address - buffer.iova > buffer.bytes ||
                    extent > buffer.bytes - (address - buffer.iova)) {
                    continue;
                }

                uint8_t *destination = static_cast<uint8_t *>(buffer.cpu) + (address - buffer.iova);
                IOLog("NativeAdrenoGmu: mapped firmware copy begin buffer=%u offset=%llx bytes=%x\n",
                    index,uint64_t(address)-buffer.iova,length);
                for (uint32_t byte = 0; byte < length; ++byte) {
                    destination[byte] = config_.firmware[cursor + byte];
                }
                IOLog("NativeAdrenoGmu: mapped firmware copy end buffer=%u\n",index);
                found = true;
                if (length) {
                    ++segments;
                }
                break;
            }
        }
        if (!found) {
            report_.status = kIOReturnNoMemory;
            return report_.status;
        }

        cursor += length;
    }
    if (!segments) {
        report_.status = kIOReturnBadMedia;
        return report_.status;
    }

    publish();
    /* Register indices are dwords: DTCM + 0xff8 is firmware address 0x43fe0,
     * and + 0xffc is 0x43ff0, the version block in the unit's A640 image. */
    IOReturn result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1c400 + 0xff8), &report_.firmwareCore);
    if (result) {
        return result;
    }
    result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1c400 + 0xffc), &report_.hfiVersion);
    if (result) {
        return result;
    }
    if (!report_.firmwareCore || (report_.hfiVersion >> 28) != 2) {
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::initializeQueues()
{
    /* kgsl_hfi.c hfi_init: four 12-dword headers, dispatch disabled. The stock
     * table counts both remain four; status selects the enabled queues. Data
     * starts at ALIGN(sizeof(hfi_queue_table), 16), not at a fixed page. */
    const NativeAdrenoA6xxGmuBuffer &buffer = config_.buffers[4];
    if (buffer.bytes < HfiRequiredBytes) {
        return kIOReturnBadArgument;
    }
    volatile uint32_t *shared = static_cast<volatile uint32_t *>(buffer.cpu);
    shared[0] = 0;
    shared[1] = QueueTableWords;
    shared[2] = 6;
    shared[3] = 12;
    shared[4] = QueueCount;
    shared[5] = QueueCount;
    const uint32_t priorities[QueueCount] = {10, 10, 40, 20};
    for (unsigned index = 0; index < QueueCount; ++index) {
        volatile uint32_t *header = shared + 6 + index * 12;
        header[0] = index == 3 ? 0 : 1;
        header[1] = uint32_t(buffer.iova) + QueueOffset + index * QueueWords * 4;
        header[2] = index | priorities[index] << 8;
        header[3] = QueueWords;
        header[4] = 0;
        header[5] = 0;
        header[6] = 1;
        header[7] = 1;
        header[8] = 1;
        header[9] = 0;
        header[10] = 0;
        header[11] = 0;
    }

    publish();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::configureRegisters()
{
    /* a6xx_gmu_fw_start: keep core and major, pack minor/patch into 15:8. */
    const uint32_t chipId = (config_.chipId & 0xffff0000) |
        (((config_.chipId >> 8) & 0xff) << 12) | ((config_.chipId & 0xff) << 8);
    struct RegisterValue { uint32_t index; uint32_t value; };
    const RegisterValue values[] = {
        {0x1f9cc, 1},                       // GMU_GENERAL_7: TCM retention
        {0x1f81c, 0},                       // CM3_FW_INIT_RESULT
        {0x1f801, 2},                       // CM3_BOOT_CONFIG = TCM_BOOT
        {0x1f985, uint32_t(config_.buffers[4].iova)}, // HFI_QTBL_ADDR
        {0x1f984, 1},                       // HFI_QTBL_INFO
        {0x23b11, 0x80a008a0},              // AHB_FENCE_RANGE_0 = GMU_FENCE_RANGE_MASK
        {0x1f82d, 0x4052},                  // CM3_CFG
        {0x1f901, 0},                       // PWR_COL_CP_RESP
        {0x1f982, chipId},                  // HFI_SFR_ADDR
        {0x1f900, uint32_t(config_.buffers[3].iova) | uint32_t(config_.buffers[3].bytes / 4096 - 1)},
        {0x1f40f, 1},                       // SYS_BUS_CONFIG
        {0x1f400, 1},                       // ICACHE_CONFIG
        {0x1f401, 1},                       // DCACHE_CONFIG
        {0x1f8c0, 0x09c40400},              // PWR_COL_INTER_FRAME_CTRL, no IFPC
        {0x1f991, 0xffffffff},
        {0x23b04, 0xffffffff},
        {0x1f993, 0xffffffff},
        {0x23b06, 0xffffffff}
    };
    IOLog("NativeAdrenoGmu: A640 HFI_SFR_ADDR chip=%08x\n", chipId);
    for (const RegisterValue &entry : values) {
        IOReturn result = gmuWrite(entry.index, entry.value);
        if (result) {
            return result;
        }
    }

    uint32_t rpmhControl;
    IOReturn result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f8e8), &rpmhControl);
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f8e8, rpmhControl | 0x0f11);
    if (result) {
        return result;
    }

    return write(NativeAdrenoA6xxGpu, 0x3c45 * 4, 0);
}

IOReturn NativeAdrenoA6xxGmu::configureSequencers()
{
    /* adreno_a6xx_gmu.c _load_gmu_rpmh_ucode for A640: the RSCC lives in the
     * GMU window (dword 0x23400) and the host loads the GPU PDC sequencer and
     * its sleep (TCS1) and wake (TCS3) commands; the PDC is not AOP-owned. */
    const NativeAdrenoA6xxTables *tables = config_.tables;
    if (tables->mxAddress != PdcMxAddress || tables->cxAddress != PdcCxAddress ||
        tables->gfxAddress != PdcGfxAddress) {
        IOLog("NativeAdrenoGmu: command-db ARC addresses mx=%05x cx=%05x gfx=%05x differ from the A640 PDC program %05x/%05x/%05x\n",
            tables->mxAddress, tables->cxAddress, tables->gfxAddress,
            PdcMxAddress, PdcCxAddress, PdcGfxAddress);
        report_.value = tables->gfxAddress;
        report_.expected = PdcGfxAddress;
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    struct RegisterValue { uint32_t index; uint32_t value; };
    const RegisterValue rscc[] = {
        {0x004, 0x01000000},                        // RSC_STATUS0_DRV0: disable SDE clock gating
        {0x00b, 1},                                 // PDC_SLAVE_ID_DRV0
        {0x00e, 0},                                 // HIDDEN_TCS_CMD0_DATA
        {0x00d, 0},                                 // HIDDEN_TCS_CMD0_ADDR
        {0x00e + RscCmdOffset, 0},
        {0x00d + RscCmdOffset, 0},
        {0x00e + RscCmdOffset * 2, 0x80000000},
        {0x00d + RscCmdOffset * 2, 0},
        {0x100, 0},                                 // OVERRIDE_START_ADDR
        {0x008, 0x4520},                            // PDC_SEQ_START_ADDR
        {0x009, 0x4510},                            // PDC_MATCH_VALUE_LO
        {0x00a, 0x4514},                            // PDC_MATCH_VALUE_HI
        {0x180, 0xa7a506a0},                        // SEQ_MEM_0_DRV0
        {0x181, 0xa1e6a6e7},
        {0x182, 0xa2e081e1},
        {0x183, 0xe9a982e2},
        {0x184, 0x0020e8a8}
    };
    for (const RegisterValue &entry : rscc) {
        IOReturn result = write(NativeAdrenoA6xxRscc, entry.index * 4, entry.value);
        if (result) {
            return result;
        }
    }

    const uint32_t sequence[] = {0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc};
    for (unsigned index = 0; index < 5; ++index) {
        IOReturn result = write(NativeAdrenoA6xxPdcSeq, index * 4, sequence[index]);
        if (result) {
            return result;
        }
    }

    /* TCS1 sleep: mx 1, cx 0, gfx 0. TCS3 wake: mx 2, cx 3, gfx 3. */
    const RegisterValue pdc[] = {
        {0x1573, 7}, {0x1574, 0}, {0x1572, 0},
        {0x1575, 0x10108}, {0x1576, PdcMxAddress}, {0x1577, 1},
        {0x1575 + PdcCmdOffset, 0x10108}, {0x1576 + PdcCmdOffset, PdcCxAddress}, {0x1577 + PdcCmdOffset, 0},
        {0x1575 + PdcCmdOffset * 2, 0x10108}, {0x1576 + PdcCmdOffset * 2, PdcGfxAddress},
        {0x1577 + PdcCmdOffset * 2, 0},
        {0x15d7, 7}, {0x15d8, 0}, {0x15d6, 0},
        {0x15d9, 0x10108}, {0x15da, PdcMxAddress}, {0x15db, 2},
        {0x15d9 + PdcCmdOffset, 0x10108}, {0x15da + PdcCmdOffset, PdcCxAddress}, {0x15db + PdcCmdOffset, 3},
        {0x15d9 + PdcCmdOffset * 2, 0x10108}, {0x15da + PdcCmdOffset * 2, PdcGfxAddress},
        {0x15db + PdcCmdOffset * 2, 3},
        {0x1148, 0},                                // PDC_GPU_SEQ_START_ADDR
        {0x1140, 0x80000001}                        // PDC_GPU_ENABLE_PDC
    };
    for (const RegisterValue &entry : pdc) {
        IOReturn result = write(NativeAdrenoA6xxPdc, entry.index * 4, entry.value);
        if (result) {
            return result;
        }
    }

    publish();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::health()
{
    /* A640 firmware boots with the legacy handshake (0xbabeface in
     * CM3_FW_INIT_RESULT) and the vendor driver never reads that register
     * again, so the newer-target error field (bits 9..11) is not checked; the
     * CM3 fault interrupt and the AO watchdog/fence/bus bits still are. */
    const bool legacyBoot = report_.firmwareCore <= 0x20010004;
    const uint32_t offsets[] = {0x1f81c, 0x1f992, 0x23b05};
    const uint32_t masks[] = {legacyBoot ? 0u : 0xe00u, 0x8000, 0x29};
    const uint32_t clean[] = {0, 0, 0};
    uint32_t values[3]{};
    for (unsigned index = 0; index < 3; ++index) {
        IOReturn result = read(NativeAdrenoA6xxGmuRegisters,
            gmuOffset(offsets[index]), &values[index]);
        if (result) {
            return result;
        }
    }
    report_.gmuInitResult = values[0];
    report_.gmuInterrupts = values[1];
    report_.gmuAoInterrupts = values[2];

    for (unsigned index = 0; index < 3; ++index) {
        if ((values[index] & masks[index]) != clean[index]) {
            int fenceRead = 0;
            if (values[2] & 8) {
                fenceRead = config_.io.read32(config_.io.cookie, NativeAdrenoA6xxGmuRegisters,
                    gmuOffset(0x23b13), &report_.gmuFenceStatus);
            }
            report_.domain = NativeAdrenoA6xxGmuRegisters;
            report_.offset = gmuOffset(offsets[index]);
            report_.value = values[index];
            report_.mask = masks[index];
            report_.expected = 0;
            report_.status = kIOReturnIOError;
            IOLog("NativeAdrenoGmu: fault phase=%u init=%08x hfi=%08x ao=%08x fence_status=%08x fence_read=%d\n",
                report_.phase, values[0], values[1], values[2], report_.gmuFenceStatus, fenceRead);
            return report_.status;
        }
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::clearQueueInterrupts()
{
    const uint32_t queues = report_.gmuInterrupts & 7;
    if (queues) {
        IOReturn result = gmuWrite(0x1f991, queues);
        if (result) {
            return result;
        }
    }

    const uint32_t completed = report_.gmuAoInterrupts & 0x12;
    if (completed) {
        return gmuWrite(0x23b04, completed);
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::drainQueue(unsigned queue, uint32_t expectedHeader,
    bool *acknowledged, uint32_t *returned)
{
    if (queue < 1 || queue > 2) {
        return kIOReturnBadArgument;
    }
    volatile uint32_t *shared = static_cast<volatile uint32_t *>(config_.buffers[4].cpu);
    volatile uint32_t *header = shared + 6 + queue * 12;
    volatile uint32_t *data = shared + QueueOffset / 4 + queue * QueueWords;
    acquire();
    uint32_t readIndex = header[10];
    const uint32_t writeIndex = header[11];
    report_.hfiQueue = queue;
    if (header[0] != 1 || header[1] != uint32_t(config_.buffers[4].iova) + QueueOffset + queue * 4096 ||
        header[3] != QueueWords || readIndex >= QueueWords || writeIndex >= QueueWords ||
        (readIndex & 3) || (writeIndex & 3)) {
        report_.value = readIndex;
        report_.expected = writeIndex;
        report_.mask = QueueWords - 1;
        report_.status = kIOReturnBadMedia;
        IOLog("NativeAdrenoGmu: queue=%u invalid status=%x address=%08x words=%x read=%x write=%x\n",
            queue, header[0], header[1], header[3], readIndex, writeIndex);
        return report_.status;
    }

    for (unsigned packet = 0; readIndex != writeIndex && packet < QueueWords / 4; ++packet) {
        const uint32_t messageHeader = data[readIndex];
        const uint32_t words = (messageHeader >> 8) & 0xff;
        const uint32_t extent = (words + 3) & ~3u;
        const uint32_t available = (writeIndex - readIndex) & (QueueWords - 1);
        report_.hfiHeader = messageHeader;
        if (words < 2 || words > 19 || extent > available) {
            report_.value = messageHeader;
            report_.expected = available;
            report_.mask = 0xff00;
            report_.status = kIOReturnBadMedia;
            IOLog("NativeAdrenoGmu: queue=%u malformed header=%08x available=%u\n",
                queue, messageHeader, available);
            return report_.status;
        }

        uint32_t message[19]{};
        for (unsigned index = 0; index < words; ++index) {
            message[index] = data[(readIndex + index) & (QueueWords - 1)];
        }
        readIndex = (readIndex + extent) & (QueueWords - 1);
        acquire();
        header[10] = readIndex;
        publish();

        const uint32_t id = messageHeader & 0xff;
        if (id == 100) {
            report_.hfiError = message[1];
            const unsigned bytes = (words - 2) * 4;
            for (unsigned byte = 0; byte < 64; ++byte) {
                report_.hfiErrorText[byte] = byte < bytes ?
                    char(message[2 + byte / 4] >> (8 * (byte & 3))) : 0;
            }
            report_.hfiErrorText[64] = 0;
            report_.value = message[1];
            report_.expected = 0;
            report_.mask = 0xffffffff;
            report_.status = kIOReturnIOError;
            IOLog("NativeAdrenoGmu: HFI error queue=%u header=%08x code=%08x text=%.64s\n",
                queue, messageHeader, report_.hfiError, report_.hfiErrorText);
            return report_.status;
        }

        if (((messageHeader >> 16) & 0xf) == 1) {
            if (words < 3) {
                report_.value = messageHeader;
                report_.status = kIOReturnBadMedia;
                return report_.status;
            }
            if (!expectedHeader || queue != 1 || message[1] != expectedHeader) {
                IOLog("NativeAdrenoGmu: HFI unmatched ACK queue=%u request=%08x expected=%08x\n",
                    queue, message[1], expectedHeader);
                continue;
            }
            /* The third dword is an error code, not only the -1 sentinel.
             * A successful FW_VER also needs its fourth dword to be present. */
            if (message[2]) {
                report_.hfiError = message[2];
                report_.value = message[2];
                report_.expected = 0;
                report_.mask = 0xffffffff;
                report_.status = kIOReturnIOError;
                IOLog("NativeAdrenoGmu: HFI ACK failed queue=%u header=%08x request=%08x error=%08x\n",
                    queue, messageHeader, message[1], message[2]);
                return report_.status;
            }
            if (returned && words < 4) {
                report_.value = messageHeader;
                report_.status = kIOReturnBadMedia;
                return report_.status;
            }
            if (returned) {
                *returned = message[3];
            }
            if (acknowledged && !*acknowledged) {
                *acknowledged = true;
                ++report_.acknowledged;
            }
        } else if (id == 101 && words >= 4) {
            IOLog("NativeAdrenoGmu: HFI debug queue=%u type=%08x timestamp=%08x data=%08x\n",
                queue, message[1], message[2], message[3]);
        } else {
            IOLog("NativeAdrenoGmu: HFI message queue=%u header=%08x words=%u\n",
                queue, messageHeader, words);
        }
    }

    return kIOReturnSuccess;
}

void NativeAdrenoA6xxGmu::captureFaultQueue()
{
    const NativeAdrenoA6xxReport failure = report_;
    (void)drainQueue(2, 0);
    report_.phase = failure.phase;
    report_.domain = failure.domain;
    report_.offset = failure.offset;
    report_.value = failure.value;
    report_.expected = failure.expected;
    report_.mask = failure.mask;
    report_.status = failure.status;
}

void NativeAdrenoA6xxGmu::logGpuAccessState(const char *caller) const
{
    uint32_t fence = 0;
    uint32_t gxStatus = 0;
    uint32_t spClock = 0;
    const NativeAdrenoA6xxIo &io = config_.io;
    const int fenceRead = io.read32(io.cookie, NativeAdrenoA6xxGmuRegisters,
        gmuOffset(0x23b10), &fence);
    const int gxRead = io.read32(io.cookie, NativeAdrenoA6xxGmuRegisters,
        gmuOffset(0x1f8d0), &gxStatus);
    IOLog("NativeAdrenoGmu: %s GPU access fence=%08x read=%d gx_status=%08x read=%d\n",
        caller, fence, fenceRead, gxStatus, gxRead);
    int spRead = -1;
    if (!fenceRead && !gxRead && !fence && !(gxStatus & 0xc0)) {
        IOLog("NativeAdrenoGmu: %s SP clock control read begin\n", caller);
        spRead = io.read32(io.cookie, NativeAdrenoA6xxGmuRegisters,
            gmuOffset(0x1a880), &spClock);
    }

    IOLog("NativeAdrenoGmu: %s SP clock control=%08x read=%d\n",
        caller, spClock, spRead);
}

IOReturn NativeAdrenoA6xxGmu::exchange(uint32_t id, const uint32_t *payload,
    size_t words, uint32_t *returned)
{
    if (id > 0xff || words > 254 || (words && !payload)) {
        return kIOReturnBadArgument;
    }
    report_.hfiMessage = id;
    volatile uint32_t *shared = static_cast<volatile uint32_t *>(config_.buffers[4].cpu);
    volatile uint32_t *command = shared + 6;
    volatile uint32_t *commandData = shared + QueueOffset / 4;
    acquire();
    uint32_t readIndex = command[10];
    uint32_t writeIndex = command[11];
    const uint32_t count = uint32_t(words) + 1;
    const uint32_t padded = (count + 3) & ~3u;
    if (readIndex >= QueueWords || writeIndex >= QueueWords ||
        (readIndex & 3) || (writeIndex & 3) ||
        ((readIndex - writeIndex - 1) & (QueueWords - 1)) < padded) {
        report_.status = kIOReturnNoSpace;
        return report_.status;
    }

    sequence_ = sequence_ % 4095 + 1;
    const uint32_t sent = sequence_ << 20 | count << 8 | id;
    commandData[writeIndex] = sent;
    for (uint32_t index = 0; index < words; ++index) {
        commandData[(writeIndex + index + 1) & (QueueWords - 1)] = payload[index];
    }
    for (uint32_t index = count; index < padded; ++index) {
        commandData[(writeIndex + index) & (QueueWords - 1)] = 0xfafafafa;
    }
    publish();
    command[11] = (writeIndex + padded) & (QueueWords - 1);
    publish();
    IOReturn result = gmuWrite(0x1f994, 1);
    if (result) {
        return result;
    }

    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        result = health();
        if (result) {
            captureFaultQueue();
            return result;
        }
        result = clearQueueInterrupts();
        if (result) {
            return result;
        }
        result = drainQueue(2, 0);
        if (result) {
            return result;
        }
        bool acknowledged = false;
        result = drainQueue(1, sent, &acknowledged, returned);
        if (result || acknowledged) {
            return result;
        }
        config_.io.delayUs(config_.io.cookie, 10);
    }

    report_.status = kIOReturnTimeout;
    return report_.status;
}

IOReturn NativeAdrenoA6xxGmu::start(const NativeAdrenoA6xxGmuConfig &config)
{
    if (attempted_) {
        return kIOReturnBusy;
    }
    if (!config.io.read32 || !config.io.write32 || !config.io.delayUs ||
        !config.firmware || config.firmwareBytes < 16 ||
        config.firmwareBytes > 0x100000 || !config.tables) {
        return kIOReturnBadArgument;
    }
    for (unsigned index = 0; index < 5; ++index) {
        const NativeAdrenoA6xxGmuBuffer &buffer = config.buffers[index];
        if (!buffer.cpu || !buffer.bytes || (buffer.bytes & 4095) ||
            (buffer.iova & 4095) || buffer.iova > 0x7fffffff ||
            buffer.bytes > 0x80000000 - buffer.iova ||
            (reinterpret_cast<uintptr_t>(buffer.cpu) & 4095)) {
            return kIOReturnBadArgument;
        }
    }
    if (config.buffers[4].bytes < HfiRequiredBytes || config.buffers[3].bytes > 0x100000 ||
        config.tables->performance[0] != 2 || config.tables->performance[1] != 2 ||
        config.tables->performance[7] != 257000 || config.tables->performance[53] != 200000 ||
        config.tables->bandwidth[0] != 2 || (config.chipId >> 16) != 0x0604) {
        return kIOReturnBadArgument;
    }

    config_ = config;
    attempted_ = true;
    report_ = {};
    report_.phase = Reset;
    IOReturn result = gmuWrite(0x1f800, 1);
    if (result) {
        return result;
    }
    config_.io.delayUs(config_.io.cookie, 10);

    /* a6xx_gmu_fw_start cold boot: RPMh sequencers before the firmware. */
    report_.phase = Sequencers;
    result = configureSequencers();
    if (result) {
        return result;
    }
    report_.phase = Firmware;
    result = loadFirmware();
    if (result) {
        return result;
    }
    report_.phase = Queues;
    result = initializeQueues();
    if (result) {
        return result;
    }
    report_.phase = Registers;
    result = configureRegisters();
    if (result) {
        return result;
    }

    report_.phase = FirmwareBoot;
    result = gmuWrite(0x1f802, 0);              // a6xx_gmu_start: CX_GMU_WFI_CONFIG
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f800, 0);
    if (result) {
        return result;
    }
    const bool legacyBoot = report_.firmwareCore <= 0x20010004;
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f81c),
        legacyBoot ? 0xffffffff : 0x1ff, legacyBoot ? 0xbabeface : 0x100, 100000);
    IOLog("NativeAdrenoGmu: firmware core=%08x hfi=%08x init=%08x status=%08x\n",
        report_.firmwareCore, report_.hfiVersion, report_.value, result);
    if (result) {
        return result;
    }

    report_.phase = HfiBoot;
    result = gmuWrite(0x1f986, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f980), 1, 1, 100000);
    if (result) {
        return result;
    }

    /* hfi_verify_fw_version precedes the tables on A640. The DTCM word
     * identifies the image; HFI_VERSION_INFO identifies the running protocol. */
    report_.phase = HfiVersion;
    uint32_t runtimeHfiVersion = 0;
    result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f981), &runtimeHfiVersion);
    if (result) {
        return result;
    }
    if (runtimeHfiVersion != report_.hfiVersion || (runtimeHfiVersion >> 28) != 2) {
        report_.value = runtimeHfiVersion;
        report_.expected = report_.hfiVersion;
        report_.mask = 0xffffffff;
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }
    const uint32_t supportedVersion = 2u << 28; // adreno-gpulist.h A640: GMU 2.0
    uint32_t firmwareVersion = 0;
    result = exchange(1, &supportedVersion, 1, &firmwareVersion);
    if (result) {
        return result;
    }
    IOLog("NativeAdrenoGmu: FW_VER acknowledged core=%08x hfi=%08x\n",
        firmwareVersion, runtimeHfiVersion);
    if ((firmwareVersion >> 28) != 2) {
        report_.value = firmwareVersion;
        report_.expected = supportedVersion;
        report_.mask = 0xf0000000;
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    report_.phase = PerformanceTable;
    result = exchange(4, config_.tables->performance, 58);
    if (result) {
        return result;
    }
    report_.phase = BandwidthTable;
    result = exchange(3, config_.tables->bandwidth, 159);
    if (result) {
        return result;
    }

    /* kgsl_hfi.c hfi_start, HFI v2: ECP off; ACD and LM stay disabled. */
    report_.phase = Features;
    const uint32_t ecpPayload[] = {1, 0, 0};
    result = exchange(11, ecpPayload, 3);
    if (result) {
        return result;
    }
    report_.phase = CoreStart;
    const uint32_t corePayload = 0;
    result = exchange(14, &corePayload, 1);
    if (result) {
        return result;
    }

    /* hfi_gx_bw_perf_vote_cmd: ack type 1, GPU level 1 (257 MHz), bw level 1. */
    report_.phase = PerformanceVote;
    const uint32_t votePayload[] = {1, 1, 1};
    result = exchange(30, votePayload, 3);
    if (result) {
        return result;
    }
    /* adreno_start: OOB GPU set (HFI v2 bits 30/31) powers GX, then A640 sends
     * H2F_MSG_START (a6xx_gmu_hfi_start_msg). */
    report_.phase = GpuWake;
    result = gmuWrite(0x1f994, uint32_t(1) << 30);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f992),
        uint32_t(1) << 31, uint32_t(1) << 31, 100000);
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f991, uint32_t(1) << 31);
    if (result) {
        return result;
    }
    result = exchange(10, nullptr, 0);
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f8c3, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f8d0), 0xc0, 0, 100000);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x23b10), 0xffffffff, 0, 100000);
    logGpuAccessState("start");
    if (result) {
        return result;
    }

    running_ = true;
    report_.phase = Ready;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::keepAlive(bool enabled)
{
    if (report_.status) {
        return report_.status;
    }
    if (!running_) {
        return kIOReturnNotReady;
    }

    IOReturn result = gmuWrite(0x1f8c3, enabled ? 1 : 0);
    if (result || !enabled) {
        return result;
    }

    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x23b10), 0xffffffff, 0, 100000);
    logGpuAccessState("keepAlive");
    return result;
}

IOReturn NativeAdrenoA6xxGmu::acquirePerformanceCounters()
{
    if (report_.status) {
        return report_.status;
    }
    if (!running_) {
        return kIOReturnNotReady;
    }
    if (performanceCountersRequested_) {
        return kIOReturnBusy;
    }

    // A640 (HFI v2 firmware) uses a6xx_gmu_oob_set's newer bits for oob_perfcntr = 1.
    constexpr uint32_t request = 1u << 28;
    constexpr uint32_t acknowledge = 1u << 30;
    performanceCountersRequested_ = true;
    IOReturn result = gmuWrite(0x1f994, request);
    if (!result) {
        result = kIOReturnTimeout;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            uint32_t value = 0;
            const IOReturn readStatus = read(NativeAdrenoA6xxGmuRegisters,
                gmuOffset(0x1f992), &value);
            if (readStatus) {
                result = readStatus;
                break;
            }
            if (value & acknowledge) {
                result = kIOReturnSuccess;
                break;
            }

            config_.io.delayUs(config_.io.cookie, 100);
        }
    }

    const IOReturn clearStatus = gmuWrite(0x1f991, acknowledge);
    if (!result) {
        result = clearStatus;
    }
    if (result) {
        releasePerformanceCounters();
        report_.status = result;
    }

    return result;
}

IOReturn NativeAdrenoA6xxGmu::releasePerformanceCounters()
{
    if (!performanceCountersRequested_) {
        return kIOReturnSuccess;
    }

    // The newer-target oob_perfcntr clear request has no firmware ACK poll.
    const IOReturn result = gmuWrite(0x1f994, 1u << 29);
    if (!result) {
        performanceCountersRequested_ = false;
    }

    return result;
}

IOReturn NativeAdrenoA6xxGmu::poll()
{
    if (report_.status) {
        return report_.status;
    }
    if (!running_) {
        return kIOReturnNotReady;
    }

    report_.phase = Runtime;
    IOReturn result = health();
    if (result) {
        captureFaultQueue();
        return result;
    }
    result = clearQueueInterrupts();
    if (!result) {
        result = drainQueue(2, 0);
    }
    if (!result) {
        result = drainQueue(1, 0);
    }
    if (result) {
        return result;
    }

    report_.phase = Ready;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::stop()
{
    if (!attempted_) {
        return kIOReturnNotReady;
    }
    const NativeAdrenoA6xxReport failure = report_;
    const bool failed = report_.status != kIOReturnSuccess;
    const auto complete = [&](IOReturn status) -> IOReturn {
        if (failed) {
            report_ = failure;
        }
        return status;
    };
    report_.phase = Stop;
    IOReturn result = gmuWrite(0x1f993, 0xffffffff);
    if (result) {
        return complete(result);
    }
    result = gmuWrite(0x23b06, 0xffffffff);
    if (result) {
        return complete(result);
    }
    result = gmuWrite(0x1f800, 1);
    if (result) {
        return complete(result);
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f800), 1, 1, 1000);
    if (result) {
        return complete(result);
    }

    running_ = false;
    performanceCountersRequested_ = false;
    report_.status = kIOReturnSuccess;
    return complete(kIOReturnSuccess);
}
