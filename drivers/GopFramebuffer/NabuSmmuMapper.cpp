/* SPDX-License-Identifier: MIT */
#include "NabuSmmuMapper.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <pexpert/pexpert.h>

extern "C" {extern volatile uint32_t native_legacy_mapper_calls[4];}

#define GPB_SMMU_BASE 0x15000000ULL   /* apps_smmu on SM8150 (live DT: apps-smmu@0x15000000) */
#define GPB_SMMU_WINDOW 0x100000ULL   /* apps_smmu window */
#define NABU_SMMU_BANK 55           /* SM8150 has 56 banks; ref/zzweiqing allocates from the top, bank 0 unused */
#define NABU_SMMU_ASID 4
#define NABU_SMMU_SLOT 8
#define NABU_SMMU_SID 0x800            /* mdss iommus = <&apps_smmu 0x800 0x420> (sm8150.dtsi) */

OSDefineMetaClassAndStructors(NabuSmmuMapper, IOMapper)

/* l645: pick a bank that SCTLR.M is clear on. ArmSmmu4KProgramDisabledContext returns
 * ArmSmmu4KBusy when the bank is already enabled, which NativeAdrenoSmmuContext::program
 * maps to kIOReturnIOError (0xE00002CA) -- the l644 board result. Bank 4 was taken.
 * Constraints (ArmSmmu4KProgram.c): bank >= s2, bank < banks, context_offset=(pages+bank)*stride,
 * pages=1<<(((id1>>28)&7)+1), stride=1<<(id1[31]?16:12). */
static unsigned findFreeSmmuBank(const ArmSmmu4KIo &io) {
    uint32_t id1=0,scr0=0;
    IOLog("nabu-smmu: scan id1=%08x scr0=%08x\n",
          (unsigned)(io.read32(io.cookie,0x24,&id1) ? 0u : id1),
          (unsigned)(io.read32(io.cookie,0x20,&scr0) ? 0u : scr0));
    if(io.read32(io.cookie,0x24,&id1)) return 0xffffffffu;
    unsigned shift=(id1>>31)?16:12;
    size_t pages=(size_t)1<<(((id1>>28)&7)+1), stride=(size_t)1<<shift;
    unsigned banks=id1&255, s2=(id1>>16)&255;
    for(unsigned b=(s2?s2:1); b<banks && b<255; ++b) {
        uint32_t v=0;
        if(io.read32(io.cookie,(pages+b)*stride,&v)) continue;
        uint32_t cbar=0; io.read32(io.cookie,(pages+b)*stride+0x4,&cbar);
        if(!(v&1) && !cbar) { IOLog("nabu-smmu: free bank=%u (s2=%u banks=%u) picked=%u\n",b,s2,banks,b); return b; }
    }
    IOLog("nabu-smmu: no free bank (s2=%u banks=%u)\n",s2,banks);
    return 0xffffffffu;
}

bool NabuSmmuMapper::init(OSDictionary *parameters) {
    if(!IOMapper::init(parameters))return false;
    fIsSystem=false;
    return true;
}

bool NabuSmmuMapper::attach(IOService *provider) {
    if(!IOMapper::attach(provider))return false;
    return true;
}

bool NabuSmmuMapper::start(IOService *provider) {
    if(!IOMapper::start(provider)){IOLog("nabu-smmu: super-start fail\n");return false;}
    IOReturn status=prepareMapper();
    logStatus("prepare",status);
    return status==kIOReturnSuccess;
}

void NabuSmmuMapper::stop(IOService *provider) {
    IOMapper::stop(provider);
}

void NabuSmmuMapper::free() {
    if(smmuMap_) {smmuMap_->release();smmuMap_=nullptr;}
    IOMapper::free();
}

bool NabuSmmuMapper::initHardware(IOService *provider) {
    return true;
}

