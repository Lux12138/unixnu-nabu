/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A6XX_HWINIT_H
#define NATIVE_ADRENO_A6XX_HWINIT_H
#include "NativeAdrenoRing.h"

/* A642L register setup follows the SM7325 a6xx_start and A660 clock tables.
 * The caller owns powered GX, a running GMU and the GPU stream mappings.
 * IFPC remains disabled while the CP uses its initial register state. */
class NativeAdrenoA6xxHwInit final {
public:
    static IOReturn program(const NativeAdrenoRingIo *,uint32_t highestBankBit);
    static IOReturn programClockGating(const NativeAdrenoRingIo *);
    static IOReturn finish(const NativeAdrenoRingIo *);
    static IOReturn stop(const NativeAdrenoRingIo *,uint32_t pollLimit);
};

#endif
