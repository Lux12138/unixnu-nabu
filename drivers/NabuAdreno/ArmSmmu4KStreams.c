/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KStreams.h"
#define FAULT (UINT32_C(2)<<16)
static int rd(const ArmSmmu4KIo *io,size_t at,uint32_t *v,ArmSmmu4KStreamResult *r)
{++r->reads;return io->read32(io->cookie,at,v);}
static int wr(const ArmSmmu4KIo *io,size_t at,uint32_t v,ArmSmmu4KStreamResult *r)
{++r->writes;return io->write32(io->cookie,at,v);}
static unsigned exids(uint32_t id){return (id>>8)&1;}
static uint32_t valid_smr(uint32_t id){return exids(id)?0:UINT32_C(1)<<31;}
static uint32_t valid_s2cr(uint32_t id){return exids(id)?1u<<10:0;}
static int valid(uint32_t id,uint32_t smr,uint32_t s2cr)
{return exids(id)?!!(s2cr&(1u<<10)):!!(smr&(UINT32_C(1)<<31));}
static int shareability_valid(ArmSmmu4KShareability shareability)
{
    return shareability==ArmSmmu4KShareabilityFromTransaction ||
        shareability==ArmSmmu4KShareabilityNonShareable;
}
static uint32_t s2cr_shareability(ArmSmmu4KShareability shareability)
{
    return (uint32_t)shareability<<8;
}
static ArmSmmu4KStatus global(const ArmSmmu4KIo *io,ArmSmmu4KStreamResult *r)
{
    if(!io || !io->read32 || !io->write32 || !io->barrier || io->register_bytes<0xc04)return ArmSmmu4KArgument;
    if(rd(io,0x20,&r->id0,r) || rd(io,0x24,&r->id1,r) || rd(io,0,&r->scr0,r) || rd(io,0x48,&r->global_fault,r))return ArmSmmu4KIoError;
    if(!(r->id0&(1u<<27)) || !(r->id0&255) || 0xc00u+4*(r->id0&255)>io->register_bytes)return ArmSmmu4KUnsupported;
    /* Real global enable (CLIENTPD clear), global fault reporting (GFRE/GFIE) and unmatched-stream fault
     * policy (USFCFG). Configuration-access fault reporting (GCFGFRE/GCFGFIE) is not required: Linux sets
     * it only in its own global reset, and firmware that owns sCR0 may leave it clear (an Adreno SMMU read
     * 0x002f0406). It reports bad register accesses, it does not change translation or bypass. */
    if((r->scr0&0x407)!=0x406 || !!(r->scr0&8)!=exids(r->id0))return ArmSmmu4KStateMismatch;
    return r->global_fault?ArmSmmu4KFault:ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus scan(const ArmSmmu4KIo *io,const ArmSmmu4KStream *s,unsigned n,
    unsigned bank,ArmSmmu4KShareability shareability,unsigned attached,ArmSmmu4KStreamResult *r)
{
    const uint32_t attributes=s2cr_shareability(shareability);
    const uint32_t translation=bank|attributes|valid_s2cr(r->id0);
    const uint32_t fault=FAULT|attributes;
    unsigned slots=r->id0&255;uint32_t sidmask=exids(r->id0)?0xffffu:(1u<<((r->id0>>9)&15))-1;
    /* NABU-A640-NO-STAGE2: IDR1.S2 is the stage-2 bank count. This instance reports 0, and on it the
     * S2CR write is accepted and discarded (measured on all six leased slots: request 0x00020300,
     * read back the old value), so "S2CR reads FAULT" can never be satisfied. With no stage-2 bank
     * there is nothing for S2CR to translate: matching the stream on SMR + stage-1 is the whole
     * contract. A valid SMR that names someone else still blocks -- we never steal a live route. */
    const int hasStage2=((r->id1>>16)&255)!=0;
    for(unsigned i=0;i<n;++i) {
        if(s[i].slot>=slots || (s[i].sid&~sidmask))return ArmSmmu4KArgument;
        for(unsigned j=0;j<i;++j)if(s[i].slot==s[j].slot || s[i].sid==s[j].sid)return ArmSmmu4KOverlap;
    }
    r->streams_verified=0;
    for(unsigned slot=0;slot<slots;++slot) {
        uint32_t smr,s2cr;
        if(rd(io,0x800+4*slot,&smr,r) || rd(io,0xc00+4*slot,&s2cr,r))return ArmSmmu4KIoError;
        unsigned own=n;
        for(unsigned i=0;i<n;++i)if(s[i].slot==slot){own=i;break;}
        if(own<n) {
            unsigned live=valid(r->id0,smr,s2cr);
            if(attached) {
                if(smr!=(valid_smr(r->id0)|s[own].sid) || s2cr!=translation) {
                    return ArmSmmu4KStateMismatch;
                }
            } else if(hasStage2 ?
                ((s2cr&~valid_s2cr(r->id0))!=fault || (live && smr!=(valid_smr(r->id0)|s[own].sid))) :
                (live && smr!=(valid_smr(r->id0)|s[own].sid))) {
                return ArmSmmu4KBusy;
            }
            ++r->streams_verified;
        } else if(valid(r->id0,smr,s2cr)) {
            if(!(s2cr&(3u<<16)) && (s2cr&255)==bank)return ArmSmmu4KBusy;
            uint32_t mask=(smr>>16)&(exids(r->id0)?0xffffu:0x7fffu),id=smr&0xffff;
            for(unsigned i=0;i<n;++i)if(!((s[i].sid^id)&~mask&sidmask))return ArmSmmu4KOverlap;
        }
    }
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KCheckFaultedStreams(const ArmSmmu4KIo *io,const ArmSmmu4KStream *s,
    unsigned n,unsigned bank,ArmSmmu4KShareability shareability,ArmSmmu4KStreamResult *r)
{
    if(!r) {
        return ArmSmmu4KArgument;
    }

    *r=(ArmSmmu4KStreamResult){0};
    r->must_retain_resources=1;
    if(!s || !n || n>8 || bank>255 || !shareability_valid(shareability)) {
        return ArmSmmu4KArgument;
    }

    ArmSmmu4KStatus status=global(io,r);
    if(status) {
        return status;
    }

    return scan(io,s,n,bank,shareability,0,r);
}
static int quarantine(const ArmSmmu4KIo *io,const ArmSmmu4KStreamState *state,unsigned count,ArmSmmu4KStreamResult *r)
{
    const uint32_t fault=FAULT|s2cr_shareability(state->stream_shareability);
    r->streams_faulted=0;int failed=0;
    for(unsigned i=0;i<count;++i) {
        size_t at=0xc00+4*state->streams[i].slot;uint32_t value;
        /* First stop translation. Only after confirmed FAULT may we restore
         * the exact owned SID and make EXIDS matching valid again. This also
         * handles an indeterminate/failed SMR publication without activating
         * an old or corrupted SID from an initially invalid slot. */
        size_t smr=0x800+4*state->streams[i].slot;
        uint32_t match=valid_smr(state->id0)|state->streams[i].sid;
        if(wr(io,at,fault,r) || io->barrier(io->cookie) || rd(io,at,&value,r) || value!=fault ||
            wr(io,smr,match,r) || io->barrier(io->cookie) || rd(io,smr,&value,r) || value!=match ||
            wr(io,at,fault|valid_s2cr(state->id0),r) || io->barrier(io->cookie) ||
            rd(io,at,&value,r) || value!=(fault|valid_s2cr(state->id0))) {
            failed=1;
        } else {
            ++r->streams_faulted;
        }
    }
    return failed;
}
static ArmSmmu4KStatus observe(const ArmSmmu4KIo *io,const ArmSmmu4KStreamState *s,
    const ArmSmmu4KContextConfig *c,ArmSmmu4KStreamResult *r)
{
    ArmSmmu4KStatus status=global(io,r);if(status)return status;
    if(r->id0!=s->id0 || r->scr0!=s->scr0)return ArmSmmu4KStateMismatch;
    status=ArmSmmu4KObserveOwnedContext(io,s->bank,c,&r->context);if(status)return status;
    if(c->stream_shareability!=s->stream_shareability) {
        return ArmSmmu4KStateMismatch;
    }
    status=scan(io,s->streams,s->count,s->bank,s->stream_shareability,1,r);
    if(status) {
        return status;
    }
    return io->barrier(io->cookie)?ArmSmmu4KIoError:ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KObserveStreams(const ArmSmmu4KIo *io,const ArmSmmu4KStreamState *s,
    const ArmSmmu4KContextConfig *c,ArmSmmu4KStreamResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KStreamResult){0};r->must_retain_resources=1;
    if(!s || !s->attached || s->poisoned || !s->count || s->count>8)return ArmSmmu4KArgument;
    return observe(io,s,c,r);
}
ArmSmmu4KStatus ArmSmmu4KObserveOwnedStreams(const ArmSmmu4KIo *io,const ArmSmmu4KStreamState *s,
    const ArmSmmu4KContextConfig *c,ArmSmmu4KStreamResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KStreamResult){0};r->must_retain_resources=1;
    if(!s || !s->attached || s->poisoned || !s->count || s->count>8)return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=global(io,r);if(status)return status;
    if(r->id0!=s->id0 || r->scr0!=s->scr0)return ArmSmmu4KStateMismatch;
    status=ArmSmmu4KObserveOwnedContext(io,s->bank,c,&r->context);if(status)return status;
    if(c->stream_shareability!=s->stream_shareability) {
        return ArmSmmu4KStateMismatch;
    }

    const uint32_t translation=s->bank|s2cr_shareability(s->stream_shareability)|valid_s2cr(r->id0);
    for(unsigned i=0;i<s->count;++i) {
        uint32_t smr,s2cr;
        if(rd(io,0x800+4*s->streams[i].slot,&smr,r) || rd(io,0xc00+4*s->streams[i].slot,&s2cr,r))return ArmSmmu4KIoError;
        if(smr!=(valid_smr(r->id0)|s->streams[i].sid) || s2cr!=translation) {
            return ArmSmmu4KStateMismatch;
        }
        ++r->streams_verified;
    }
    return io->barrier(io->cookie)?ArmSmmu4KIoError:ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KAttachStreams(const ArmSmmu4KIo *io,ArmSmmu4KStreamState *state,
    const ArmSmmu4KStream *s,unsigned n,unsigned bank,const ArmSmmu4KContextConfig *c,ArmSmmu4KStreamResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KStreamResult){0};r->must_retain_resources=1;
    if(!state || !s || !n || n>8 || bank>255)return ArmSmmu4KArgument;
    if(state->attempted || state->attached || state->poisoned)return ArmSmmu4KBusy;
    r->phase=1;ArmSmmu4KStatus status=global(io,r);if(status)return status;
    status=ArmSmmu4KObserveOwnedContext(io,bank,c,&r->context);if(status)return status;
    status=scan(io,s,n,bank,c->stream_shareability,0,r);
    if(status) {
        return status;
    }

    state->attempted=1;state->id0=r->id0;state->scr0=r->scr0;state->count=n;state->bank=bank;
    state->stream_shareability=c->stream_shareability;
    for(unsigned i=0;i<n;++i)state->streams[i]=s[i];
    const uint32_t translation=bank|s2cr_shareability(state->stream_shareability);
    unsigned touched=0;r->phase=2;
    for(unsigned i=0;i<n;++i) {
        uint32_t value;size_t smr=0x800+4*s[i].slot,s2cr=0xc00+4*s[i].slot;
        touched=i+1;
        /* In EXIDS mode make this leased slot invalid until its new SID is
         * installed. All masters are quiesced, and unmatched SIDs fault. */
        if(wr(io,s2cr,translation,r) || io->barrier(io->cookie) || rd(io,s2cr,&value,r)) {
            status=ArmSmmu4KIoError;
            goto failed;
        }
        if(value!=translation) {
            status=ArmSmmu4KStateMismatch;
            goto failed;
        }
        if(wr(io,smr,valid_smr(r->id0)|s[i].sid,r) || io->barrier(io->cookie) || rd(io,smr,&value,r)) {status=ArmSmmu4KIoError;goto failed;}
        if(value!=(valid_smr(r->id0)|s[i].sid)){status=ArmSmmu4KStateMismatch;goto failed;}
        if(exids(r->id0) && (wr(io,s2cr,translation|valid_s2cr(r->id0),r) || io->barrier(io->cookie))) {
            status=ArmSmmu4KIoError;
            goto failed;
        }
    }
    r->phase=3;status=observe(io,state,c,r);if(status)goto failed;
    state->attached=1;r->phase=4;return ArmSmmu4KSuccess;
failed:
    state->poisoned=1;state->attached=0;r->phase=5;
    (void)quarantine(io,state,touched,r);return status;
}
ArmSmmu4KStatus ArmSmmu4KFaultStreams(const ArmSmmu4KIo *io,ArmSmmu4KStreamState *s,ArmSmmu4KStreamResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KStreamResult){0};r->must_retain_resources=1;
    if(!s || !s->attached || s->poisoned || !s->count || s->count>8 ||
        !shareability_valid(s->stream_shareability)) {
        return ArmSmmu4KArgument;
    }
    ArmSmmu4KStatus status=global(io,r);if(status)return status;
    if(r->id0!=s->id0 || r->scr0!=s->scr0)return ArmSmmu4KStateMismatch;
    status=scan(io,s->streams,s->count,s->bank,s->stream_shareability,1,r);
    if(status) {
        return status;
    }
    r->phase=6;
    if(quarantine(io,s,s->count,r)){s->poisoned=1;s->attached=0;return ArmSmmu4KIoError;}
    s->attached=0;r->phase=7;return ArmSmmu4KSuccess;
}
