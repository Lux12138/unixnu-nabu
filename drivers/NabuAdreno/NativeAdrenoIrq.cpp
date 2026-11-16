/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoIrq.h"
#include <IOKit/IOLib.h>

/* Register offsets and bit positions are the pinned Linux definitions in
 * registers/adreno/a6xx.xml: RBBM_INT_0_STATUS 0x0201, RBBM_INT_CLEAR_CMD
 * 0x0037, RBBM_INT_0_MASK 0x0038, CP_INTERRUPT_STATUS 0x0823 and
 * CP_PROTECT_STATUS 0x0824, plus the A7XX_INT_MASK enable set from
 * a6xx_gpu.c. No bit is inferred beyond those sources. */
enum : uint32_t {
    IrqRbbmStatus = 0x0201,
    IrqRbbmClear = 0x0037,
    IrqRbbmMask = 0x0038,
    IrqCpStatus = 0x0823,
    IrqCpProtect = 0x0824,
    IrqBitIdle = 1u << 0,
    IrqBitAhb = 1u << 1,
    IrqBitAtbAsync = 1u << 6,
    IrqBitGpc = 1u << 7,
    IrqBitCpSw = 1u << 8,
    IrqBitCpHw = 1u << 9,
    IrqBitCpCcuDepth = 1u << 10,
    IrqBitCpCcuColor = 1u << 11,
    IrqBitCpCcuResolve = 1u << 12,
    IrqBitCpIb2 = 1u << 13,
    IrqBitCpIb1 = 1u << 14,
    IrqBitCpPm4 = 1u << 15,
    IrqBitCpPm4Lpac = 1u << 16,
    IrqBitCpRbDone = 1u << 17,
    IrqBitCpWtDone = 1u << 18,
    IrqBitCpFence = 1u << 20,
    IrqBitCpFenceLpac = 1u << 21,
    IrqBitAtbBus = 1u << 22,
    IrqBitHang = 1u << 23,
    IrqBitUcheOob = 1u << 24,
    IrqBitUcheTrap = 1u << 25,
    IrqBitTsbWrite = 1u << 28,
    IrqBitFuse = 1u << 29,
    IrqBitIsdbCpu = 1u << 30,
    IrqBitIsdbDebug = 1u << 31
};
/* Every A7XX status bit that this decoder knows and may clear. */
static constexpr uint32_t kKnown = IrqBitIdle | IrqBitAhb | IrqBitAtbAsync | IrqBitGpc |
    IrqBitCpSw | IrqBitCpHw | IrqBitCpCcuDepth | IrqBitCpCcuColor | IrqBitCpCcuResolve |
    IrqBitCpIb1 | IrqBitCpIb2 | IrqBitCpPm4 | IrqBitCpPm4Lpac | IrqBitCpRbDone |
    IrqBitCpWtDone | IrqBitCpFence | IrqBitCpFenceLpac | IrqBitAtbBus | IrqBitHang |
    IrqBitUcheOob | IrqBitUcheTrap | IrqBitTsbWrite | IrqBitFuse | IrqBitIsdbCpu |
    IrqBitIsdbDebug;
/* A7XX_INT_MASK from the pinned a6xx_gpu.c. */
static constexpr uint32_t kEnable = IrqBitAhb | IrqBitAtbAsync | IrqBitGpc | IrqBitCpSw |
    IrqBitCpHw | IrqBitCpPm4 | IrqBitCpRbDone | IrqBitCpFence | IrqBitAtbBus |
    IrqBitHang | IrqBitUcheOob | IrqBitUcheTrap | IrqBitTsbWrite | IrqBitFuse;
