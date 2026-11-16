/* SPDX-License-Identifier: GPL-2.0-only */
/* SM8150 register sequences and ARC/BCM formats follow the Qualcomm SM8150
 * GPUCC, GCC, GDSC, RPMh, QMP and A6xx GMU drivers of the Xiaomi nabu kernel
 * (nabu-r-oss: gpucc-sm8150.c, gcc-sm8150.c, gdsc-regulator.c, msm_qmp.c,
 * kgsl_gmu.c, kgsl_hfi.c) and the upstream a640_build_bw_table. Adapted from
 * the SM7325 Adreno6xx power procedure.
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved. */
#include "NativeAdrenoA6xxPower.h"
#include <IOKit/IOLib.h>

namespace {
constexpr uint32_t Bit(unsigned bit)
{
    return uint32_t(1) << bit;
}

/* gcc-sm8150.c / gpucc-sm8150.c */
constexpr uint32_t GccGpll0Mode = 0x0;
constexpr uint32_t GccGpll0Vote = 0x52000;           // gpll0 .enable_reg, BIT(0)
constexpr uint32_t GccGpuGpll0Vote = 0x52004;        // gpu_gpll0 BIT(15), gpu_gpll0_div BIT(16)
constexpr uint32_t GccGpuCfgAhb = 0x71004;
constexpr uint32_t GccGpuMemnocGfx = 0x7100c;
constexpr uint32_t GccGpuSnocDvmGfx = 0x71018;
constexpr uint32_t GccDdrssGpuAxi = 0x71154;
constexpr uint32_t GpuCcGxGdscr = 0x100c;
constexpr uint32_t GpuCcGxGmu = 0x1064;
constexpr uint32_t GpuCcCxGdscr = 0x106c;
constexpr uint32_t GpuCcGmuRcg = 0x1120;
constexpr uint32_t GpuCcCxGdsHwCtrl = 0x1540;        // gpu_cx_hw_ctrl syscon

struct Resource {
    uint32_t address;
    uint32_t length;
    uint32_t recordOffset;
    uint32_t auxiliaryOffset;
    uint8_t auxiliary[512];
};

uint16_t little16(const uint8_t *p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

uint32_t little32(const uint8_t *p)
{
    return little16(p) | uint32_t(little16(p + 2)) << 16;
}

bool resourceName(uint32_t low, uint32_t high, const char *name)
{
    uint32_t expectedLow = 0;
    uint32_t expectedHigh = 0;
    for (unsigned index = 0; index < 8 && name[index]; ++index) {
        if (index < 4) {
            expectedLow |= uint32_t(uint8_t(name[index])) << (8 * index);
        } else {
            expectedHigh |= uint32_t(uint8_t(name[index])) << (8 * (index - 4));
        }
    }

    return low == expectedLow && high == expectedHigh;
}

/* kgsl_gmu.c setup_volt_dependency_tbl: the first primary level at or above
 * the requested one, then the first secondary level at or above its voltage. */
bool arcVote(const Resource &primary, const Resource &secondary,
    uint32_t level, uint32_t *vote)
{
    if (!primary.length || primary.length & 1 || !secondary.length ||
        secondary.length & 1 || little16(primary.auxiliary) ||
        little16(secondary.auxiliary)) {
        return false;
    }

    unsigned primaryIndex = 0;
    for (; primaryIndex < primary.length / 2; ++primaryIndex) {
        if (little16(primary.auxiliary + 2 * primaryIndex) >= level) {
            break;
        }
    }
    if (primaryIndex >= primary.length / 2 || primaryIndex > 255) {
        return false;
    }

    const uint32_t voltage = little16(primary.auxiliary + 2 * primaryIndex);
    unsigned secondaryCount = 1;
    while (secondaryCount < secondary.length / 2) {
        if (little16(secondary.auxiliary + 2 * (secondaryCount - 1)) &&
            !little16(secondary.auxiliary + 2 * secondaryCount)) {
            break;
        }
        ++secondaryCount;
    }

    unsigned secondaryIndex = 0;
    while (secondaryIndex + 1 < secondaryCount &&
        little16(secondary.auxiliary + 2 * secondaryIndex) < voltage) {
        ++secondaryIndex;
    }
    if (secondaryIndex > 255) {
        return false;
    }

    *vote = voltage << 16 | secondaryIndex << 8 | primaryIndex;
    return true;
}
}

IOReturn NativeAdrenoA6xxPower::read(uint32_t domain, uint32_t offset, uint32_t *out)
{
    report_.domain = domain;
    report_.offset = offset;
    if (io_.read32(io_.cookie, domain, offset, out)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    report_.value = *out;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::write(uint32_t domain, uint32_t offset, uint32_t value)
{
    report_.domain = domain;
    report_.offset = offset;
    report_.value = value;
    if (io_.write32(io_.cookie, domain, offset, value)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::update(uint32_t domain, uint32_t offset,
    uint32_t clear, uint32_t set)
{
    uint32_t value;
    IOReturn result = read(domain, offset, &value);
    if (result) {
        return result;
    }

    return write(domain, offset, (value & ~clear) | set);
}

IOReturn NativeAdrenoA6xxPower::poll(uint32_t domain, uint32_t offset,
    uint32_t mask, uint32_t expected, uint32_t microseconds)
{
    report_.mask = mask;
    report_.expected = expected;
    for (uint32_t elapsed = 0; elapsed < microseconds; ++elapsed) {
        uint32_t value;
        IOReturn result = read(domain, offset, &value);
        if (result) {
            return result;
        }
        if ((value & mask) == expected) {
            return kIOReturnSuccess;
        }
        io_.delayUs(io_.cookie, 1);
    }

    report_.status = kIOReturnTimeout;
    return report_.status;
}

/* msm_qmp.c send_irq: one write of the irq-mask to the APCS IPC register. */
IOReturn NativeAdrenoA6xxPower::ring()
{
    return write(NativeAdrenoA6xxAopDoorbell, doorbellOffset_, doorbellValue_);
}

IOReturn NativeAdrenoA6xxPower::buildTables()
{
    constexpr unsigned ResourceCount = 10;
    static const char *const names[] = {
        "gfx.lvl", "mx.lvl", "cx.lvl", "SH0", "MC0", "ACV", "SH2", "xo.lvl", "SN15", "SN3"
    };
    Resource resources[ResourceCount]{};
    uint32_t headers[8][2]{};
    uint32_t version = 0;
    uint32_t magic = 0;
    unsigned headerCount = 0;
    unsigned seen = 0;
    auto reject = [&](const char *reason, IOReturn status, unsigned resource,
        uint32_t offset, uint32_t actual, uint32_t expected) -> IOReturn {
        report_.domain = NativeAdrenoA6xxCommandDb;
        report_.offset = offset;
        report_.value = actual;
        report_.expected = expected;
        report_.mask = 0xffffffff;
        report_.status = status;
        IOLog("NativeAdrenoA6xx: command-db rejected reason=%s resource=%u version=%x magic=%x seen=%x offset=%x actual=%x expected=%x\n",
            reason, resource, version, magic, seen, offset, actual, expected);
        for (unsigned index = 0; index < headerCount; ++index) {
            IOLog("NativeAdrenoA6xx: command-db header[%u] at=%x raw=%08x,%08x slave=%u records=%x auxiliary=%x count=%u\n",
                index, 8 + index * 16, headers[index][0], headers[index][1],
                headers[index][0] & 0xffff, 144 + (headers[index][0] >> 16),
                144 + (headers[index][1] & 0xffff), headers[index][1] >> 16);
        }
        for (unsigned index = 0; index < ResourceCount; ++index) {
            const Resource &entry = resources[index];
            IOLog("NativeAdrenoA6xx: command-db resource[%u]=%s found=%u record=%x address=%x auxiliary=%x bytes=%u first=%08x,%08x\n",
                index, names[index], (seen >> index) & 1, entry.recordOffset,
                entry.address, entry.auxiliaryOffset, entry.length,
                little32(entry.auxiliary), little32(entry.auxiliary + 4));
        }

        return status;
    };

    IOReturn result = read(NativeAdrenoA6xxCommandDb, 0, &version);
    if (result) {
        return reject("version-read", result, ResourceCount, 0, 0, 0);
    }
    result = read(NativeAdrenoA6xxCommandDb, 4, &magic);
    if (result) {
        return reject("magic-read", result, ResourceCount, 4, 0, 0x0c0330db);
    }
    if (magic != 0x0c0330db) {
        return reject("magic", kIOReturnBadMedia, ResourceCount, 4, magic, 0x0c0330db);
    }

    for (unsigned slave = 0; slave < 8; ++slave) {
        uint32_t *header = headers[slave];
        headerCount = slave + 1;
        for (unsigned word = 0; word < 2; ++word) {
            result = read(NativeAdrenoA6xxCommandDb, 8 + 16 * slave + 4 * word, &header[word]);
            if (result) {
                return reject("slave-header-read", result, ResourceCount,
                    8 + 16 * slave + 4 * word, 0, 0);
            }
        }
        if (!(header[0] & 0xffff)) {
            break;
        }

        const uint32_t records = 144 + (header[0] >> 16);
        const uint32_t auxiliary = 144 + (header[1] & 0xffff);
        const uint32_t count = header[1] >> 16;
        if (count > 4096 || records > 0x20000 || count * 24 > 0x20000 - records) {
            return reject("slave-record-extent", kIOReturnBadMedia, ResourceCount,
                8 + 16 * slave + 4, header[1], 0x20000);
        }

        for (uint32_t index = 0; index < count; ++index) {
            uint32_t record[6];
            for (unsigned word = 0; word < 6; ++word) {
                result = read(NativeAdrenoA6xxCommandDb, records + index * 24 + word * 4, &record[word]);
                if (result) {
                    return reject("resource-record-read", result, ResourceCount,
                        records + index * 24 + word * 4, 0, 0);
                }
            }

            for (unsigned name = 0; name < ResourceCount; ++name) {
                if (!resourceName(record[0], record[1], names[name])) {
                    continue;
                }
                const uint32_t length = record[5] & 0xffff;
                const uint32_t offset = auxiliary + (record[5] >> 16);
                const uint32_t slaveId = header[0] & 0xffff;
                const uint32_t recordOffset = records + index * 24;
                if (seen & Bit(name)) {
                    return reject("duplicate-resource", kIOReturnBadMedia,
                        name, recordOffset, seen, seen & ~Bit(name));
                }

                resources[name].address = record[4];
                resources[name].length = length;
                resources[name].recordOffset = recordOffset;
                resources[name].auxiliaryOffset = offset;
                if (!record[4] || (record[4] & 3)) {
                    return reject("resource-address", kIOReturnBadMedia,
                        name, recordOffset + 16, record[4], 4);
                }
                const uint32_t expectedSlave = name < 3 || name == 7 ? 3u : 5u;
                if (((record[4] >> 16) & 7) != slaveId || slaveId != expectedSlave) {
                    return reject("resource-slave", kIOReturnBadMedia,
                        name, recordOffset + 16, record[4], expectedSlave << 16);
                }
                if (length > sizeof(resources[name].auxiliary) ||
                    offset > 0x20000 || length > 0x20000 - offset) {
                    return reject("resource-auxiliary-extent", kIOReturnBadMedia,
                        name, recordOffset + 20, record[5], 512);
                }

                for (unsigned byte = 0; byte < length; ++byte) {
                    uint32_t value;
                    result = read(NativeAdrenoA6xxCommandDb, (offset + byte) & ~3u, &value);
                    if (result) {
                        return reject("resource-auxiliary-read", result,
                            name, (offset + byte) & ~3u, 0, 0);
                    }
                    resources[name].auxiliary[byte] = uint8_t(value >> (8 * ((offset + byte) & 3)));
                }
                seen |= Bit(name);
            }
        }
    }
    if (seen != (1u << ResourceCount) - 1) {
        return reject("required-resources", kIOReturnNotFound, ResourceCount, 0, seen, (1u << ResourceCount) - 1);
    }

    /* The nabu OPP table gives LOW_SVS+1 for 257 MHz; kgsl_gmu.c subtracts the
     * RPMh regulator offset of one. gpu_cc_gmu_clk_src runs 200 MHz at VDD_MIN. */
    uint32_t gpuVote;
    uint32_t gmuVote;
    if (!arcVote(resources[0], resources[1], 64, &gpuVote)) {
        return reject("gpu-arc-level", kIOReturnUnsupported, 0,
            resources[0].auxiliaryOffset, little32(resources[0].auxiliary), 64);
    }
    if (!arcVote(resources[2], resources[1], 48, &gmuVote)) {
        return reject("gmu-arc-level", kIOReturnUnsupported, 2,
            resources[2].auxiliaryOffset, little32(resources[2].auxiliary), 48);
    }
    for (unsigned index = 3; index < ResourceCount; ++index) {
        if(index==7)continue; // xo.lvl is ARC, all other entries here are BCMs.
        const Resource &resource = resources[index];
        /* ACV is an enable mask, not a bandwidth conversion. Its SM8150
         * command-db width is legitimately zero (unit 1, VCD 11). */
        if (resource.length != 8 || !little32(resource.auxiliary) ||
            (index!=5 && !little16(resource.auxiliary+4)) || resource.auxiliary[6]>=16) {
            return reject("bcm-auxiliary", kIOReturnBadMedia, index,
                resource.recordOffset + 20, resource.length, 8);
        }
    }
    for (unsigned index = 0; index < ResourceCount; ++index) {
        IOLog("NativeAdrenoA6xx: command-db %s address=%05x auxiliary=%u bytes first=%08x\n",
            names[index], resources[index].address, resources[index].length,
            little32(resources[index].auxiliary));
    }

    tables_ = {};
    tables_.cxAddress = resources[2].address;
    tables_.mxAddress = resources[1].address;
    tables_.gfxAddress = resources[0].address;
    tables_.xoAddress = resources[7].address;
    tables_.cxIndex = gmuVote & 0xff;
    tables_.gfxIndex = gpuVote & 0xff;
    tables_.mxIndex = (gpuVote >> 8) & 0xff;
    if (((gmuVote >> 8) & 0xff) > tables_.mxIndex) {
        tables_.mxIndex = (gmuVote >> 8) & 0xff;
    }
    IOLog("NativeAdrenoA6xx: ARC votes gpu=%08x gmu=%08x gfx=%u mx=%u cx=%u\n",
        gpuVote, gmuVote, tables_.gfxIndex, tables_.mxIndex, tables_.cxIndex);

    /* hfi_dcvstable_cmd: two GPU levels (off, 257 MHz) with the unused ACD
     * threshold field at its kgsl_hfi.c value, then two GMU levels. */
    tables_.performance[0] = 2;
    tables_.performance[1] = 2;
    tables_.performance[3] = 0xffffffff;
    tables_.performance[5] = gpuVote;
    tables_.performance[6] = 0xffffffff;
    tables_.performance[7] = 257000;
    tables_.performance[52] = gmuVote;
    tables_.performance[53] = 200000;

    /* The unit's CNOC path is GPU -> SH2 -> SN15 -> SN3. Its 257 MHz
     * pwrlevel requests DDR usecase 3 (1799000 KB/s). Follow KGSL's live BCM
     * width/unit conversion and VCD grouping, instead of assuming CN0 alone
     * or leaving the GPU's DDR vote at the off entry. */
    tables_.bandwidth[0] = 2; // off and the fixed 257 MHz bandwidth
    tables_.bandwidth[1] = 3;
    tables_.bandwidth[2] = 3;
    auto bandwidthGroup=[&](unsigned indices[3],const uint32_t busWidths[3],
        uint32_t kbps,unsigned addressAt,unsigned dataAt,unsigned rowStride,unsigned waitAt) {
        for(unsigned i=1;i<3;++i) {
            const unsigned selected=indices[i];unsigned j=i;
            while(j && resources[indices[j-1]].auxiliary[6]>resources[selected].auxiliary[6]) {
                indices[j]=indices[j-1];--j;
            }
            indices[j]=selected;
        }
        for(unsigned i=0;i<3;++i) {
            const unsigned index=indices[i];
            const Resource &resource=resources[index];
            const bool commit=i==2 || resource.auxiliary[6]!=resources[indices[i+1]].auxiliary[6];
            const uint32_t commitBit=commit?Bit(30):0;
            uint32_t vote=8; // ACV_APPS = BIT(3), KGSL bcm_update_acv_req.
            if(index!=5) {
                const unsigned slot=index==3?0:(index==4?1:(index==6?0:(index==8?1:2)));
                uint64_t peak=uint64_t(kbps)*1000*little16(resource.auxiliary+4)/busWidths[slot];
                peak=peak/little32(resource.auxiliary);
                vote=peak>0x3fff?0x3fff:(peak?uint32_t(peak):1u);
            }
            tables_.bandwidth[addressAt+i]=resource.address;
            tables_.bandwidth[dataAt+i]=commitBit;
            tables_.bandwidth[dataAt+rowStride+i]=commitBit|Bit(29)|vote;
            if(commit)tables_.bandwidth[waitAt]|=Bit(i);
            IOLog("NativeAdrenoA6xx: BCM=%s vcd=%u address=%05x vote=%08x\n",
                names[index],resource.auxiliary[6],resource.address,commitBit|Bit(29)|vote);
        }
    };
    unsigned ddr[3]={3,4,5},cnoc[3]={6,8,9};
    const uint32_t ddrWidths[3]={16,4,4},cnocWidths[3]={8,8,8};
    bandwidthGroup(ddr,ddrWidths,1799000,23,31,8,4);
    bandwidthGroup(cnoc,cnocWidths,100,5,11,6,3);

    return kIOReturnSuccess;
}

/* msm_qmp.c link negotiation and channel open, then a mailbox check. The
 * descriptor layout is magic, version, features, ucore and mcore channel_desc. */
IOReturn NativeAdrenoA6xxPower::openQmp()
{
    if (linkUp_) {
        return kIOReturnSuccess;
    }
    uint32_t magic;
    uint32_t version;
    IOReturn result = read(NativeAdrenoA6xxAop, 0, &magic);
    if (result) {
        return result;
    }
    result = read(NativeAdrenoA6xxAop, 4, &version);
    if (result) {
        return result;
    }
    if (magic != 0x4d41494c || version != 1) {
        IOLog("NativeAdrenoA6xx: AOP QMP descriptor magic=%08x version=%x\n", magic, version);
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    result = read(NativeAdrenoA6xxAop, 52, &mailboxBytes_);
    if (result) {
        return result;
    }
    result = read(NativeAdrenoA6xxAop, 56, &mailboxOffset_);
    if (result) {
        return result;
    }
    if (mailboxOffset_ < 64 || mailboxOffset_ & 3 || mailboxBytes_ < 36 ||
        mailboxOffset_ > 0x400 || mailboxBytes_ > 0x400 - mailboxOffset_) {
        report_.status = kIOReturnBadMedia;
        return report_.status;
    }

    // ucore link/ack/channel/ack at 12..24, mcore link/ack/channel/ack at 36..48.
    uint32_t states[10];
    for (unsigned index = 0; index < 10; ++index) {
        result = read(NativeAdrenoA6xxAop, 12 + 4 * index, &states[index]);
        if (result) {
            return result;
        }
    }
    IOLog("NativeAdrenoA6xx: AOP QMP mailbox=%x/%x ucore=%08x/%08x/%08x/%08x mcore=%08x/%08x/%08x/%08x\n",
        mailboxOffset_, mailboxBytes_, states[0], states[1], states[2], states[3],
        states[6], states[7], states[8], states[9]);

    uint32_t remoteLink;
    result = read(NativeAdrenoA6xxAop, 12, &remoteLink);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxAop, 16, remoteLink);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxAop, 36, 0xffff);
    if (result) {
        return result;
    }
    result = ring();
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxAop, 40, 0xffffffff, 0xffff, 1000000);
    if (result) {
        return result;
    }

    result = write(NativeAdrenoA6xxAop, 44, 0xffff);
    if (result) {
        return result;
    }
    result = ring();
    if (result) {
        return result;
    }
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        uint32_t remoteChannel;
        uint32_t localAck;
        result = read(NativeAdrenoA6xxAop, 20, &remoteChannel);
        if (result) {
            return result;
        }
        result = write(NativeAdrenoA6xxAop, 24, remoteChannel);
        if (result) {
            return result;
        }
        result = read(NativeAdrenoA6xxAop, 48, &localAck);
        if (result) {
            return result;
        }
        if (localAck == 0xffff && remoteChannel == 0xffff) {
            break;
        }
        if (attempt == 99999) {
            report_.status = kIOReturnTimeout;
            return report_.status;
        }
        result = ring();
        if (result) {
            return result;
        }
        io_.delayUs(io_.cookie, 10);
    }

    linkUp_ = true;
    return kIOReturnSuccess;
}

/* msm_qmp.c qmp_send_data: size word at the mailbox start, payload after it,
 * one doorbell; the remote clears the size word when it has consumed it. */
IOReturn NativeAdrenoA6xxPower::sendQmp(const char *message)
{
    size_t length = 0;
    while (message[length]) {
        ++length;
    }
    const uint32_t messageBytes = uint32_t((length + 1 + 3) & ~size_t(3));
    if (!linkUp_ || messageBytes + 4 > mailboxBytes_) {
        report_.status = kIOReturnBadArgument;
        return report_.status;
    }

    IOReturn result = poll(NativeAdrenoA6xxAop, mailboxOffset_, 0xffffffff, 0, 100000);
    if (result) {
        return result;
    }
    for (uint32_t offset = 0; offset < messageBytes; offset += 4) {
        uint32_t value = 0;
        for (unsigned byte = 0; byte < 4 && offset + byte < length; ++byte) {
            value |= uint32_t(uint8_t(message[offset + byte])) << (8 * byte);
        }
        result = write(NativeAdrenoA6xxAop, mailboxOffset_ + 4 + offset, value);
        if (result) {
            return result;
        }
    }
    result = write(NativeAdrenoA6xxAop, mailboxOffset_, messageBytes);
    if (result) {
        return result;
    }
    result = ring();
    if (result) {
        return result;
    }

    result = poll(NativeAdrenoA6xxAop, mailboxOffset_, 0xffffffff, 0, 1000000);
    IOLog("NativeAdrenoA6xx: AOP QMP \"%s\" status=%08x\n", message, result);
    return result;
}

/* Active-only ARC votes for the parent rails and XO through an unused APPS
 * DRV2 active TCS (sm8150 apps_rsc: drv-id 2, tcs-offset 0xd00, two active
 * TCSes first). The GMU applies the bandwidth table's CNOC and DDR votes
 * during HFI startup; these parent rail votes remain held by the host. */
IOReturn NativeAdrenoA6xxPower::voteRails()
{
    constexpr unsigned CommandCount = 4;
    uint32_t configuration;
    IOReturn result = read(NativeAdrenoA6xxRpmh, 0x0c, &configuration);
    if (result) {
        return result;
    }
    if (((configuration >> 12) & 0x3f) < 2 || (configuration >> 27) < CommandCount) {
        IOLog("NativeAdrenoA6xx: APPS DRV2 configuration=%08x\n", configuration);
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    unsigned tcs = 0;
    for (; tcs < 2; ++tcs) {
        uint32_t enabled;
        result = read(NativeAdrenoA6xxRpmh, 0xd00 + tcs * 672 + 0x1c, &enabled);
        if (result) {
            return result;
        }
        if (!enabled) {
            break;
        }
    }
    if (tcs == 2) {
        report_.status = kIOReturnBusy;
        return report_.status;
    }

    const uint32_t base = 0xd00 + tcs * 672;
    const uint32_t addresses[] = {tables_.mxAddress, tables_.cxAddress, tables_.gfxAddress,
        tables_.xoAddress};
    const uint32_t values[] = {tables_.mxIndex, tables_.cxIndex, tables_.gfxIndex, 3};

    uint32_t control;
    result = read(NativeAdrenoA6xxRpmh, base + 0x14, &control);
    if (result) {
        return result;
    }
    // The RSC requires trigger removal to finish before AMC mode is cleared.
    const uint32_t controlValues[] = {
        control & ~Bit(24), control & ~(Bit(24) | Bit(16)), Bit(16)
    };
    for (uint32_t value : controlValues) {
        result = write(NativeAdrenoA6xxRpmh, base + 0x14, value);
        if (result) {
            return result;
        }
        result = poll(NativeAdrenoA6xxRpmh, base + 0x14, 0xffffffff, value, 1000);
        if (result) {
            return result;
        }
    }

    result = write(NativeAdrenoA6xxRpmh, 0xd08, Bit(tcs));
    if (result) {
        return result;
    }
    for (unsigned command = 0; command < CommandCount; ++command) {
        IOLog("NativeAdrenoA6xx: parent vote tcs=%u command=%u address=%08x value=%08x\n",
            tcs, command, addresses[command], values[command]);
        result = write(NativeAdrenoA6xxRpmh, base + 0x30 + command * 20, 0x10108);
        if (result) {
            return result;
        }
        result = write(NativeAdrenoA6xxRpmh, base + 0x34 + command * 20, addresses[command]);
        if (result) {
            return result;
        }
        result = write(NativeAdrenoA6xxRpmh, base + 0x38 + command * 20, values[command]);
        if (result) {
            return result;
        }
    }
    const uint32_t commandMask = (1u << CommandCount) - 1;
    result = write(NativeAdrenoA6xxRpmh, base + 0x10, commandMask);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxRpmh, base + 0x1c, commandMask);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxRpmh, base + 0x14, Bit(16) | Bit(24));
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxRpmh, 0xd04, Bit(tcs), Bit(tcs), 100000);
    if (result) {
        return result;
    }
    for (unsigned command = 0; command < CommandCount; ++command) {
        result = poll(NativeAdrenoA6xxRpmh, base + 0x3c + command * 20,
            Bit(8) | Bit(16), Bit(8) | Bit(16), 1000);
        if (result) {
            return result;
        }
    }
    result = write(NativeAdrenoA6xxRpmh, base + 0x1c, 0);
    if (result) {
        return result;
    }

