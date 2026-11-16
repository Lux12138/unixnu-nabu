/* SPDX-License-Identifier: GPL-2.0-only */
/* A6xx initialization and A640 register tables follow adreno_a6xx.c and
 * adreno-gpulist.h from the Xiaomi nabu (SM8150) kernel; adapted from the
 * SM7325 A660 Adreno6xx implementation.
 * Copyright (c) 2002,2007-2021, The Linux Foundation. All rights reserved. */
#include "NativeAdrenoA6xxHwInit.h"
#include <IOKit/IOLib.h>

namespace {
/* Register indices are dwords from the nabu a6xx_reg.h. */
const uint32_t kTrustedMemoryRegisters[]={
    0xf803,0xf800,0xf801,0xf802
};
/* a6xx_enable_64bit: CP, VSC, GRAS, RB, PC, HLSQ, VFD, VPC, UCHE, SP, TPL1, TSB. */
const uint32_t kAddressModes[]={
    0x842,0xc01,0x8601,0x8e05,0x9e01,0xbe05,0xa601,0x9601,
    0xe00,0xae01,0xb601,0xf810
};
/* a640_gbif, then a6xx_start for A640 (no LIMIT_UCHE_GBIF_RW or TWO_PASS quirks,
 * no preemption). A640 UCHE routes the 1..2 MiB aperture to its 1 MiB GMEM. */
const uint32_t kInitial[][2]={
    {0x3c03,0x00071620},{0x3c04,0x00071620},
    {0x3c05,0x00071620},{0x3c06,0x00071620},{0x11,3},
    {0x50b,0xffffffff},
    {0xe05,0xffffffc0},{0xe06,0x0001ffff},
    {0xe09,0xfffff000},{0xe0a,0x0001ffff},
    {0xe07,0xfffff000},{0xe08,0x0001ffff},
    {0xe0b,0x100000},{0xe0c,0},{0xe0d,0x1fffff},{0xe0e,0},
    {0xe18,0x804},{0xe17,4},
    {0x8c2,0x02000140},{0x8c1,0x8040362c},{0x8c3,128},
    {0x9e00,0x400<<11},{0x98d,1},{0x500,1},
    {0x1f,(1u<<30)|0xcfffff},{0xe19,1}
};
/* a640_hwcg_regs */
const uint32_t kHwcg[][2]={
    {0xb0,0x02222222},{0xb4,0x02222220},{0xb8,0x00000080},{0xbc,0x0000f3cf},
    {0xc0,0x02222222},{0xc4,0x22222222},{0xc8,0x22222222},{0xcc,0x00022222},
    {0xd0,0x11111111},{0xd4,0x11111111},{0xd8,0x11111111},{0xdc,0x00011111},
    {0xe0,0x77777777},{0xe4,0x77777777},{0xe8,0x77777777},{0xec,0x00077777},
    {0xf0,0x22222222},{0xf4,0x01002222},{0xf8,0x00002220},{0x100,0x00040f00},
    {0x104,0x05222022},{0x105,0x00005555},{0x106,0x00000011},{0x107,0x00445044},
    {0x108,0x04222222},{0x111,0x00002222},{0x114,0x00222222},{0x117,0x00000002},
    {0x11b,0x00002222},{0x109,0x00004000},{0x112,0x00002222},{0x115,0x00000200},
    {0x11c,0x00000000},{0x10a,0x00000000},{0x113,0x00000000},{0x116,0x04104004},
    {0x11d,0x00000000},{0x120,0x00000222},{0x121,0x00000111},{0x122,0x00000777},
    {0x10b,0x22222222},{0x110,0x00000004},{0x10f,0x00000002},{0x533,0x00000182},
    {0x44,0x00000000},{0x42,0x00000000},{0x118,0x00000222},{0x119,0x00000111},
    {0x11a,0x00000555}
};
/* a6xx_protected_regs_group: base, count, read protect; then the kgsl-iommu
 * qcom,protect window <0xa0000 0xc000> in dwords, split at 0x2000. */
const uint32_t kProtected[][3]={
    {0x600,0x51,0},{0xae50,0x2,1},{0x9624,0x13,1},{0x8630,0x8,1},
    {0x9e70,0x1,1},{0x9e78,0x187,1},{0xf000,0x810,1},{0xfc00,0x3,0},
    {0x50e,0x0,1},{0x50f,0x0,0},{0x510,0x0,1},{0x0,0x4f9,0},
    {0x501,0xa,0},{0x511,0x44,0},{0xe00,0x1,1},{0xe03,0xb,1},
    {0x8e00,0x0,1},{0x8e50,0xf,1},{0xbe02,0x0,1},{0xbe20,0x11f3,1},
    {0x800,0x82,1},{0x8a0,0x8,1},{0x8ab,0x19,1},{0x900,0x4d,1},
    {0x98d,0x76,1},{0x8d0,0x23,0},{0x980,0x4,0},{0xa630,0x0,1},
    {0x28000,0x1fff,1},{0x2a000,0xfff,1}
};
}