/* CP_INTERRUPT_STATUS error bits, including the A7XX LPAC/BV replicas. */
static constexpr uint32_t kCpErrors = 0x3fff7u;
IOReturn NativeAdrenoIrq::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
uint32_t NativeAdrenoIrq::mask(NativeAdrenoGeneration generation)
{
    if(nativeAdrenoA6xx(generation)) {
        return IrqBitAhb|IrqBitAtbAsync|IrqBitGpc|IrqBitCpSw|IrqBitCpHw|
            IrqBitCpIb2|IrqBitCpIb1|IrqBitCpPm4|IrqBitCpFence|IrqBitAtbBus|
            IrqBitHang|IrqBitUcheOob|IrqBitUcheTrap|IrqBitTsbWrite;
    }
    return kEnable;
}
uint32_t NativeAdrenoIrq::cpErrorMask(NativeAdrenoGeneration generation)
{
    if(nativeAdrenoA6xx(generation)) {
        return 0xf7u;
    }
    return kCpErrors;
}
IOReturn NativeAdrenoIrq::attach(const NativeAdrenoRingIo *io,NativeAdrenoGeneration generation)
{
    if(attached_ || poisoned_)return kIOReturnBusy;
    if(!io || !io->read32 || !io->write32 || !io->barrier)return kIOReturnBadArgument;
    generation_=generation;
    io_=*io;attached_=true;enabled_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoIrq::detach()
{
    if(poisoned_ || !attached_)return kIOReturnNotReady;
    attached_=false;io_={};enabled_=0;return kIOReturnSuccess;
}
IOReturn NativeAdrenoIrq::programMask(uint32_t *programmed)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(programmed)*programmed=0;
    uint32_t readback=0;
    const uint32_t enable=mask(generation_);
    if(io_.write32(io_.cookie,IrqRbbmMask,enable))return poison();
    if(io_.read32(io_.cookie,IrqRbbmMask,&readback))return poison();
    if(readback!=enable)return poison();
    enabled_=readback;
    if(programmed)*programmed=readback;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoIrq::handle(NativeAdrenoIrqResult *result)
{
    if(!result)return kIOReturnBadArgument;
    *result={};
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    uint32_t status=0;
    if(io_.read32(io_.cookie,IrqRbbmStatus,&status))return poison();
    /* Linux acknowledges the whole read status before decoding it. */
    if(io_.write32(io_.cookie,IrqRbbmClear,status))return poison();
    if(io_.barrier(io_.cookie))return poison();
    uint32_t flags=0,cp_status=0,protect=0;
    if(status&IrqBitCpFence)flags|=NativeAdrenoIrqFence;
    if(status&IrqBitCpSw)flags|=NativeAdrenoIrqPreempt;
    if(status&(IrqBitIdle))flags|=NativeAdrenoIrqIdle;
    if(status&(IrqBitHang))flags|=NativeAdrenoIrqHang;
    if(status&(IrqBitUcheOob|IrqBitUcheTrap))flags|=NativeAdrenoIrqUche;
    if(status&(IrqBitAhb|IrqBitGpc|IrqBitAtbAsync|IrqBitAtbBus))flags|=NativeAdrenoIrqBusError;
    if(status&(IrqBitTsbWrite|IrqBitFuse))flags|=NativeAdrenoIrqSecurity;
    if(status&IrqBitCpHw) {
        if(io_.read32(io_.cookie,IrqCpStatus,&cp_status))return poison();
        flags|=NativeAdrenoIrqCpError;
        if(cp_status&cpErrorMask(generation_)) {
            if(io_.read32(io_.cookie,IrqCpProtect,&protect))return poison();
        }
    }
    const uint32_t known=nativeAdrenoA6xx(generation_)?
        mask(generation_)|IrqBitIdle|IrqBitCpCcuDepth|IrqBitCpCcuColor|
            IrqBitCpCcuResolve|IrqBitIsdbCpu|IrqBitIsdbDebug:kKnown;
    const uint32_t unknown=status&~known;
    if(unknown)flags|=NativeAdrenoIrqUnknown;
    if(flags&(NativeAdrenoIrqHang|NativeAdrenoIrqCpError|NativeAdrenoIrqUche|
        NativeAdrenoIrqBusError|NativeAdrenoIrqSecurity|NativeAdrenoIrqUnknown))
        flags|=NativeAdrenoIrqFatal;
    result->flags=flags;result->status=status;result->unknown=unknown;
    result->cp_status=cp_status;result->protect_status=protect;
    return kIOReturnSuccess;
}
