/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * QSMMUv500 reset and ECATS procedures adapted from the SM7325 arm-smmu and
 * arm-smmu-qcom drivers. Board clocks are held on by NativeAdrenoA6xxPower. */
#include "NativeAdrenoA6xxSmmu.h"
#include "NabuSmmuPolicy.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>

namespace {
constexpr uint32_t FaultMask=0xc00001fe;
constexpr uint32_t Stalled=1u<<30;
constexpr uint32_t ContextFaultInterrupt=1u<<6;
constexpr uint32_t ContextStall=1u<<7;
constexpr uint32_t HaltRequest=1u<<16;
constexpr uint32_t BroadcastTlbMaintenance=1u<<13;
constexpr uint32_t PrivateTlbHints=(1u<<12)|(1u<<11);
constexpr uint32_t ConfigurationFaultControls=(1u<<5)|(1u<<4);
constexpr uint64_t EcatsPhysicalAddressMask=UINT64_C(0x0000fffffffff000);
constexpr unsigned EcatsPhysicalAddressShift=12;
constexpr unsigned PollMicroseconds=1000;

OSSharedPtr<IOMemoryMap> mapRegisters(const NativeAdrenoA6xxWindow &window)
{
    if(!window.physical || !window.bytes || window.physical>UINT64_MAX-window.bytes) {
        return {};
    }
    IODeviceMemory *memory=IODeviceMemory::withRange(window.physical,window.bytes);
    if(!memory) {
        return {};
    }
    OSSharedPtr<IOMemoryMap> mapping(memory->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
    memory->release();
    return mapping;
}

uint32_t read32(IOMemoryMap *map,size_t offset)
{
    const auto value=*reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()+offset);
    __asm__ volatile("dsb ld" ::: "memory");
    return value;
}

uint64_t read64(IOMemoryMap *map,size_t offset)
{
    const auto value=*reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress()+offset);
    __asm__ volatile("dsb ld" ::: "memory");
    return value;
}

void write32(IOMemoryMap *map,size_t offset,uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()+offset)=value;
}

void write64(IOMemoryMap *map,size_t offset,uint64_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress()+offset)=value;
}

bool waitBits(IOMemoryMap *map,size_t offset,uint32_t mask,uint32_t expected)
{
    for(unsigned elapsed=0;elapsed<PollMicroseconds;++elapsed) {
        if((read32(map,offset)&mask)==expected) {
            return true;
        }
        IODelay(1);
    }
    return false;
}
}

size_t NativeAdrenoA6xxSmmu::contextOffset(unsigned bank) const
{
    return contextBase_+bank*contextStride_;
}

