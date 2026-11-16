/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuContext.h"
#include "ArmSmmu4KMmio.h"
#include <IOKit/IOLib.h>

namespace {
bool contextOffset(const ArmSmmu4KIo *io,uint32_t id1,unsigned bank,size_t *page,size_t *offset)
{
    *page=size_t(1)<<((id1>>31)?16:12);
    const size_t pages=size_t(1)<<(((id1>>28)&7)+1);
    const unsigned banks=id1&255;
    *offset=(pages+bank)*(*page);
    return banks && bank<banks && *offset<=io->register_bytes &&
        io->register_bytes-*offset>=0x7f8 && *page+0x804+4*bank<=io->register_bytes;
}

bool readContext(const ArmSmmu4KIo *io,uint32_t id1,unsigned bank,
    ArmSmmu4KContextImage *image,size_t *offset,uint32_t *actlr,uint32_t *fsr)
{
    size_t page=0;
    if(!contextOffset(io,id1,bank,&page,offset)) {
        return false;
    }
    return !io->read32(io->cookie,page+4*bank,&image->cbar) &&
        !io->read32(io->cookie,page+0x800+4*bank,&image->cba2r) &&
        !io->read32(io->cookie,*offset,&image->sctlr) &&
        !io->read32(io->cookie,*offset+4,actlr) &&
        !io->read32(io->cookie,*offset+0x10,&image->tcr2) &&
        !io->read32(io->cookie,*offset+0x30,&image->tcr) &&
        !io->read64(io->cookie,*offset+0x20,&image->ttbr0) &&
        !io->read64(io->cookie,*offset+0x28,&image->ttbr1) &&
        !io->read32(io->cookie,*offset+0x38,&image->mair0) &&
        !io->read32(io->cookie,*offset+0x3c,&image->mair1) &&
        !io->read32(io->cookie,*offset+0x58,fsr);
}
}

void NativeAdrenoCaptureSmmuReport(const ArmSmmu4KIo *io,NativeAdrenoSmmuContext *context,
    NativeAdrenoGmuContextReport *report)
{
    report->program=*context->programResult();
    report->routes=*context->streamResult();
    report->program_phase=report->program.phase;
    report->program_writes=report->program.writes;
    report->program_readback_valid=report->program.readback_valid;
    report->program_enabled=report->program.enabled;
    report->program_status=context->programStatus();
    report->route_status=context->attachStatus();
    report->route_phase=report->routes.phase;
    report->route_writes=report->routes.writes;
    report->routes_verified=report->routes.streams_verified;
    report->actlr_status=context->actlrStatus();
    report->actlr_expected=0x303;
    io->read32(io->cookie,0x20,&report->id0);
    io->read32(io->cookie,0x24,&report->id1);
    io->read32(io->cookie,0x28,&report->id2);
    io->read32(io->cookie,0,&report->scr0);
    io->read32(io->cookie,0x48,&report->global_fault);
    size_t offset=0;
    uint32_t fsr=0;
    report->context_read_valid=readContext(io,report->id1,report->bank,&report->image,
        &offset,&report->actlr,&fsr);
    report->observed_context_offset=offset;
    report->context_fault=fsr;
    report->sctlr=report->image.sctlr;
    report->cbar=report->image.cbar;
    report->ttbr0=report->image.ttbr0;
    for(unsigned index=0;index<report->stream_count;++index) {
        const unsigned slot=report->streams[index].slot;
        io->read32(io->cookie,0x800+4*slot,&report->stream_smr[index]);
        io->read32(io->cookie,0xc00+4*slot,&report->stream_s2cr[index]);
    }
    report->smr=report->stream_smr[0];
    report->s2cr=report->stream_s2cr[0];
    if(context->prepared()) {
        ArmSmmu4KContextConfig actual=*context->configHandle();
        actual.id0=report->id0;
        actual.id2=report->id2;
        report->expected_valid=ArmSmmu4KEncodeContext(&actual,&report->expected)==ArmSmmu4KSuccess;
    }
}

