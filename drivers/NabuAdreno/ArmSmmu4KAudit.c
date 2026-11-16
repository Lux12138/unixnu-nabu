/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KAudit.h"
#define PAGE UINT64_C(4096)
#define ADDRESS UINT64_C(0x0000fffffffff000)
typedef struct {
    const ArmSmmu4KTableIo *io;
    const ArmSmmu4KConfig *c;
    const ArmSmmu4KResult *built;
    const ArmSmmu4KAuditRegion *a;
    ArmSmmu4KTableAuditResult *r;
    size_t range;
    uint64_t offset;
} Walk;
static int intersects(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{return a<b+bn && b<a+an;}
static int incompatible(const ArmSmmu4KRange *a,const ArmSmmu4KRange *b)
{return a->writeback!=b->writeback || a->shareability!=b->shareability || ((a->permissions^b->permissions)&ArmSmmu4KPrivileged);}
static ArmSmmu4KStatus validate(const ArmSmmu4KConfig *c,const ArmSmmu4KResult *b,
    const ArmSmmu4KAuditRegion *a,uint64_t root)
{
    if(!c || !b || !a || !a->ranges || !a->range_count || !a->visited ||
        !a->capacity_pages || a->capacity_pages>SIZE_MAX/4096 ||
        a->visited_bytes<a->capacity_pages || !a->read_budget ||
        c->input_bits<32 || c->input_bits>48 ||
        !(c->output_bits==32 || c->output_bits==36 || c->output_bits==40 ||
          c->output_bits==42 || c->output_bits==44 || c->output_bits==48) ||
        !c->leaf_budget || c->leaf_budget>(SIZE_MAX-1)/3 ||
        a->range_count>SIZE_MAX/sizeof(*a->ranges))return ArmSmmu4KArgument;
    if(a->range_count>c->leaf_budget)return ArmSmmu4KBudget;
    uint64_t limit=UINT64_C(1)<<c->output_bits,span=UINT64_C(1)<<c->input_bits;
    uint64_t pool_bytes=(uint64_t)a->capacity_pages*PAGE;
    if((c->table_physical&4095) || c->table_physical>=limit || pool_bytes>limit-c->table_physical)
        return ArmSmmu4KBadRange;
    if(root!=c->table_physical || b->root_physical!=root || b->mair!=0xff44 ||
        !b->table_pages || b->table_pages>a->capacity_pages || !b->leaf_pages ||
        b->start_level!=4-(c->input_bits-12+8)/9 || b->upper_region>1)
        return ArmSmmu4KStateMismatch;
    if(b->table_pages>a->read_budget/512)return ArmSmmu4KBudget;
    size_t leaves=0;uint64_t end=0;
    for(size_t i=0;i<a->range_count;++i) {
        const ArmSmmu4KRange *r=&a->ranges[i];
        if(!r->bytes || ((r->iova|r->physical|r->bytes)&4095) ||
            !(r->permissions&ArmSmmu4KRead) || (r->permissions&~15u) ||
            r->writeback>1 || (r->shareability!=2 && r->shareability!=3) ||
            r->physical>=limit || r->bytes>limit-r->physical ||
            (b->upper_region?(r->iova<UINT64_MAX-span+1 || r->bytes-1>UINT64_MAX-r->iova):
             (r->iova>=span || r->bytes>span-r->iova)))return ArmSmmu4KBadRange;
        if(i && r->iova<=end)return ArmSmmu4KOverlap;
        end=r->iova+r->bytes-1;
        if(intersects(r->physical,r->bytes,c->table_physical,pool_bytes))return ArmSmmu4KOverlap;
        for(size_t j=0;j<i;++j)if(incompatible(r,&a->ranges[j]) &&
            intersects(r->physical,r->bytes,a->ranges[j].physical,a->ranges[j].bytes))return ArmSmmu4KOverlap;
        uint64_t pages=r->bytes/PAGE;
        if(pages>c->leaf_budget-leaves)return ArmSmmu4KBudget;
        leaves+=(size_t)pages;
    }
    return leaves==b->leaf_pages?ArmSmmu4KSuccess:ArmSmmu4KStateMismatch;
}
static ArmSmmu4KStatus walk(Walk *w,uint64_t table,unsigned level,uint64_t base)
{
    if((table&4095) || table<w->c->table_physical ||
        (table-w->c->table_physical)/PAGE>=w->a->capacity_pages)return ArmSmmu4KBadRange;
    size_t page=(size_t)((table-w->c->table_physical)/PAGE);
    if(w->a->visited[page])return ArmSmmu4KOverlap;
    if(w->r->table_pages>=w->built->table_pages)return ArmSmmu4KStateMismatch;
    w->a->visited[page]=1;++w->r->table_pages;
    size_t initial_leaves=w->r->leaf_pages;
    unsigned shift=39-9*level;
    unsigned width=level==w->built->start_level?w->c->input_bits-shift:9;
    for(unsigned i=0;i<512;++i) {
        w->r->descriptor_physical=table+8*i;w->r->descriptor=0;
        w->r->level=level;w->r->iova=base|((uint64_t)i<<shift);w->r->expected_descriptor=0;
        if(w->r->descriptor_reads==w->a->read_budget)return ArmSmmu4KBudget;
        ++w->r->descriptor_reads;
        uint64_t d;
        if(w->io->read_descriptor(w->io->cookie,table+8*i,&d))return ArmSmmu4KIoError;
        w->r->descriptor=d;
        if(!d)continue;
        if(i>=(1u<<width) || (d&3)!=3)return ArmSmmu4KStateMismatch;
        if(level<3) {
            if(d!=((d&ADDRESS)|3))return ArmSmmu4KStateMismatch;
            ArmSmmu4KStatus status=walk(w,d&ADDRESS,level+1,base|((uint64_t)i<<shift));
            if(status)return status;
        } else {
            if(w->range==w->a->range_count)return ArmSmmu4KStateMismatch;
            const ArmSmmu4KRange *r=&w->a->ranges[w->range];
            uint64_t expected=(r->physical+w->offset)|3|(1u<<11)|(1u<<10)|((uint64_t)r->shareability<<8);
            if(!(r->permissions&ArmSmmu4KExecute))expected|=UINT64_C(3)<<53;
            if(!(r->permissions&ArmSmmu4KPrivileged))expected|=1u<<6;
            if(!(r->permissions&ArmSmmu4KWrite))expected|=1u<<7;
            if(r->writeback)expected|=1u<<2;
            w->r->expected_descriptor=expected;
            if(w->r->iova!=r->iova+w->offset || d!=expected)return ArmSmmu4KStateMismatch;
            ++w->r->leaf_pages;w->offset+=PAGE;
            if(w->offset==r->bytes){++w->range;w->offset=0;}
        }
    }
    /* Empty intermediate tables are not part of the declared resource graph. */
    return w->r->leaf_pages>initial_leaves?ArmSmmu4KSuccess:ArmSmmu4KStateMismatch;
}
ArmSmmu4KStatus ArmSmmu4KAuditTables(const ArmSmmu4KTableIo *io,
    const ArmSmmu4KConfig *c,const ArmSmmu4KResult *b,const ArmSmmu4KAuditRegion *a,
    uint64_t root,ArmSmmu4KTableAuditResult *r)
{
    if(!r)return ArmSmmu4KArgument;*r=(ArmSmmu4KTableAuditResult){0};
    if(!io || !io->read_descriptor)return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=validate(c,b,a,root);if(status)return status;
    for(size_t i=0;i<a->capacity_pages;++i)a->visited[i]=0;
    Walk w={io,c,b,a,r,0,0};
    uint64_t base=b->upper_region?UINT64_MAX-(UINT64_C(1)<<c->input_bits)+1:0;
    status=walk(&w,root,b->start_level,base);if(status)return status;
    if(r->table_pages!=b->table_pages || r->leaf_pages!=b->leaf_pages || w.range!=a->range_count)
        return ArmSmmu4KStateMismatch;
    r->complete=1;return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus pair_validate(const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *a)
{
    if(!c || !a || !a->io.read_descriptor || (!c->lower && !c->upper) ||
        !!c->lower!=!!c->lower_tables || !!c->upper!=!!c->upper_tables ||
        !!c->lower!=!!a->lower || !!c->upper!=!!a->upper)return ArmSmmu4KArgument;
    const ArmSmmu4KConfig *cs[2]={c->lower,c->upper};
    const ArmSmmu4KResult *bs[2]={c->lower_tables,c->upper_tables};
    const ArmSmmu4KAuditRegion *as[2]={a->lower,a->upper};
    for(unsigned i=0;i<2;++i)if(cs[i]) {
        ArmSmmu4KStatus status=validate(cs[i],bs[i],as[i],bs[i]->root_physical);if(status)return status;
        if(bs[i]->upper_region!=i)return ArmSmmu4KStateMismatch;
    }
    if(c->lower && c->upper) {
        if(c->lower->output_bits!=c->upper->output_bits)return ArmSmmu4KStateMismatch;
        if(intersects(c->lower->table_physical,a->lower->capacity_pages*PAGE,
                      c->upper->table_physical,a->upper->capacity_pages*PAGE))return ArmSmmu4KOverlap;
        for(unsigned side=0;side<2;++side)for(size_t i=0;i<as[side]->range_count;++i) {
            const ArmSmmu4KRange *r=&as[side]->ranges[i];
            if(intersects(r->physical,r->bytes,cs[!side]->table_physical,as[!side]->capacity_pages*PAGE))return ArmSmmu4KOverlap;
        }
        for(size_t i=0;i<a->lower->range_count;++i)for(size_t j=0;j<a->upper->range_count;++j) {
            const ArmSmmu4KRange *x=&a->lower->ranges[i],*y=&a->upper->ranges[j];
            if(incompatible(x,y) && intersects(x->physical,x->bytes,y->physical,y->bytes))return ArmSmmu4KOverlap;
        }
    }
    return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus regions(const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *a,
    uint64_t lower,uint64_t upper,ArmSmmu4KAuditResult *r)
{
    ArmSmmu4KStatus status;
    if(c->lower){r->phase=2;status=ArmSmmu4KAuditTables(&a->io,c->lower,c->lower_tables,a->lower,lower,&r->lower);if(status)return status;}
    if(c->upper){r->phase=3;status=ArmSmmu4KAuditTables(&a->io,c->upper,c->upper_tables,a->upper,upper,&r->upper);if(status)return status;}
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KAuditDeclaredContext(const ArmSmmu4KContextConfig *c,
    const ArmSmmu4KAuditConfig *a,ArmSmmu4KAuditResult *r)
{
    if(!r)return ArmSmmu4KArgument;*r=(ArmSmmu4KAuditResult){0};
    ArmSmmu4KStatus status=pair_validate(c,a);if(status)return status;
    status=regions(c,a,c->lower?c->lower_tables->root_physical:0,c->upper?c->upper_tables->root_physical:0,r);
    if(!status){r->phase=4;r->complete=1;}return status;
}
ArmSmmu4KStatus ArmSmmu4KObserveAuditedContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *a,ArmSmmu4KAuditResult *r)
{
    if(!r)return ArmSmmu4KArgument;*r=(ArmSmmu4KAuditResult){0};
    ArmSmmu4KStatus status=pair_validate(c,a);if(status)return status;
    r->phase=1;status=ArmSmmu4KObserveOwnedContext(io,bank,c,&r->before);if(status)return status;
    status=regions(c,a,r->before.readback.ttbr0&ADDRESS,r->before.readback.ttbr1&ADDRESS,r);if(status)return status;
    r->phase=5;status=ArmSmmu4KObserveOwnedContext(io,bank,c,&r->after);if(status)return status;
    /* Each complete image independently matched the same immutable expected
     * context, including capabilities, ASID, roots, MAIR and granules. */
    if(r->before.id0!=r->after.id0 || r->before.id1!=r->after.id1 || r->before.id2!=r->after.id2)
        return ArmSmmu4KStateMismatch;
    r->phase=6;r->complete=1;return ArmSmmu4KSuccess;
}