IOReturn NativeAdrenoA6xxSmmu::prepare(const NativeAdrenoA6xxResources &resources,
    const ArmSmmu4KIo &io)
{
    if(prepared_ || !io.read32 || !io.read64 || !io.write32 || !io.barrier) {
        return kIOReturnBadArgument;
    }
    resources_=resources;
    io_=io;
    uint32_t id0=0,id1=0,id2=0;
    if(io_.read32(io_.cookie,0x20,&id0) || io_.read32(io_.cookie,0x24,&id1) ||
        io_.read32(io_.cookie,0x28,&id2)) {
        return kIOReturnIOError;
    }
    const unsigned banks=id1&255;
    contextStride_=size_t(1)<<((id1>>31)?16:12);
    contextBase_=contextStride_*(size_t(1)<<(((id1>>28)&7)+1));
    /* SMMUv2 uses the bank index as its context IRQ index. The nabu DT
     * supplies eight IRQs; this unit reports five banks. Surplus IRQs are
     * unused, as in arm-smmu_device_cfg_probe(), rather than additional banks. */
    const size_t irqCapacity=sizeof(resources_.smmuContextInterrupts)/
        sizeof(resources_.smmuContextInterrupts[0]);
    if(!NabuSmmuGeometryMatches(id1,io_.register_bytes,
        resources_.smmuContextInterruptCount,irqCapacity,resources_.gpuBank,resources_.gmuBank)) {
        IOLog("NativeAdrenoSmmu: unsupported geometry id=%08x/%08x/%08x banks=%u context_irqs=%u\n",
            id0,id1,id2,banks,resources_.smmuContextInterruptCount);
        return kIOReturnUnsupported;
    }

    /* Global maintenance precedes all new client traffic. Existing live banks
     * require their owner's quiescence and are not taken over by this driver. */
    for(unsigned bank=0;bank<banks;++bank) {
        uint32_t control=0;
        if(io_.read32(io_.cookie,contextOffset(bank),&control)) {
            return kIOReturnIOError;
        }
        if(control&1) {
            IOLog("NativeAdrenoSmmu: global setup blocked by live bank=%u sctlr=%08x\n",bank,control);
            return kIOReturnBusy;
        }
    }

    if(resources_.smmuTcu.bytes<0x20) {
        return kIOReturnBadArgument;
    }
    tcu_=mapRegisters(resources_.smmuTcu);
    for(unsigned index=0;index<2;++index) {
        if(resources_.smmuTbu[index].bytes<0x38 || resources_.smmuTbuStatus[index].bytes<4 ||
            !resources_.smmuTbuSidCount[index]) {
            return kIOReturnBadArgument;
        }
        tbu_[index]=mapRegisters(resources_.smmuTbu[index]);
        tbuStatus_[index]=mapRegisters(resources_.smmuTbuStatus[index]);
        if(!tbu_[index] || !tbuStatus_[index]) {
            return kIOReturnNoMemory;
        }
    }
    if(!tcu_) {
        return kIOReturnNoMemory;
    }
    version_=read32(tcu_.get(),0x18);
    if(!version_ || version_==UINT32_MAX) {
        IOLog("NativeAdrenoSmmu: invalid TCU version=%08x\n",version_);
        return kIOReturnNotReady;
    }

    interrupts_[0]={this,UINT32_MAX,resources_.smmuGlobalInterrupt,false,false};
    interrupts_[1]={this,resources_.gpuBank,resources_.smmuContextInterrupts[resources_.gpuBank],false,false};
    interrupts_[2]={this,resources_.gmuBank,resources_.smmuContextInterrupts[resources_.gmuBank],false,false};
    for(auto &entry:interrupts_) {
        const IOReturn status=native_adreno_smmu_register_irq(entry.number,interrupt,&entry);
        if(status) {
            return status;
        }
        entry.installed=true;
    }

    uint32_t acr=0,acrAfter=0,globalFault=0,before=0,after=0;
    if(io_.read32(io_.cookie,0x10,&acr) ||
        io_.write32(io_.cookie,0x10,acr&~(1u<<26)) ||
        io_.barrier(io_.cookie) || io_.read32(io_.cookie,0x10,&acrAfter) ||
        (acrAfter&(1u<<26)) || io_.read32(io_.cookie,0x48,&globalFault) ||
        io_.write32(io_.cookie,0x48,globalFault)) {
        return kIOReturnIOError;
    }
    const unsigned ownedBanks[]={resources_.gpuBank,resources_.gmuBank};
    for(unsigned bank:ownedBanks) {
        if(io_.write32(io_.cookie,contextOffset(bank)+0x58,FaultMask)) {
            return kIOReturnIOError;
        }
    }
    if(io_.write32(io_.cookie,0x6c,UINT32_MAX) ||
        io_.write32(io_.cookie,0x68,UINT32_MAX) ||
        io_.read32(io_.cookie,0,&before)) {
        return kIOReturnIOError;
    }
    uint32_t control=before;
    control|=(1u<<1)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<10)|(1u<<11)|(1u<<12);
    control&=~((1u<<0)|(1u<<13)|(3u<<14)|(3u<<22));
    control|=3u<<22;
    if(id2&(1u<<15)) {
        control|=1u<<31;
    }
    if(id0&(1u<<8)) {
        control|=1u<<3;
    }
    if(io_.write32(io_.cookie,0x70,UINT32_MAX) || io_.barrier(io_.cookie)) {
        return kIOReturnIOError;
    }
    bool synced=false;
    for(unsigned elapsed=0;elapsed<PollMicroseconds;++elapsed) {
        uint32_t status=0;
        if(io_.read32(io_.cookie,0x74,&status)) {
            return kIOReturnIOError;
        }
        if(!(status&1)) {
            synced=true;
            break;
        }
        IODelay(1);
    }
    if(!synced) {
        IOLog("NativeAdrenoSmmu: global TLB synchronization timed out\n");
        return kIOReturnTimeout;
    }
    if(io_.barrier(io_.cookie) || io_.write32(io_.cookie,0,control) ||
        io_.barrier(io_.cookie) || io_.read32(io_.cookie,0,&after)) {
        return kIOReturnIOError;
    }

    /* SMMUv2 section 9.6 permits configuration-fault controls to be RAZ/WI.
     * PTM and VMIDPNE may also be RAZ/WI when IDR0.BTM is set. */
    uint32_t optionalZero=ConfigurationFaultControls;
    if(id0&BroadcastTlbMaintenance) {
        optionalZero|=PrivateTlbHints;
    }
    const uint32_t acceptedZero=control&~after&optionalZero;
    if((control^after)&~acceptedZero) {
        IOLog("NativeAdrenoSmmu: global control rejected requested=%08x actual=%08x\n",control,after);
        return kIOReturnIOError;
    }
    if(acceptedZero) {
        IOLog("NativeAdrenoSmmu: optional global controls read zero mask=%08x id0=%08x\n",acceptedZero,id0);
    }

    prepared_=true;
    for(auto &entry:interrupts_) {
        const IOReturn status=native_adreno_smmu_enable_irq(entry.number);
        if(status) {
            disableInterrupts();
            return status;
        }
        entry.enabled=true;
    }
    IOLog("NativeAdrenoSmmu: provider ready tcu=%08x acr=%08x/%08x scr0=%08x/%08x polled_sources=%u/%u/%u\n",
        version_,acr,acrAfter,before,after,interrupts_[0].number,interrupts_[1].number,
        interrupts_[2].number);
    return observe();
}