void NativeAdrenoLogSmmuReport(const char *role,const NativeAdrenoGmuContextReport *report)
{
    const auto &program=report->program;
    const auto &routes=report->routes;
    IOLog("NativeAdrenoSmmu: role=%s step=%u status=%08x bank=%u asid=%u id=%08x/%08x/%08x scr0=%08x gfsr=%08x allocation=%u/%llx/%08x\n",
        role,report->step,report->status,report->bank,report->asid,report->id0,report->id1,
        report->id2,report->scr0,report->global_fault,report->allocation_index,
        report->allocation_bytes,report->allocation_status);
    IOLog("NativeAdrenoSmmu: role=%s ownership_status=%u phase=%u gfsr=%08x reads=%u verified=%u program=%u phase=%u writes=%u polls=%u cb=%llx valid=%u enabled=%u disabled=%u sync=%u retain=%u fsr=%08x\n",
        role,report->ownership_status,report->ownership.phase,report->ownership.global_fault,report->ownership.reads,
        report->ownership.streams_verified,report->program_status,program.phase,program.writes,
        program.poll_reads,program.context_offset,program.readback_valid,program.enabled,
        program.disabled_verified,program.tlb_synchronized,program.must_retain_tables,program.fault_status);
    IOLog("NativeAdrenoSmmu: role=%s actual read=%u cb=%llx fsr=%08x sctlr=%08x actlr=%08x cbar=%08x cba2r=%08x tcr=%08x tcr2=%08x mair=%08x/%08x ttbr=%016llx/%016llx actlr_status=%u\n",
        role,report->context_read_valid,report->observed_context_offset,report->context_fault,
        report->image.sctlr,report->actlr,report->image.cbar,report->image.cba2r,
        report->image.tcr,report->image.tcr2,report->image.mair0,report->image.mair1,
        report->image.ttbr0,report->image.ttbr1,report->actlr_status);
    IOLog("NativeAdrenoSmmu: role=%s expected valid=%u sctlr=%08x actlr=%08x cbar=%08x cba2r=%08x tcr=%08x tcr2=%08x mair=%08x/%08x ttbr=%016llx/%016llx\n",
        role,report->expected_valid,report->expected.sctlr,report->actlr_expected,
        report->expected.cbar,report->expected.cba2r,report->expected.tcr,report->expected.tcr2,
        report->expected.mair0,report->expected.mair1,report->expected.ttbr0,report->expected.ttbr1);
    IOLog("NativeAdrenoSmmu: role=%s program_readback sctlr=%08x cbar=%08x cba2r=%08x tcr=%08x tcr2=%08x mair=%08x/%08x ttbr=%016llx/%016llx routes=%u phase=%u reads=%u writes=%u verified=%u faulted=%u retain=%u\n",
        role,program.readback.sctlr,program.readback.cbar,program.readback.cba2r,
        program.readback.tcr,program.readback.tcr2,program.readback.mair0,program.readback.mair1,
        program.readback.ttbr0,program.readback.ttbr1,report->route_status,routes.phase,
        routes.reads,routes.writes,routes.streams_verified,routes.streams_faulted,routes.must_retain_resources);
    for(unsigned index=0;index<report->stream_count;++index) {
        IOLog("NativeAdrenoSmmu: role=%s request[%u] slot=%u sid=%04x bank=%u actual_smr=%08x actual_s2cr=%08x\n",
            role,index,report->streams[index].slot,report->streams[index].sid,report->bank,
            report->stream_smr[index],report->stream_s2cr[index]);
    }
}