IOReturn NabuSmmuMapper::prepareMapper() {
    // 1. Map the SMMU MMIO window
    IODeviceMemory *memory=IODeviceMemory::withRange(GPB_SMMU_BASE,GPB_SMMU_WINDOW);
    if(!memory)return kIOReturnNoMemory;
    smmuMap_=memory->map(kIOMapAnywhere|kIOMapInhibitCache);
    memory->release();
    if(!smmuMap_){IOLog("nabu-smmu: map-window fail\n");return kIOReturnNoMemory;}
    smmuVa_=reinterpret_cast<volatile unsigned char *>(smmuMap_->getVirtualAddress());
    smmuBytes_=smmuMap_->getLength();
    // 2. Bind the MMIO to ArmSmmu4KIo
    mmio_={smmuVa_,smmuBytes_};
    if(ArmSmmu4KBindMmio(&mmio_,&io_)!=ArmSmmu4KSuccess){
        IOLog("nabu-smmu: bind id0=%08x id1=%08x\n",
              (unsigned)*(volatile uint32_t *)(mmio_.mapping + 0x20),
              (unsigned)*(volatile uint32_t *)(mmio_.mapping + 0x24));
        return kIOReturnBadArgument;
    }
    ioBound_=true;
    // 3. Allocate the page-table pool. A 1 GiB window needs 512 L2 tables (each covering
    // 2 MiB) plus one L1 table: 513 pages ~ 2.1 MiB, so 1 MiB left the build at
    // ArmSmmu4KCapacity -> kIOReturnNoResources (l644 review).
    /* l645: 2 MiB pool. 512 MiB IOVA / 2 MiB per L2 = 256 L2 tables + 1 L1 = 257 pages ~= 1.03 MiB. */
    const size_t poolBytes=2*1024*1024;
    /* l646: output_bits must match the SMMU encoding (40, as in context_.prepare below).
     * With 32 the table is confined to physical addresses below 4 GiB, which a 6/8 GiB
     machine usually cannot satisfy -> kIOReturnIOError from allocate. */
    IOReturn status=tables_.allocate(poolBytes,40,true,poolBytes,false,false,true);
    if(status!=kIOReturnSuccess){IOLog("nabu-smmu: tables allocate status=0x%x\n",(unsigned)status);return status;}
    IOLog("nabu-smmu: tables bytes=%zu wc=%d spans=%zu\n", tables_.bytes(),
          (int)tables_.writeCombined(), tables_.spanCount());
    /* l651: the display has a dedicated bank. sid800 slot=3 -> s2cr=0x00000002 ->
     * CBNDX = s2cr & 0xff = bank 2 (the old (s2cr>>7)&0x1f read bank 0). sctlr=0xe0
     * (M=0) => bank 2 is an unenabled context we can take over; CBAR may be non-zero. */
    bank_=0xffffffffu;
    {
        uint32_t id1_scan=0; io_.read32(io_.cookie,0x24,&id1_scan);
        unsigned shift=(id1_scan>>31)?16:12;
        size_t pages_scan=(size_t)1<<(((id1_scan>>28)&7)+1), stride_scan=(size_t)1<<shift;
        for(unsigned i=0;i<94;++i) {
            uint32_t smr=0,s2cr=0;
            if(io_.read32(io_.cookie,0x800+4*i,&smr)) continue;
            if(!(smr&0x80000000u)) continue;
            if((smr&0xffffu)!=uint16_t(NABU_SMMU_SID)) continue;
            io_.read32(io_.cookie,0xc00+4*i,&s2cr);
            unsigned b=s2cr&0xff;
            uint32_t cbar=0,sctlr=0,cba2r=0;
            io_.read32(io_.cookie,(pages_scan+b)*stride_scan+0x4,&cbar);
            io_.read32(io_.cookie,(pages_scan+b)*stride_scan+0x0,&sctlr);
            io_.read32(io_.cookie,(pages_scan+b)*stride_scan+0x8,&cba2r);
            IOLog("nabu-smmu: sid800 slot=%u smr=%08x s2cr=%08x bank=%u cbar=%08x sctlr=%08x cba2r=%08x\n",
                  i,(unsigned)smr,(unsigned)s2cr,b,(unsigned)cbar,(unsigned)sctlr,(unsigned)cba2r);
            bank_=b;
        }
    }
    if(bank_==0xffffffffu) { bank_=findFreeSmmuBank(io_); }
    if(bank_==0xffffffffu){IOLog("nabu-smmu: no free bank\n");return kIOReturnNotReady;}
    status=context_.prepare(&tables_,0,nextIova_,iovaLimit_-nextIova_,NativeAdrenoSmmuContext::MaxLeaves,
        bank_,NABU_SMMU_ASID,4096,40,true);
    if(status!=kIOReturnSuccess){IOLog("nabu-smmu: prepare status=0x%x\n",(unsigned)status);return status;}
    prepared_=true;
    // 5. Program the context (enable the bank)
    {uint32_t id1p=0; io_.read32(io_.cookie,0x24,&id1p);
     IOLog("nabu-smmu: pre-program id1=%08x bank=%u\n",(unsigned)id1p,bank_);}
    status=context_.program(&io_);
    if(status!=kIOReturnSuccess){
        /* l647b: 0x0 is SCR0, 0x20 is IDR0, 0x24 is IDR1. The previous offsets read
         * SCR0/SCR1/IDR0 and mislabelled them, which inverted the id0/id1 reading. */
        IOLog("nabu-smmu: program status=0x%x bank=%u id0=%08x id1=%08x scr0=%08x\n",(unsigned)status,bank_,
              (unsigned)*(volatile uint32_t *)(mmio_.mapping + 0x20),
              (unsigned)*(volatile uint32_t *)(mmio_.mapping + 0x24),
              (unsigned)*(volatile uint32_t *)(mmio_.mapping + 0x0));
        return status;
    }
    programmed_=true;
    /* l651: skip attach. sid800 slot=3 already carries SMR(MASK=0x420, ID=0x800) and
     * S2CR(CBNDX=bank 2); rewriting SMR would destroy the firmware's MASK. */
    attached_=true;
    IOLog("n651: reuse sid800 bank=%u (attach skipped)\n",bank_);
    return kIOReturnSuccess;
}

