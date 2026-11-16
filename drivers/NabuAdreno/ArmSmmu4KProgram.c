/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KProgram.h"
#define FSR_FAULT UINT32_C(0xc00001fe)
static int read_image(const ArmSmmu4KIo *io,size_t gr1,size_t cb,unsigned bank,
    ArmSmmu4KContextImage *v)
{
    v->reserved_zero=0;
    return io->read32(io->cookie,gr1+bank*4,&v->cbar) ||
        io->read32(io->cookie,gr1+0x800+bank*4,&v->cba2r) ||
        io->read32(io->cookie,cb+0x30,&v->tcr) ||
        io->read32(io->cookie,cb+0x10,&v->tcr2) ||
        io->read64(io->cookie,cb+0x20,&v->ttbr0) ||
        io->read64(io->cookie,cb+0x28,&v->ttbr1) ||
        io->read32(io->cookie,cb+0x38,&v->mair0) ||
        io->read32(io->cookie,cb+0x3c,&v->mair1) ||
        io->read32(io->cookie,cb,&v->sctlr);
}
static int write32(const ArmSmmu4KIo *io,size_t at,uint32_t value,ArmSmmu4KProgramResult *r)
{
    ++r->writes;return io->write32(io->cookie,at,value);
}
static int write64(const ArmSmmu4KIo *io,size_t at,uint64_t value,ArmSmmu4KProgramResult *r)
{
    ++r->writes;return io->write64(io->cookie,at,value);
}
/* TLBIASID (+0x610, value = ASID) or, with all set, TLBIALL (+0x618, value ignored), then TLBSYNC/TLBSTATUS. */
static ArmSmmu4KStatus sync_tlb(const ArmSmmu4KIo *io,size_t cb,unsigned asid,unsigned all,
    unsigned budget,ArmSmmu4KProgramResult *r)
{
    r->tlb_synchronized=0;
    if(io->barrier(io->cookie) || write32(io,cb+(all?0x618:0x610),all?0:asid,r) ||
        write32(io,cb+0x7f0,UINT32_MAX,r) || io->barrier(io->cookie))return ArmSmmu4KIoError;
    for(unsigned i=0;i<budget;++i) {
        uint32_t status;++r->poll_reads;
        if(io->read32(io->cookie,cb+0x7f4,&status))return ArmSmmu4KIoError;
        if(!(status&1)) {
            /* Order the completed device observation before callers recycle
             * normal-memory tables or publish subsequent context work. */
            if(io->barrier(io->cookie))return ArmSmmu4KIoError;
            r->tlb_synchronized=1;return ArmSmmu4KSuccess;
        }
    }
    return ArmSmmu4KTimeout;
}
static ArmSmmu4KStatus sync_asid(const ArmSmmu4KIo *io,size_t cb,unsigned asid,
    unsigned budget,ArmSmmu4KProgramResult *r)
{
    return sync_tlb(io,cb,asid,0,budget,r);
}
static ArmSmmu4KStatus disable_sync(const ArmSmmu4KIo *io,size_t cb,unsigned asid,
    unsigned budget,ArmSmmu4KProgramResult *r)
{
    r->must_retain_tables=1;r->disabled_verified=0;
    uint32_t value;
    if(write32(io,cb,0,r) || io->barrier(io->cookie) || io->read32(io->cookie,cb,&value))return ArmSmmu4KIoError;
    if(value)return ArmSmmu4KStateMismatch;
    r->disabled_verified=1;r->enabled=0;
    ArmSmmu4KStatus status=sync_asid(io,cb,asid,budget,r);
    if(status==ArmSmmu4KSuccess)r->must_retain_tables=0;
    return status;
}
static ArmSmmu4KStatus prepare(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,unsigned budget,ArmSmmu4KProgramResult *r,
    ArmSmmu4KContextConfig *actual,ArmSmmu4KContextImage *image,size_t *gr1)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KProgramResult){0};
    r->must_retain_tables=1;
    if(!io || !config || !io->read32 || !io->read64 || !io->write32 || !io->write64 ||
        !io->barrier || io->register_bytes<0x2c || !budget || budget>1048576 || bank>255)return ArmSmmu4KArgument;
    r->phase=1;
    if(io->read32(io->cookie,0x20,&r->id0) || io->read32(io->cookie,0x24,&r->id1) ||
        io->read32(io->cookie,0x28,&r->id2))return ArmSmmu4KIoError;
    unsigned shift=(r->id1>>31)?16:12;
    size_t pages=(size_t)1<<(((r->id1>>28)&7)+1);
    size_t stride=(size_t)1<<shift;
    unsigned banks=r->id1&255,s2=(r->id1>>16)&255;
    if(!banks || banks>pages || s2>banks || bank<s2 || bank>=banks ||
        io->register_bytes<2*pages*stride)return ArmSmmu4KUnsupported;
    *gr1=stride;r->context_offset=(pages+bank)*stride;
    if(r->context_offset>io->register_bytes || io->register_bytes-r->context_offset<0x7f8 ||
        *gr1+0x804+bank*4>io->register_bytes)return ArmSmmu4KBadRange;
    *actual=*config;actual->id0=r->id0;actual->id2=r->id2;
    return ArmSmmu4KEncodeContext(actual,image);
}
ArmSmmu4KStatus ArmSmmu4KProgramDisabledContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,unsigned budget,ArmSmmu4KProgramResult *r)
{
    ArmSmmu4KContextConfig actual;ArmSmmu4KContextImage image;size_t gr1;
    ArmSmmu4KStatus status=prepare(io,bank,config,budget,r,&actual,&image,&gr1);
    if(status!=ArmSmmu4KSuccess)return status;
    size_t cb=(size_t)r->context_offset;uint32_t sctlr;
    if(io->read32(io->cookie,cb,&sctlr))return ArmSmmu4KIoError;
    if(sctlr&1){r->enabled=1;return ArmSmmu4KBusy;}
    r->must_retain_tables=1;r->phase=2;
    /* A disabled owned bank only. TCR2/TCR precede all TTBR accesses. */
    if(write32(io,cb,0,r) || io->barrier(io->cookie) ||
        write32(io,gr1+0x800+bank*4,image.cba2r,r) || write32(io,gr1+bank*4,image.cbar,r) ||
        write32(io,cb+0x10,image.tcr2,r) || write32(io,cb+0x30,image.tcr,r) ||
        write64(io,cb+0x20,image.ttbr0,r) || write64(io,cb+0x28,image.ttbr1,r) ||
        write32(io,cb+0x38,image.mair0,r) || write32(io,cb+0x3c,image.mair1,r) ||
        write32(io,cb+0x58,UINT32_MAX,r)) {status=ArmSmmu4KIoError;goto failed;}
    status=sync_asid(io,cb,actual.asid,budget,r);if(status!=ArmSmmu4KSuccess)goto failed;
    r->phase=3;
    if(read_image(io,gr1,cb,bank,&r->readback)) {status=ArmSmmu4KIoError;goto failed;}
    r->readback_valid=1;
    /* Verify configuration while M is still zero; never enable a bad granule. */
    sctlr=r->readback.sctlr;r->readback.sctlr=image.sctlr;
    status=ArmSmmu4KValidateContextReadback(&actual,&r->readback);r->readback.sctlr=sctlr;
    if(status!=ArmSmmu4KSuccess || sctlr) {status=ArmSmmu4KStateMismatch;goto failed;}
    r->readback_valid=0;
    if(io->barrier(io->cookie) || write32(io,cb,image.sctlr,r) || io->barrier(io->cookie)) {status=ArmSmmu4KIoError;goto failed;}
    r->phase=4;
    if(read_image(io,gr1,cb,bank,&r->readback)) {status=ArmSmmu4KIoError;goto failed;}
    r->readback_valid=1;r->enabled=r->readback.sctlr&1;
    if(io->read32(io->cookie,cb+0x58,&r->fault_status)) {status=ArmSmmu4KIoError;goto failed;}
    status=ArmSmmu4KValidateContextReadback(&actual,&r->readback);if(status!=ArmSmmu4KSuccess)goto failed;
    if(r->fault_status&FSR_FAULT) {status=ArmSmmu4KFault;goto failed;}
    r->phase=5;r->enabled=1;return ArmSmmu4KSuccess;
failed:
    /* Even a reported failed write may have reached hardware. On uncertainty
     * retain all tables/leases; never claim safe recycling after timeout. */
    r->readback_valid=0;(void)disable_sync(io,cb,actual.asid,budget,r);
    r->phase=6;return status;
}
ArmSmmu4KStatus ArmSmmu4KDisableOwnedContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,unsigned budget,ArmSmmu4KProgramResult *r)
{
    ArmSmmu4KContextConfig actual;ArmSmmu4KContextImage image;size_t gr1;
    ArmSmmu4KStatus status=prepare(io,bank,config,budget,r,&actual,&image,&gr1);
    if(status!=ArmSmmu4KSuccess)return status;
    size_t cb=(size_t)r->context_offset;r->must_retain_tables=1;
    if(read_image(io,gr1,cb,bank,&r->readback))return ArmSmmu4KIoError;
    r->readback_valid=1;uint32_t sctlr=r->readback.sctlr;
    /* An enabled bank must match the owned image (ValidateContextReadback, with its documented read-only bits); a
     * disabled one is compared with SCTLR substituted. */
    if(sctlr)status=ArmSmmu4KValidateContextReadback(&actual,&r->readback);
    else {r->readback.sctlr=image.sctlr;status=ArmSmmu4KValidateContextReadback(&actual,&r->readback);r->readback.sctlr=sctlr;}
    if(status!=ArmSmmu4KSuccess)return status; /* Another context: do not touch. */
    r->readback_valid=0;status=disable_sync(io,cb,actual.asid,budget,r);r->phase=status==ArmSmmu4KSuccess?7:6;
    return status;
}

