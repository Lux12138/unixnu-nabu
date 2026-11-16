/* SPDX-License-Identifier: MIT */
#ifndef NABU_SMMU_POLICY_H
#define NABU_SMMU_POLICY_H
#include <stdint.h>
#include <stddef.h>

/* SM8150 S2CR fields used by this driver. The rest of the read word is not
 * part of this comparison. TYPE always matters; CBNDX, SHCFG and PRIVCFG
 * configure translated transactions, not a FAULT route. EXIDVALID is used
 * only when IDR0.EXIDS is set. SMR validity is checked separately otherwise. */
static inline uint32_t NabuSmmuS2crMask(uint32_t id0,uint32_t requested)
{
    uint32_t mask=UINT32_C(3)<<16;
    if(id0&(UINT32_C(1)<<8))mask|=UINT32_C(1)<<10;
    if(!(requested&(UINT32_C(3)<<16)))
        mask|=(UINT32_C(3)<<24)|(UINT32_C(3)<<8)|UINT32_C(255);
    return mask;
}
static inline int NabuSmmuS2crMatches(uint32_t id0,uint32_t actual,uint32_t requested)
{
    return !((actual^requested)&NabuSmmuS2crMask(id0,requested));
}
static inline int NabuSmmuGeometryMatches(uint32_t id1,size_t bytes,
    unsigned irqCount,unsigned irqCapacity,unsigned gpuBank,unsigned gmuBank)
{
    const unsigned banks=id1&255,firstStage1=(id1>>16)&255;
    const size_t stride=(size_t)1<<((id1>>31)?16:12);
    const size_t pages=(size_t)1<<(((id1>>28)&7)+1);
    return banks && banks<=irqCapacity && irqCount>=banks && irqCount<=irqCapacity &&
        firstStage1<banks && gpuBank>=firstStage1 && gmuBank>=firstStage1 &&
        gpuBank<banks && gmuBank<banks && gpuBank!=gmuBank &&
        (pages+banks)*stride<=bytes;
}
#endif