IOReturn NabuSmmuMapper::mapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,
    uint64_t *mapAddress,uint64_t *mapLength) {
    if(!prepared_||!programmed_)return kIOReturnNotReady;
    // 1. Collect all physical spans
    uint64_t total=0;
    size_t spanCount=0;
    static const size_t kMaxSpans=4096;
    ArmSmmu4KRange ranges[kMaxSpans];
    uint64_t curOffset=offset;
    while(total<length && spanCount<kMaxSpans) {
        IOByteCount remaining=0;
        IOByteCount physical=memory->getPhysicalSegment(curOffset,&remaining,kIOMemoryMapperNone);
        if(!physical||remaining==0)break;
        ranges[spanCount]={nextIova_+total,uint64_t(physical),uint64_t(remaining),
            ArmSmmu4KRead|ArmSmmu4KWrite,0,2};
        ++spanCount;
        total+=remaining;
        curOffset+=remaining;
    }
    if(total<length)return kIOReturnUnsupported;
    // 2. Map through the SMMU
    ArmSmmu4KLiveRegion region={context_.arenaHandle(),context_.stateHandle(),ranges,spanCount};
    ArmSmmu4KLiveVisibility visibility={&tables_,nullptr};
    ArmSmmu4KLiveResult result={};
    arm_smmu_4k_live_now_us=0;
    IOReturn status=ArmSmmu4KLiveUpdateContextRanges(&io_,bank_,context_.configHandle(),
        &region,nullptr,0,&visibility,ranges,spanCount,1,4096,&result);
    if(status!=kIOReturnSuccess)return status;
    *mapAddress=nextIova_;
    *mapLength=length;
    nextIova_+=length;
    if(nextIova_>=iovaLimit_)nextIova_=0x40000000ULL;
    return kIOReturnSuccess;
}

IOReturn NabuSmmuMapper::iovmMapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,
    uint32_t mapOptions,const IODMAMapSpecification *mapSpec,IODMACommand *command,
    const IODMAMapPageList *pageList,uint64_t *mapAddress,uint64_t *mapLength) {
    if(!memory||!mapAddress||!mapLength)return kIOReturnBadArgument;
    *mapAddress=0;*mapLength=0;
    IOReturn status=mapMemory(memory,offset,length,mapAddress,mapLength);
    if(status==kIOReturnSuccess && *mapAddress && *mapLength)
        __atomic_fetch_add(&native_legacy_mapper_calls[0],1U,__ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&native_legacy_mapper_calls[1],1U,__ATOMIC_RELAXED);
    return status;
}

IOReturn NabuSmmuMapper::iovmUnmapMemory(IOMemoryDescriptor *memory,IODMACommand *command,
    uint64_t mapAddress,uint64_t mapLength) {
    __atomic_fetch_add(&native_legacy_mapper_calls[2],1U,__ATOMIC_RELAXED);
    return unmapMemory(mapAddress,mapLength);
}

IOReturn NabuSmmuMapper::unmapMemory(uint64_t mapAddress,uint64_t mapLength) {
    if(!prepared_||!programmed_)return kIOReturnNotReady;
    ArmSmmu4KLiveRegion region={context_.arenaHandle(),context_.stateHandle(),nullptr,0};
    ArmSmmu4KLiveVisibility visibility={&tables_,nullptr};
    ArmSmmu4KLiveResult result={};
    ArmSmmu4KRange range={mapAddress,0,mapLength,ArmSmmu4KRead|ArmSmmu4KWrite,0,2};
    IOReturn status=ArmSmmu4KLiveUpdateContextRanges(&io_,bank_,context_.configHandle(),
        &region,nullptr,0,&visibility,&range,1,0,4096,&result);
    return status;
}

IOReturn NabuSmmuMapper::iovmInsert(uint32_t options,uint64_t mapAddress,uint64_t offset,
    uint64_t physicalAddress,uint64_t length) {
    __atomic_fetch_add(&native_legacy_mapper_calls[3],1U,__ATOMIC_RELAXED);
    return kIOReturnUnsupported;
}

uint64_t NabuSmmuMapper::mapToPhysicalAddress(uint64_t mappedAddress) {
    return mappedAddress;
}

void NabuSmmuMapper::logStatus(const char *what,IOReturn status) {
    IOLog("nabu-smmu: %s status=0x%x\n",what,(unsigned)status);
}