IOReturn NativeAdrenoA6xxHwInit::program(const NativeAdrenoRingIo *io,uint32_t highestBankBit)
{
    if(!io || !io->write32 || !io->read32 || !io->barrier ||
        highestBankBit<13 || highestBankBit>16) {
        return kIOReturnBadArgument;
    }

    uint32_t trustedBefore[4]={0};
    uint32_t trustedAfter[4]={0};
    for(unsigned index=0;index<4;++index) {
        if(io->read32(io->cookie,kTrustedMemoryRegisters[index],&trustedBefore[index])) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: inherited TSB control=%08x base=%08x:%08x size=%08x\n",
        trustedBefore[0],trustedBefore[2],trustedBefore[1],trustedBefore[3]);

    /* This driver allocates ordinary GPU memory. The DRM A6xx setup clears
     * TSB control and the trusted range before enabling SQE in this mode;
     * the A640 secvid registers accept that first write (secvid-set-once). */
    for(uint32_t reg:kTrustedMemoryRegisters) {
        if(io->write32(io->cookie,reg,0)) {
            return kIOReturnIOError;
        }
    }
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    for(unsigned index=0;index<4;++index) {
        if(io->read32(io->cookie,kTrustedMemoryRegisters[index],&trustedAfter[index])) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: configured TSB control=%08x base=%08x:%08x size=%08x\n",
        trustedAfter[0],trustedAfter[2],trustedAfter[1],trustedAfter[3]);
    if(trustedAfter[0] || trustedAfter[1] || trustedAfter[2] || trustedAfter[3]) {
        return kIOReturnIOError;
    }

    IOLog("NativeAdrenoHwInit: address modes begin\n");
    for(uint32_t reg:kAddressModes) {
        if(io->write32(io->cookie,reg,1)) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: core register table begin\n");
    for(const auto &entry:kInitial) {
        if(io->write32(io->cookie,entry[0],entry[1])) {
            return kIOReturnIOError;
        }
    }

    /* qcom,ubwc-mode 3 gives mode 0 and amsbc 1; min-access-length 32 gives mal 0. */
    IOLog("NativeAdrenoHwInit: UBWC mode registers begin\n");
    const uint32_t hbb=(highestBankBit-13)&3;
    const uint32_t modes[][2]={
        {0x8e08,0x10|(hbb<<1)},{0xb604,hbb<<1},
        {0xae02,hbb<<1},{0xe01,hbb<<21}
    };
    for(const auto &entry:modes) {
        if(io->write32(io->cookie,entry[0],entry[1])) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: CP protected register ranges begin\n");
    if(io->write32(io->cookie,0x84f,0x3)) {
        return kIOReturnIOError;
    }
    for(unsigned slot=0;slot<sizeof(kProtected)/sizeof(kProtected[0]);++slot) {
        const uint32_t encoded=kProtected[slot][0]|(kProtected[slot][1]<<18)|(kProtected[slot][2]<<31);
        if(io->write32(io->cookie,0x850+slot,encoded)) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: core register setup completed\n");
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxHwInit::programClockGating(const NativeAdrenoRingIo *io)
{
    if(!io || !io->write32 || !io->barrier) {
        return kIOReturnBadArgument;
    }
    IOLog("NativeAdrenoHwInit: RBBM hardware clock table begin\n");
    for(const auto &entry:kHwcg) {
        if(io->write32(io->cookie,entry[0],entry[1])) {
            return kIOReturnIOError;
        }
    }
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoHwInit: RBBM hardware clock table completed\n");
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxHwInit::finish(const NativeAdrenoRingIo *io)
{
    if(!io || !io->write32 || !io->barrier) {
        return kIOReturnBadArgument;
    }
    IOLog("NativeAdrenoHwInit: GBIF halt release begin\n");
    if(io->write32(io->cookie,0x3c45,0) ||
        io->write32(io->cookie,0x16,0) || io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoHwInit: GBIF halt release completed\n");
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxHwInit::stop(const NativeAdrenoRingIo *io,uint32_t pollLimit)
{
    if(!io || !io->read32 || !io->write32 || !pollLimit) {
        return kIOReturnBadArgument;
    }
    if(io->write32(io->cookie,0x38,0) || io->write32(io->cookie,0x808,3)) {
        return kIOReturnIOError;
    }
    const uint32_t halts[][3]={{0x16,0x17,1},{0x3c45,0x3c46,1},{0x3c45,0x3c46,2}};
    for(const auto &halt:halts) {
        if(io->write32(io->cookie,halt[0],halt[2])) {
            return kIOReturnIOError;
        }
        bool observed=false;
        for(uint32_t attempt=0;attempt<pollLimit;++attempt) {
            uint32_t ack=0;
            if(io->read32(io->cookie,halt[1],&ack)) {
                return kIOReturnIOError;
            }
            if((ack&halt[2])==halt[2]) {
                observed=true;
                break;
            }
            IODelay(1);
        }
        if(!observed) {
            return kIOReturnTimeout;
        }
    }
    return kIOReturnSuccess;
}