void NativeAdrenoSnapshotSmmu(const ArmSmmu4KIo *io,const char *phase)
{
    uint32_t id0=0,id1=0,id2=0,id7=0,scr0=0,acr=0,gfsr=0,syn0=0,syn1=0,syn2=0;
    if(io->read32(io->cookie,0x20,&id0) || io->read32(io->cookie,0x24,&id1) ||
        io->read32(io->cookie,0x28,&id2)) {
        IOLog("NativeAdrenoSmmu: snapshot=%s ID register access failed bytes=%lx\n",phase,io->register_bytes);
        return;
    }
    io->read32(io->cookie,0x3c,&id7);
    io->read32(io->cookie,0,&scr0);
    io->read32(io->cookie,0x10,&acr);
    io->read32(io->cookie,0x48,&gfsr);
    io->read32(io->cookie,0x50,&syn0);
    io->read32(io->cookie,0x54,&syn1);
    io->read32(io->cookie,0x58,&syn2);
    IOLog("NativeAdrenoSmmu: snapshot=%s bytes=%lx id=%08x/%08x/%08x id7=%08x scr0=%08x acr=%08x gfsr=%08x syn=%08x/%08x/%08x banks=%u s2banks=%u slots=%u\n",
        phase,io->register_bytes,id0,id1,id2,id7,scr0,acr,gfsr,syn0,syn1,syn2,
        id1&255,(id1>>16)&255,id0&255);
    const unsigned slots=(id0&255)<64?(id0&255):64;
    bool referenced[64]={};
    const bool extended=id0&(1u<<8);
    for(unsigned slot=0;slot<slots;++slot) {
        uint32_t smr=0,s2cr=0;
        const bool readOk=!io->read32(io->cookie,0x800+4*slot,&smr) &&
            !io->read32(io->cookie,0xc00+4*slot,&s2cr);
        const bool valid=extended?bool(s2cr&(1u<<10)):bool(smr&(1u<<31));
        const unsigned bank=s2cr&255;
        if(valid && !(s2cr&(3u<<16)) && bank<64) {
            referenced[bank]=true;
        }
        IOLog("NativeAdrenoSmmu: snapshot=%s slot=%u read=%u smr=%08x s2cr=%08x valid=%u sid=%04x mask=%04x type=%u bank=%u\n",
            phase,slot,readOk,smr,s2cr,valid,smr&0xffff,(smr>>16)&(extended?0xffff:0x7fff),
            (s2cr>>16)&3,bank);
    }
    const unsigned banks=(id1&255)<64?(id1&255):64;
    for(unsigned bank=0;bank<banks;++bank) {
        size_t page=0,offset=0;
        if(!contextOffset(io,id1,bank,&page,&offset)) {
            IOLog("NativeAdrenoSmmu: snapshot=%s bank=%u lies beyond mapped register window\n",phase,bank);
            break;
        }
        uint32_t sctlr=0;
        if(io->read32(io->cookie,offset,&sctlr)) {
            continue;
        }
        if(bank>1 && !(sctlr&1) && !referenced[bank]) {
            continue;
        }
        ArmSmmu4KContextImage image{};
        uint32_t actlr=0,fsr=0;
        const bool readOk=readContext(io,id1,bank,&image,&offset,&actlr,&fsr);
        IOLog("NativeAdrenoSmmu: snapshot=%s bank=%u cb=%lx read=%u sctlr=%08x actlr=%08x cbar=%08x cba2r=%08x tcr=%08x tcr2=%08x fsr=%08x mair=%08x/%08x ttbr=%016llx/%016llx\n",
            phase,bank,offset,readOk,image.sctlr,actlr,image.cbar,image.cba2r,image.tcr,
            image.tcr2,fsr,image.mair0,image.mair1,image.ttbr0,image.ttbr1);
    }
}
IOReturn NativeAdrenoGmuContext::bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,
    unsigned slot,uint16_t sid,void (*progress)(uint32_t),NativeAdrenoGmuContextReport *report,
    uint16_t sidAlias,unsigned aliasSlot)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    if(aliasSlot==UINT32_MAX) {
        aliasSlot=slot+1;
    }
    if(!io || bank>63 || !asid || asid>0xffff || slot>127 ||
        (sidAlias && (aliasSlot>127 || aliasSlot==slot)))return kIOReturnBadArgument;
    used_=true;
    report->bank=bank;
    report->asid=asid;
    report->stream_count=sidAlias?2:1;
    report->streams[0]={slot,sid};
    report->streams[1]={aliasSlot,sidAlias};
    IOReturn status=kIOReturnSuccess;
    auto step=[&](Step s) {report->step=s;if(progress)progress(s);};
    step(Allocate);
    /* Stage-1 output size is ID2.OAS, as arm_smmu_init_domain_context uses smmu->ipa_size; tables and
     * payloads must then lie below it. 40 bits is this driver's own allocation ceiling. */
    static const uint8_t oas_bits[16]={32,36,40,42,44,48};
    if(io->read32(io->cookie,0x28,&report->id2))status=kIOReturnIOError;
    else {
        const unsigned code=(report->id2>>4)&15;
        report->output_bits=code<6?oas_bits[code]:0;
        if(report->output_bits>40)report->output_bits=40;
        if(!report->output_bits)status=kIOReturnUnsupported;
    }
    if(!status) {
        report->allocation_bytes=TablePages*0x1000;
        status=gmu_.allocate(NativeAdrenoGmuDefaultSizes(TablePages*0x1000),report->output_bits);
        report->allocation_status=status;
    }
    if(!status) {
        step(Prepare);
        status=smmu_.prepare(&gmu_.tables(),0,0,GmuApertureBytes,NativeAdrenoSmmuContext::MaxLeaves,
            bank,asid,1000,report->output_bits,false);
    }
    if(!status) {
        step(Ownership);
        report->ownership_status=ArmSmmu4KCheckFaultedStreams(io,report->streams,
            report->stream_count,bank,smmu_.configuration()->stream_shareability,&report->ownership);
        if(report->ownership_status!=ArmSmmu4KSuccess) {
            status=kIOReturnIOError;
        } else if(((report->ownership.id1>>16)&255)==0) {
            /* NABU-A640-NO-STAGE2: say it once, at the step that used to return Busy. id1 is the
             * same word the role line prints, so this cannot be a stale copy of a guess. */
            IOLog("NativeAdrenoSmmu: no stage-2 bank (id1=%08x); ownership accepted on SMR+stage-1 alone\n",
                report->ownership.id1);
        }
    }
    if(!status) {
        step(Program);
        status=smmu_.program(io);
        const ArmSmmu4KProgramResult *p=smmu_.programResult();
        report->program_phase=p->phase;report->program_writes=p->writes;
        report->program_readback_valid=p->readback_valid;report->program_enabled=p->enabled;
        report->id1=p->id1;report->program_status=smmu_.programStatus();
        report->image=p->readback;
        ArmSmmu4KContextConfig actual=*smmu_.configHandle();
        actual.id0=p->id0;actual.id2=p->id2;
        ArmSmmu4KContextImage want;
        if(p->phase>=3 && ArmSmmu4KEncodeContext(&actual,&want)==ArmSmmu4KSuccess) {
            const ArmSmmu4KContextImage &got=p->readback;
            const uint32_t sctlr_want=p->enabled?want.sctlr:0;
            report->mismatch=(got.cbar!=want.cbar)|(got.cba2r!=want.cba2r)<<1|(got.tcr!=want.tcr)<<2|
                (got.tcr2!=want.tcr2)<<3|(got.mair0!=want.mair0)<<4|(got.mair1!=want.mair1)<<5|
                (got.ttbr0!=want.ttbr0)<<6|(got.ttbr1!=want.ttbr1)<<7|(got.sctlr!=sctlr_want)<<8;
        }
    }
    if(!status) {
        step(Attach);
        const ArmSmmu4KStream routes[2]={{slot,sid},{aliasSlot,sidAlias}};
        status=smmu_.attach(routes,sidAlias?2:1);
        const ArmSmmu4KStreamResult *s=smmu_.streamResult();
        report->route_status=smmu_.attachStatus();
        report->route_phase=s->phase;report->route_writes=s->writes;report->routes_verified=s->streams_verified;
    }
    if(!status) {
        step(VmAttach);
        NativeAdrenoVmRegion lower={&gmu_.tables(),smmu_.arenaHandle(),smmu_.stateHandle()};
        status=vm_.attach(io,smmu_.configHandle(),smmu_.streamsHandle(),&lower,nullptr,64,4000);
    }
    if(!status) {
        step(Map);
        status=gmu_.mapInto(&vm_);
        report->spans=uint32_t(vm_.spanCount());
    }
    if(!status) {
        step(Ready);
        ready_=true;
    }
    report->status=status;
    NativeAdrenoCaptureSmmuReport(io,&smmu_,report);
    return status;
}
/* Lives for the rest of the boot: the context bank points at its tables. */
static NativeAdrenoGmuContext *board_context;
extern "C" IOReturn NativeAdrenoGmuContextBringup(volatile uint8_t *smmu,size_t bytes,
    unsigned bank,unsigned asid,unsigned slot,unsigned sid,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report)
{
    static ArmSmmu4KMmio mmio;
    static ArmSmmu4KIo io;
    NativeAdrenoGmuContext *&context=board_context;
    if(!report || context || !smmu || !bytes || sid>0xffff)return kIOReturnBadArgument;
    mmio={reinterpret_cast<volatile unsigned char *>(smmu),bytes};
    if(ArmSmmu4KBindMmio(&mmio,&io)!=ArmSmmu4KSuccess)return kIOReturnBadArgument;
    context=new NativeAdrenoGmuContext;
    if(!context)return kIOReturnNoMemory;
    return context->bringUp(&io,bank,asid,slot,uint16_t(sid),progress,report);
}
extern "C" IOReturn NativeAdrenoGmuContextPayloads(NativeAdrenoGmuPayload out[5])
{
    if(!out)return kIOReturnBadArgument;
    for(unsigned i=0;i<5;++i)out[i]={};
    if(!board_context || !board_context->ready())return kIOReturnNotReady;
    NativeAdrenoGmuAllocator &gmu=board_context->allocator();
    const NativeAdrenoGmuMemoryLayout &layout=gmu.layout();
    const NativeAdrenoMemory *objects[5]={&gmu.icache(),&gmu.dummy(),&gmu.debug(),&gmu.log(),&gmu.hfi()};
    const uint64_t iovas[5]={layout.icache_iova,layout.dummy_iova,layout.debug_iova,layout.log_iova,layout.hfi_iova};
    for(unsigned i=0;i<5;++i) {
        const NativeAdrenoMemory *m=objects[i];
        if(m->spanCount()!=1 || m->spans()[0].bytes!=m->bytes())return kIOReturnUnsupported;
        out[i]={m->spans()[0].physical,m->bytes(),iovas[i]};
    }
    return kIOReturnSuccess;
}