void NativeAdrenoA6xxSmmu::interrupt(void *cookie)
{
    auto *entry=static_cast<Interrupt *>(cookie);
    entry->owner->capture(entry->bank);
}

void NativeAdrenoA6xxSmmu::capture(unsigned bank)
{
    Fault fault{};
    fault.bank=bank;
    if(bank==UINT32_MAX) {
        if(io_.read32(io_.cookie,0x48,&fault.status) || !fault.status) {
            return;
        }
        for(unsigned index=0;index<3;++index) {
            io_.read32(io_.cookie,0x50+4*index,&fault.syndrome[index]);
        }
        io_.write32(io_.cookie,0x48,fault.status);
        uint32_t control=0;
        io_.read32(io_.cookie,0,&control);
        io_.write32(io_.cookie,0,control&~((1u<<2)|(1u<<5)));
    } else {
        const size_t offset=contextOffset(bank);
        if(io_.read32(io_.cookie,offset+0x58,&fault.status) || !(fault.status&FaultMask)) {
            return;
        }
        io_.read64(io_.cookie,offset+0x60,&fault.address);
        io_.read32(io_.cookie,offset+0x68,&fault.syndrome[0]);
        io_.read32(io_.cookie,offset+0x6c,&fault.syndrome[1]);
        io_.read32(io_.cookie,contextStride_+0x400+4*bank,&fault.syndrome[2]);
        uint32_t control=0;
        io_.read32(io_.cookie,offset,&control);
        io_.write32(io_.cookie,offset,control&~ContextFaultInterrupt);
        io_.write32(io_.cookie,offset+0x58,fault.status);
        io_.barrier(io_.cookie);
        if(fault.status&Stalled) {
            io_.write32(io_.cookie,offset+8,1);
        }
    }
    io_.barrier(io_.cookie);
    uint32_t expected=0;
    if(__atomic_compare_exchange_n(&faultClaimed_,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        firstFault_=fault;
        __atomic_store_n(&faultReady_,1,__ATOMIC_RELEASE);
    }
}

IOReturn NativeAdrenoA6xxSmmu::observe()
{
    if(!prepared_) {
        return kIOReturnNotReady;
    }
    /* Callbacks are polled on this worker, including during bootstrap waits. */
    for(const auto &entry:interrupts_)if(entry.installed && entry.enabled)capture(entry.bank);
    if(!__atomic_load_n(&faultReady_,__ATOMIC_ACQUIRE)) {
        return kIOReturnSuccess;
    }
    if(!faultPrinted_) {
        faultPrinted_=true;
        IOLog("NativeAdrenoSmmu: FIRST FAULT bank=%u fsr=%08x far=%llx syndrome=%08x/%08x/%08x\n",
            firstFault_.bank,firstFault_.status,firstFault_.address,
            firstFault_.syndrome[0],firstFault_.syndrome[1],firstFault_.syndrome[2]);
    }
    return kIOReturnIOError;
}

void NativeAdrenoA6xxSmmu::disableInterrupts()
{
    for(auto &entry:interrupts_) {
        if(entry.installed && entry.enabled) {
            native_adreno_smmu_disable_irq(entry.number);
            entry.enabled=false;
        }
    }
}

IOReturn NativeAdrenoA6xxSmmu::translate(unsigned bank,uint16_t sid,uint64_t address,
    bool privileged,bool instruction,bool write,uint64_t *physical)
{
    if(!prepared_ || !physical || address>UINT32_MAX ||
        (bank!=resources_.gpuBank && bank!=resources_.gmuBank)) {
        return kIOReturnBadArgument;
    }
    *physical=0;
    unsigned selected=2;
    for(unsigned index=0;index<2;++index) {
        const uint32_t start=resources_.smmuTbuSidStart[index];
        if(sid>=start && uint32_t(sid-start)<resources_.smmuTbuSidCount[index]) {
            selected=index;
            break;
        }
    }
    if(selected==2) {
        return kIOReturnBadArgument;
    }
    if(observe()) {
        return kIOReturnIOError;
    }
    IOMemoryMap *tbu=tbu_[selected].get();
    IOMemoryMap *statusMap=tbuStatus_[selected].get();
    const size_t cb=contextOffset(bank);
    uint32_t original=0;
    if(io_.read32(io_.cookie,cb,&original)) {
        return kIOReturnIOError;
    }
    Interrupt &entry=interrupts_[bank==resources_.gpuBank?1:2];
    IOReturn result=native_adreno_smmu_disable_irq(entry.number);
    if(result) {
        return result;
    }
    entry.enabled=false;
    bool halted=false,locked=false;
    const uint32_t halt=read32(tbu,0);
    write32(tbu,0,halt|HaltRequest);

    uint32_t fsr=0;
    io_.read32(io_.cookie,cb+0x58,&fsr);
    if((fsr&FaultMask) && (fsr&Stalled)) {
        io_.write32(io_.cookie,cb,original&~(ContextStall|ContextFaultInterrupt));
        io_.write32(io_.cookie,cb+0x58,fsr);
        io_.barrier(io_.cookie);
        io_.write32(io_.cookie,cb+8,1);
        io_.write32(io_.cookie,cb,original);
    }
    halted=waitBits(tbu,0x20,2,2);
    if(!halted) {
        result=kIOReturnTimeout;
    } else {
        io_.write32(io_.cookie,cb,original&~(ContextStall|ContextFaultInterrupt));
        io_.read32(io_.cookie,cb+0x58,&fsr);
        if(fsr&FaultMask) {
            io_.write32(io_.cookie,cb+0x58,fsr);
            io_.barrier(io_.cookie);
            if(fsr&Stalled) {
                io_.write32(io_.cookie,cb+8,1);
            }
        }
        locked=version_==0x01000000 || waitBits(statusMap,0,UINT32_MAX,1);
        if(!locked) {
            result=kIOReturnBusy;
        }
    }

    if(halted && locked) {
        for(unsigned attempt=0;attempt<3;++attempt) {
            const uint64_t sidControl=read64(tbu,0);
            write64(tbu,0,(sidControl&~UINT64_C(0x3ff))|(sid&0x3ff));
            write64(tbu,8,address);
            write64(tbu,0x30,UINT64_C(255)<<36);
            const uint32_t protection=2u|(privileged?1u:0u)|(instruction?4u:0u);
            write64(tbu,0x18,(protection<<6)|(15u<<2)|(write?2u:0u)|1u);

            bool completed=false;
            for(unsigned elapsed=0;elapsed<PollMicroseconds;++elapsed) {
                const uint32_t status=read32(tbu,0x20);
                io_.read32(io_.cookie,cb+0x58,&fsr);
                if(!(status&1) || (fsr&FaultMask)) {
                    completed=true;
                    break;
                }
                IODelay(1);
            }
            const uint64_t par=read64(tbu,0x28);
            io_.read32(io_.cookie,cb+0x58,&fsr);
            IOLog("NativeAdrenoSmmu: ECATS bank=%u sid=%04x va=%llx prot=%u write=%u attempt=%u par=%llx fsr=%08x complete=%u\n",
                bank,sid,address,protection,unsigned(write),attempt,par,fsr,unsigned(completed));
            result=!completed?kIOReturnTimeout:((par&1) || (fsr&FaultMask)?kIOReturnVMError:kIOReturnSuccess);
            if(!result) {
                /* DEBUG_PAR_PA contains the complete 36-bit byte address in
                 * bits 47:12, as extracted by the stock FIELD_GET operation. */
                *physical=(par&EcatsPhysicalAddressMask)>>EcatsPhysicalAddressShift;
                if(!*physical) {
                    result=kIOReturnVMError;
                }
            }
            if(fsr&FaultMask) {
                io_.write32(io_.cookie,cb+0x58,fsr);
                io_.barrier(io_.cookie);
                if(fsr&Stalled) {
                    io_.write32(io_.cookie,cb+8,1);
                }
            }
            write64(tbu,0x18,0);
            write64(tbu,8,0);
            write32(tbu,0,read32(tbu,0)&~0x3ffu);
            if(!result) {
                break;
            }
            /* QSMMUv500 can report the next successful translation as failed
             * after an earlier failed request. Every unsuccessful operation,
             * including a timeout, gets two retries after resetting ECATS. */
        }
    }

    if(locked && version_!=0x01000000) {
        write32(statusMap,0,0);
    }
    io_.write32(io_.cookie,cb,original);
    write32(tbu,0,read32(tbu,0)&~HaltRequest);
    read32(tbu,0x20);
    io_.barrier(io_.cookie);
    const IOReturn enable=native_adreno_smmu_enable_irq(entry.number);
    entry.enabled=enable==kIOReturnSuccess;
    if(!result) {
        result=enable;
    }
    if(result) {
        IOLog("NativeAdrenoSmmu: ECATS stopped bank=%u sid=%04x status=%08x halted=%u locked=%u\n",
            bank,sid,result,unsigned(halted),unsigned(locked));
    }
    return result;
}

IOReturn NativeAdrenoA6xxSmmu::checkTranslation(unsigned bank,uint16_t sid,uint64_t address,
    uint64_t physical,bool privileged,bool instruction,bool write)
{
    uint64_t translated=0;
    const IOReturn status=translate(bank,sid,address,privileged,instruction,write,&translated);
    if(status) {
        return status;
    }
    IOLog("NativeAdrenoSmmu: ECATS address va=%llx pa=%llx expected=%llx sid=%04x\n",
        address,translated,physical,sid);
    return translated==physical?kIOReturnSuccess:kIOReturnVMError;
}