    return write(NativeAdrenoA6xxRpmh, 0xd08, Bit(tcs));
}

/* gdsc-regulator.c gdsc_enable for gpu_cx_gdsc: the sm8150 node carries an AOP
 * mailbox, so instead of clearing SW_COLLAPSE the host sends the gpu_noc_wa
 * request and polls PWR_ON through gpu_cx_hw_ctrl (qcom,gds-timeout 500 us). */
IOReturn NativeAdrenoA6xxPower::enableCxDomain()
{
    uint32_t gdscr;
    uint32_t status;
    IOReturn result = read(NativeAdrenoA6xxGpuCc, GpuCcCxGdscr, &gdscr);
    if (!result) {
        result = read(NativeAdrenoA6xxGpuCc, GpuCcCxGdsHwCtrl, &status);
    }
    if (result) {
        return result;
    }
    IOLog("NativeAdrenoA6xx: GPU CX GDSC inherited gdscr=%08x hw_ctrl=%08x\n", gdscr, status);
    if (status & Bit(31)) {
        return kIOReturnSuccess;
    }
    if (gdscr & Bit(1)) {
        report_.status = kIOReturnBusy;
        return report_.status;
    }
    if (gdscr & Bit(0)) {
        result = sendQmp("{class: clock, res: gpu_noc_wa}");
    } else {
        result = kIOReturnSuccess;
    }
    if (result) {
        return result;
    }
    io_.delayUs(io_.cookie, 1);
    result = poll(NativeAdrenoA6xxGpuCc, GpuCcCxGdsHwCtrl, Bit(31), Bit(31), 5000);
    uint32_t after = 0;
    (void)io_.read32(io_.cookie, NativeAdrenoA6xxGpuCc, GpuCcCxGdscr, &after);
    IOLog("NativeAdrenoA6xx: GPU CX GDSC result=%08x gdscr=%08x hw_ctrl=%08x\n",
        result, after, report_.value);
    return result;
}