ArmSmmu4KStatus ArmSmmu4KObserveOwnedContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,ArmSmmu4KProgramResult *r)
{
    ArmSmmu4KContextConfig actual;ArmSmmu4KContextImage image;size_t gr1;
    ArmSmmu4KStatus status=prepare(io,bank,config,1,r,&actual,&image,&gr1);
    if(status!=ArmSmmu4KSuccess)return status;
    if(read_image(io,gr1,(size_t)r->context_offset,bank,&r->readback))return ArmSmmu4KIoError;
    r->readback_valid=1;r->enabled=r->readback.sctlr&1;
    status=ArmSmmu4KValidateContextReadback(&actual,&r->readback);
    if(status!=ArmSmmu4KSuccess)return status;
    if(io->read32(io->cookie,(size_t)r->context_offset+0x58,&r->fault_status) || io->barrier(io->cookie))return ArmSmmu4KIoError;
    if(r->fault_status&FSR_FAULT)return ArmSmmu4KFault;
    r->phase=8;return ArmSmmu4KSuccess;
}

static ArmSmmu4KStatus synchronize(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,unsigned all,unsigned budget,ArmSmmu4KProgramResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KProgramResult){0};r->must_retain_tables=1;
    if(!budget || budget>1048576)return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=ArmSmmu4KObserveOwnedContext(io,bank,config,r);
    if(status!=ArmSmmu4KSuccess)return status;
    status=sync_tlb(io,(size_t)r->context_offset,config->asid,all,budget,r);
    if(status!=ArmSmmu4KSuccess)return status;
    ArmSmmu4KProgramResult after;
    status=ArmSmmu4KObserveOwnedContext(io,bank,config,&after);
    r->readback=after.readback;r->readback_valid=after.readback_valid;
    r->fault_status=after.fault_status;r->enabled=after.enabled;
    if(status!=ArmSmmu4KSuccess)return status;
    if(r->id0!=after.id0 || r->id1!=after.id1 || r->id2!=after.id2 ||
        r->context_offset!=after.context_offset)return ArmSmmu4KStateMismatch;
    r->phase=9;return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KSyncContextTlb(const ArmSmmu4KIo *io,uint64_t context_offset,unsigned asid,unsigned all,
    unsigned budget,ArmSmmu4KProgramResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KProgramResult){0};r->must_retain_tables=1;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !budget || budget>1048576 || !context_offset ||
        context_offset>io->register_bytes || io->register_bytes-context_offset<0x7f8)return ArmSmmu4KArgument;
    r->context_offset=context_offset;
    ArmSmmu4KStatus status=sync_tlb(io,(size_t)context_offset,asid,all,budget,r);
    if(status==ArmSmmu4KSuccess)r->phase=9;
    return status;
}
ArmSmmu4KStatus ArmSmmu4KSynchronizeOwnedContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,unsigned budget,ArmSmmu4KProgramResult *r)
{return synchronize(io,bank,config,0,budget,r);}
ArmSmmu4KStatus ArmSmmu4KInvalidateAllOwnedContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *config,unsigned budget,ArmSmmu4KProgramResult *r)
{return synchronize(io,bank,config,1,budget,r);}
