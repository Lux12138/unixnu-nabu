/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoCpInit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* Pinned definitions: CP_SQE_INSTR_BASE is the 64-bit register at dword offset
 * 0x0830 for A6XX..A7XX (a6xx.xml). CP_THREAD_CONTROL is opcode 0x17 with
 * CONCURRENT_BIN_DISABLE at bit 27, and CP_ME_INIT is opcode 0x48 with seven
 * payload words (adreno_pm4.xml). The payload values are a7xx_cp_init in
 * a6xx_gpu.c: mask BIT(0)|BIT(1)|BIT(3)|BIT(6)|BIT(8), three hardware
 * contexts, 0x20000000 error detection, 0x00000002 operation mode, the
 * power-up register list address and BIT(31) to read it. */
enum : uint32_t {
    CpSqeInstrBase = 0x0830,
    CpThreadControlOpcode = 0x17,
    CpMeInitOpcode = 0x48,
    CpConcurrentBinDisable = 1u << 27,
    CpMeInitWords = 8,
    CpInitWords = 10
};
IOReturn NativeAdrenoCpInit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
uint32_t NativeAdrenoCpInit::meInitMask(NativeAdrenoGeneration generation)
{
    if(nativeAdrenoA6xx(generation)) {
        return 0x16f;
    }
    return (1u<<0)|(1u<<1)|(1u<<3)|(1u<<6)|(1u<<8);
}
IOReturn NativeAdrenoCpInit::run(const NativeAdrenoRingIo *io,NativeAdrenoRing *ring,
    const NativeAdrenoCpInitConfig *config,uint32_t *fence)
{
    if(config && nativeAdrenoA6xx(config->generation)) {
        return kIOReturnUnsupported;
    }
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(done_ || poisoned_)return kIOReturnNotReady;
    if(!io || !io->write32 || !ring || ring->poisoned())return kIOReturnBadArgument;
    if(!config || !config->sqe_iova || (config->sqe_iova&4095) ||
        config->sqe_iova>=UINT64_C(1)<<48 || !config->pwrup_reglist_iova ||
        (config->pwrup_reglist_iova&3) || config->pwrup_reglist_iova>=UINT64_C(1)<<48)
        return kIOReturnBadArgument;
    if(config->sqe_base_programmed) {
        uint32_t lo=0,hi=0;
        if(!io->read32 || io->read32(io->cookie,CpSqeInstrBase,&lo) || io->read32(io->cookie,CpSqeInstrBase+1,&hi) ||
            (((uint64_t)hi<<32)|lo)!=config->sqe_iova)return poison();
    } else if(io->write32(io->cookie,CpSqeInstrBase,(uint32_t)config->sqe_iova) ||
        io->write32(io->cookie,CpSqeInstrBase+1,(uint32_t)(config->sqe_iova>>32)))
        return poison();
    uint32_t words[CpInitWords]={0};
    if(AdrenoPM4Type7(CpThreadControlOpcode,1,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(CpMeInitOpcode,7,&words[2])!=AdrenoPM4Success)return poison();
    words[1]=CpConcurrentBinDisable;
    words[3]=meInitMask();
    words[4]=0x00000003;      /* hardware contexts */
    words[5]=0x20000000;      /* error detection */
    words[6]=0x00000002;      /* operation mode */
    words[7]=(uint32_t)config->pwrup_reglist_iova;
    words[8]=(uint32_t)(config->pwrup_reglist_iova>>32);
    words[9]=1u<<31;          /* read registers from the list */
    uint32_t value=0;
    const IOReturn status=ring->submit(words,CpInitWords,&value);
    if(status)return poison();
    done_=true;*fence=value;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoCpInit::runBootstrap(const NativeAdrenoRingIo *io,NativeAdrenoRing *ring,
    const NativeAdrenoCpInitConfig *config,uint32_t *wptr)
{
    if(!wptr) {
        return kIOReturnBadArgument;
    }
    *wptr=0;
    if(done_ || poisoned_) {
        return kIOReturnNotReady;
    }
    if(!io || !io->write32 || !io->read32 || !ring || ring->poisoned() ||
        !config || !nativeAdrenoA6xx(config->generation) ||
        !config->sqe_iova || (config->sqe_iova&4095) ||
        config->sqe_iova>=(UINT64_C(1)<<48) || !config->pwrup_reglist_iova ||
        (config->pwrup_reglist_iova&3) || config->pwrup_reglist_iova>=(UINT64_C(1)<<48)) {
        return kIOReturnBadArgument;
    }
    uint32_t words[12]={0};
    if(AdrenoPM4Type7(CpMeInitOpcode,11,&words[0])!=AdrenoPM4Success) {
        return poison();
    }
    words[1]=meInitMask(config->generation);
    words[2]=3;
    words[3]=0x20000000;
    words[7]=2;
    words[8]=uint32_t(config->pwrup_reglist_iova);
    words[9]=uint32_t(config->pwrup_reglist_iova>>32);
    IOLog("NativeAdrenoCpInit: A6xx bootstrap packet=%08x/%08x/%08x/%08x/%08x/%08x\n",
        words[0],words[1],words[2],words[3],words[4],words[5]);
    IOLog("NativeAdrenoCpInit: A6xx bootstrap packet=%08x/%08x/%08x/%08x/%08x/%08x\n",
        words[6],words[7],words[8],words[9],words[10],words[11]);

    if(config->sqe_base_programmed) {
        uint32_t lo=0,hi=0;
        if(io->read32(io->cookie,CpSqeInstrBase,&lo) ||
            io->read32(io->cookie,CpSqeInstrBase+1,&hi) ||
            (((uint64_t(hi)<<32)|lo)!=config->sqe_iova)) {
            return poison();
        }
    } else if(io->write32(io->cookie,CpSqeInstrBase,uint32_t(config->sqe_iova)) ||
        io->write32(io->cookie,CpSqeInstrBase+1,uint32_t(config->sqe_iova>>32))) {
        return poison();
    }
    if(io->write32(io->cookie,0x808,1)) {
        return poison();
    }

    /* The first ring submission immediately follows SQE activation, as in
     * a6xx_rb_start; framebuffer output must stay outside this interval. */
    if(ring->submitBootstrap(words,12,wptr)) {
        return poison();
    }
    done_=true;
    return kIOReturnSuccess;
}