IOReturn NativeAdrenoA6xxPower::branch(uint32_t domain, uint32_t offset,
    uint32_t enable, BranchRule rule, uint32_t hwcgMask)
{
    uint32_t value;
    IOReturn result = update(domain, offset, 0, enable);
    if (result) {
        return result;
    }
    result = read(domain, offset, &value);
    if (result) {
        return result;
    }
    if (rule == BranchDelay) {
        io_.delayUs(io_.cookie, 10);
        return kIOReturnSuccess;
    }
    if (rule == BranchSkip || (hwcgMask && (value & hwcgMask))) {
        return kIOReturnSuccess;
    }

    return poll(domain, offset, Bit(31), 0, 200);
}

IOReturn NativeAdrenoA6xxPower::setSource(uint32_t command, uint32_t configuration)
{
    IOReturn result = update(NativeAdrenoA6xxGpuCc, command + 4, 0x71f, configuration);
    if (result) {
        return result;
    }
    result = update(NativeAdrenoA6xxGpuCc, command, 0, 1);
    if (result) {
        return result;
    }

    return poll(NativeAdrenoA6xxGpuCc, command, 1, 0, 1000);
}

IOReturn NativeAdrenoA6xxPower::enableCx(const NativeAdrenoA6xxIo &io,
    uint32_t doorbellOffset, uint32_t doorbellValue)
{
    if (cxReady_ || !io.read32 || !io.write32 || !io.delayUs || (doorbellOffset & 3) ||
        !doorbellValue) {
        return kIOReturnBadArgument;
    }
    io_ = io;
    doorbellOffset_ = doorbellOffset;
    doorbellValue_ = doorbellValue;
    report_ = {};
    report_.phase = CommandDatabase;
    IOReturn result = buildTables();
    if (result) {
        return result;
    }
    report_.phase = AopAcd;
    result = openQmp();
    if (!result) {
        // kgsl_gmu.c gmu_aop_send_acd_state; ACD stays disabled for this GMU.
        result = sendQmp("{class: gpu, res: acd, value: 0}");
    }
    if (result) {
        return result;
    }
    report_.phase = ParentVotes;
    result = voteRails();
    if (result) {
        return result;
    }

    report_.phase = GccClocks;
    result = update(NativeAdrenoA6xxGcc, GccGpll0Vote, 0, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGcc, GccGpll0Mode, Bit(31), Bit(31), 1000);
    if (result) {
        return result;
    }
    result = branch(NativeAdrenoA6xxGcc, GccGpuGpll0Vote, Bit(15) | Bit(16), BranchDelay);
    if (result) {
        return result;
    }
    result = branch(NativeAdrenoA6xxGcc, GccGpuCfgAhb, 1, BranchDelay);
    if (result) {
        return result;
    }

    report_.phase = CxDomain;
    result = enableCxDomain();
    if (result) {
        return result;
    }

    report_.phase = GmuClock;
    result = setSource(GpuCcGmuRcg, 0x602); // GPLL0_OUT_MAIN_DIV (6) / 1.5 gives 200 MHz.
    if (result) {
        return result;
    }

    report_.phase = GpuCcClocks;
    struct Clock {
        uint32_t domain;
        uint32_t offset;
        BranchRule rule;
        uint32_t hwcgMask;
    };
    const Clock clocks[] = {
        {NativeAdrenoA6xxGpuCc, 0x1004, BranchHalt, 0},       // gpu_cc_cxo_aon_clk
        {NativeAdrenoA6xxGpuCc, 0x109c, BranchHalt, 0},       // gpu_cc_cxo_clk
        {NativeAdrenoA6xxGpuCc, 0x1078, BranchDelay, 0},      // gpu_cc_ahb_clk
        {NativeAdrenoA6xxGpuCc, 0x107c, BranchHalt, 0},       // gpu_cc_crc_ahb_clk
        {NativeAdrenoA6xxGpuCc, 0x1098, BranchHalt, 0},       // gpu_cc_cx_gmu_clk
        {NativeAdrenoA6xxGpuCc, 0x108c, BranchHalt, 0},       // gpu_cc_cx_snoc_dvm_clk
        {NativeAdrenoA6xxGpuCc, 0x1090, BranchHalt, 0},       // gpu_cc_sleep_clk
        {NativeAdrenoA6xxGcc, GccDdrssGpuAxi, BranchVoted, Bit(1)},
        {NativeAdrenoA6xxGcc, GccGpuMemnocGfx, BranchVoted, Bit(1)},
        {NativeAdrenoA6xxGcc, GccGpuSnocDvmGfx, BranchHalt, 0}
    };
    for (const Clock &clock : clocks) {
        result = branch(clock.domain, clock.offset, 1, clock.rule, clock.hwcgMask);
        if (result) {
            return result;
        }
    }

    cxReady_ = true;
    report_.phase = CxReady;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::enableGx()
{
    if (!cxReady_) {
        return kIOReturnNotReady;
    }
    report_.phase = GxDomain;
    IOReturn result = poll(NativeAdrenoA6xxGpuCc, GpuCcGxGdscr, Bit(31), Bit(31), 5000);
    if (result) {
        return result;
    }
    const uint32_t gxDomain = report_.value;
    result = poll(NativeAdrenoA6xxGmuRegisters, 0x1f8d0 * 4 - 0x6a000, 0xc0, 0, 5000);
    if (result) {
        return result;
    }
    const uint32_t gxStatus = report_.value;
    result = poll(NativeAdrenoA6xxGmuRegisters, 0x23b10 * 4 - 0x6a000, 0xffffffff, 0, 5000);
    if (result) {
        return result;
    }
    const uint32_t fence = report_.value;
    uint32_t gxClock;
    result = read(NativeAdrenoA6xxGpuCc, GpuCcGxGmu, &gxClock);
    if (result) {
        return result;
    }
    IOLog("NativeAdrenoA6xx: GMU owns GX domain=%08x status=%08x fence=%08x clock=%08x\n",
        gxDomain, gxStatus, fence, gxClock);

    gxReady_ = true;
    report_.phase = GxReady;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::stop(bool dmaStopped)
{
    if (!dmaStopped) {
        return kIOReturnBusy;
    }
    if (!cxReady_) {
        return kIOReturnNotReady;
    }

    // Host votes and SMMU clocks remain asserted while the service owns memory.
    if (!report_.status) {
        report_.phase = Stopped;
    }
    return kIOReturnSuccess;
}
