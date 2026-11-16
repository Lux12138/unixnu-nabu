/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KLive.h"
#define PAGE UINT64_C(4096)
#define PA_MASK UINT64_C(0x0000fffffffff000)

uint64_t (*arm_smmu_4k_live_now_us)(void);
volatile uint64_t arm_smmu_4k_live_phase_us[2][8];

static void phase_lap(uint64_t (*now_us)(void),uint64_t *last,uint64_t *elapsed)
{
    if(!now_us) {
        return;
    }

    const uint64_t now=now_us();
    *elapsed=now-*last;
    *last=now;
}

static int output_width(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
static ArmSmmu4KStatus layout(const ArmSmmu4KLiveArena *a,ArmSmmu4KResult *out)
{
    if(!a || !a->mapping || ((uintptr_t)a->mapping&4095) || a->upper_region>1 ||
        a->config.input_bits<32 || a->config.input_bits>48 ||
        !output_width(a->config.output_bits) || !a->config.leaf_budget ||
        !a->capacity_pages || a->capacity_pages>SIZE_MAX/4096 ||
        a->capacity_pages*4096>UINTPTR_MAX-(uintptr_t)a->mapping)return ArmSmmu4KArgument;
    uint64_t limit=UINT64_C(1)<<a->config.input_bits;
    uint64_t physical_limit=UINT64_C(1)<<a->config.output_bits;
    if(!a->bytes || ((a->iova|a->bytes|a->config.table_physical)&4095) ||
        (a->upper_region ? a->iova<UINT64_MAX-limit+1 : a->iova>=limit) ||
        a->bytes>limit-(a->iova&(limit-1)) ||
        a->config.table_physical>=physical_limit ||
        a->capacity_pages*PAGE>physical_limit-a->config.table_physical)return ArmSmmu4KBadRange;
    if(a->bytes/PAGE>a->config.leaf_budget)return ArmSmmu4KBudget;
    unsigned start=4-(a->config.input_bits-12+8)/9;
    uint64_t low=a->iova&(limit-1),high=low+a->bytes-1;
    size_t pages=1;
    for(unsigned level=start;level<3;++level) {
        unsigned shift=39-9*level;
        uint64_t extra=(high>>shift)-(low>>shift)+1;
        if(extra>SIZE_MAX-pages)return ArmSmmu4KCapacity;
        pages+=(size_t)extra;
    }
    if(pages>a->capacity_pages)return ArmSmmu4KCapacity;
    *out=(ArmSmmu4KResult){a->config.table_physical,0xff44,pages,0,start,a->upper_region};
    return ArmSmmu4KSuccess;
}
static uint64_t descriptor(const ArmSmmu4KRange *r,uint64_t iova)
{
    uint64_t attrs=(1u<<11)|(1u<<10)|3u|((uint64_t)r->shareability<<8);
    if(!(r->permissions&ArmSmmu4KExecute))attrs|=UINT64_C(3)<<53;
    if(!(r->permissions&ArmSmmu4KPrivileged))attrs|=1u<<6;
    if(!(r->permissions&ArmSmmu4KWrite))attrs|=1u<<7;
    if(r->writeback)attrs|=1u<<2;
    return (r->physical+(iova-r->iova))|attrs;
}
static const ArmSmmu4KRange *find(const ArmSmmu4KRange *ranges,size_t count,uint64_t iova)
{
    size_t low=0,high=count;
    while(low<high) {
        size_t mid=low+(high-low)/2;
        const ArmSmmu4KRange *r=&ranges[mid];
        if(iova<r->iova)high=mid;
        else if(iova-r->iova>=r->bytes)low=mid+1;
        else return r;
    }
    return 0;
}
/* The walk visits addresses in ascending order, so one cursor per range list replaces a binary search for every leaf: lower_bound once per
 * leaf page, then forward. A range is behind the address when the address lies past its end (written as a difference: native addresses
 * may end at 2^64). */
static size_t first_not_behind(const ArmSmmu4KRange *ranges,size_t count,uint64_t va)
{
    size_t low=0,high=count;
    while(low<high) {
        size_t mid=low+(high-low)/2;
        if(va>=ranges[mid].iova && va-ranges[mid].iova>=ranges[mid].bytes)low=mid+1;
        else high=mid;
    }
    return low;
}
static const ArmSmmu4KRange *scan(const ArmSmmu4KRange *ranges,size_t count,size_t *cursor,uint64_t va)
{
    while(*cursor<count && va>=ranges[*cursor].iova && va-ranges[*cursor].iova>=ranges[*cursor].bytes)++*cursor;
    if(*cursor<count && va>=ranges[*cursor].iova && va-ranges[*cursor].iova<ranges[*cursor].bytes)return &ranges[*cursor];
    return 0;
}
static uint64_t expected(const ArmSmmu4KRange *ranges,size_t count,uint64_t iova)
{
    const ArmSmmu4KRange *r=find(ranges,count,iova);
    return r?descriptor(r,iova):0;
}
static ArmSmmu4KStatus validate_range(const ArmSmmu4KLiveArena *a,const ArmSmmu4KRange *r)
{
    uint64_t physical_limit=UINT64_C(1)<<a->config.output_bits;
    uint64_t pool=a->config.table_physical,pool_bytes=a->capacity_pages*PAGE;
    if(!r || !r->bytes || ((r->iova|r->physical|r->bytes)&4095) ||
        !(r->permissions&ArmSmmu4KRead) || (r->permissions&~15u) ||
        r->writeback>1 || (r->shareability!=2 && r->shareability!=3) ||
        r->physical>=physical_limit || r->bytes>physical_limit-r->physical ||
        r->iova<a->iova || r->iova-a->iova>=a->bytes ||
        r->bytes>a->bytes-(r->iova-a->iova))return ArmSmmu4KBadRange;
    if(r->physical<pool+pool_bytes && pool<r->physical+r->bytes)return ArmSmmu4KOverlap;
    return ArmSmmu4KSuccess;
}
static int conflicting_alias(const ArmSmmu4KRange *a,const ArmSmmu4KRange *b)
{
    return (a->writeback!=b->writeback || a->shareability!=b->shareability ||
        ((a->permissions^b->permissions)&ArmSmmu4KPrivileged)) &&
        a->physical<b->physical+b->bytes && b->physical<a->physical+a->bytes;
}
/* Only ranges of different cache/shareability/privilege classes can alias in conflict, and of two conflicting ranges at most one
 * is in the class most ranges of both sets have. So for every other class k, the members of class k in one set are compared with
 * the members of other classes in the other set, both ways. Each such comparison sorts the smaller side by physical address in
 * the arena's scratch and looks every range of the larger side up in it: a scattered buffer is one range per physical page, and
 * with coherent imports (write-back) beside the GPU's own non-cacheable buffers a pairwise loop over both was some 2.4 billion
 * comparisons per map (int-144: 368 ms per map on average, 8 s at worst). Without scratch, or a side larger than it, the
 * comparison is the pairwise loop. */
static unsigned alias_class(const ArmSmmu4KRange *r)
{return (r->writeback&1u)|((r->shareability&3u)<<1)|((r->permissions&ArmSmmu4KPrivileged)?8u:0u);}
typedef struct {
    const ArmSmmu4KRange *ranges;size_t count;
    unsigned cls;int same;   /* members: class == cls (same) or class != cls */
} AliasSide;
static int alias_member(const AliasSide *s,size_t i)
{return (alias_class(&s->ranges[i])==s->cls)==(s->same!=0);}
static void sift(const ArmSmmu4KRange *r,uint32_t *h,size_t root,size_t n)
{
    for(;;) {
        size_t child=2*root+1;
        if(child>=n)return;
        if(child+1<n && r[h[child+1]].physical>r[h[child]].physical)++child;
        if(r[h[root]].physical>=r[h[child]].physical)return;
        uint32_t t=h[root];h[root]=h[child];h[child]=t;root=child;
    }
}
static int alias_pass(const ArmSmmu4KLiveArena *arena,const AliasSide *p,const AliasSide *q)
{
    size_t np=0,nq=0;
    for(size_t i=0;i<p->count;++i)np+=(size_t)alias_member(p,i);
    for(size_t j=0;j<q->count;++j)nq+=(size_t)alias_member(q,j);
    if(!np || !nq)return 0;
    const AliasSide *sorted=np<=nq?p:q,*probe=np<=nq?q:p;
    const size_t n=np<=nq?np:nq;
    uint32_t *h=arena?arena->alias_scratch:0;
    if(!h || n>arena->alias_scratch_entries || sorted->count>UINT32_MAX) {
        for(size_t i=0;i<p->count;++i)if(alias_member(p,i))
            for(size_t j=0;j<q->count;++j)if(alias_member(q,j) && conflicting_alias(&p->ranges[i],&q->ranges[j]))return 1;
        return 0;
    }
    const ArmSmmu4KRange *r=sorted->ranges;
    size_t k=0;
    for(size_t i=0;i<sorted->count;++i)if(alias_member(sorted,i)) {
        h[k++]=(uint32_t)i;
    }
    for(size_t i=n/2;i-->0;)sift(r,h,i,n);
    for(size_t end=n;end>1;) {--end;uint32_t t=h[0];h[0]=h[end];h[end]=t;sift(r,h,0,end);}
    /* Drop intervals contained by an earlier interval. Both starts and ends of
     * the retained indices are now monotonic, even with nested/duplicate runs.
     * A distant long run cannot force gap queries to scan short runs backward.
     * Every compared pair belongs to different classes by AliasSide's filter. */
    size_t kept=0;
    for(size_t i=0;i<n;++i) {
        uint32_t index=h[i];
        if(!kept || r[index].physical+r[index].bytes>r[h[kept-1]].physical+r[h[kept-1]].bytes)h[kept++]=index;
    }
    for(size_t j=0;j<probe->count;++j) {
        if(!alias_member(probe,j))continue;
        const ArmSmmu4KRange *y=&probe->ranges[j];
        size_t lo=0,hi=kept;   /* first interval ending after y's start */
        while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(r[h[mid]].physical+r[h[mid]].bytes<=y->physical)lo=mid+1;else hi=mid;}
        if(lo<kept && r[h[lo]].physical<y->physical+y->bytes)return 1;
    }
    return 0;
}
static int any_conflicting_alias(const ArmSmmu4KLiveArena *arena,const ArmSmmu4KRange *a,size_t na,
    const ArmSmmu4KRange *b,size_t nb)
{
    if(!na || !nb)return 0;
    size_t members[16]={0};
    for(size_t i=0;i<na;++i)++members[alias_class(&a[i])];
    for(size_t j=0;j<nb;++j)++members[alias_class(&b[j])];
    unsigned reference=0;
    for(unsigned k=1;k<16;++k)if(members[k]>members[reference])reference=k;
    const int self=a==b && na==nb;
    for(unsigned k=0;k<16;++k) {
        if(k==reference || !members[k])continue;
        const AliasSide ak={a,na,k,1},bo={b,nb,k,0};
        if(alias_pass(arena,&ak,&bo))return 1;
        if(self)continue;
        const AliasSide bk={b,nb,k,1},ao={a,na,k,0};
        if(alias_pass(arena,&bk,&ao))return 1;
    }
    return 0;
}
static ArmSmmu4KStatus validate_ranges_checked(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KRange *ranges,size_t count,size_t *leaves,int aliases);
static ArmSmmu4KStatus validate_ranges(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KRange *ranges,size_t count,size_t *leaves)
{return validate_ranges_checked(a,ranges,count,leaves,1);}
static ArmSmmu4KStatus validate_ranges_checked(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KRange *ranges,size_t count,size_t *leaves,int aliases)
{
    *leaves=0;
    if((count && !ranges) || count>a->config.leaf_budget ||
        count>SIZE_MAX/sizeof(*ranges))return ArmSmmu4KArgument;
    for(size_t i=0;i<count;++i) {
        ArmSmmu4KStatus status=validate_range(a,&ranges[i]);
        if(status!=ArmSmmu4KSuccess)return status;
        if(i && ranges[i].iova<=ranges[i-1].iova+(ranges[i-1].bytes-1))return ArmSmmu4KOverlap;
        size_t pages=(size_t)(ranges[i].bytes/PAGE);
        if(pages>a->config.leaf_budget-*leaves)return ArmSmmu4KBudget;
        *leaves+=pages;
    }
    return aliases && any_conflicting_alias(a,ranges,count,ranges,count)?ArmSmmu4KOverlap:ArmSmmu4KSuccess;
}
/* Traverse the actual fixed topology, never generate/copy a reference table.
 * A fixed owned aperture reserves empty intermediate tables; every other slot
 * must be zero. Resource expectations are metadata, not translated page tables. */
static ArmSmmu4KStatus walk(const ArmSmmu4KLiveArena *a,const ArmSmmu4KResult *shape,
    unsigned level,size_t page,uint64_t prefix,size_t *next,unsigned build,
    const ArmSmmu4KRange *ranges,size_t count,const ArmSmmu4KRange *changes,size_t changed,
    unsigned map,ArmSmmu4KLiveResult *r)
{
    uint64_t limit=UINT64_C(1)<<a->config.input_bits;
    uint64_t low=a->iova&(limit-1),high=low+a->bytes-1;
    unsigned shift=39-9*level;
    unsigned width=level==shape->start_level?a->config.input_bits-shift:9;
    size_t range_cursor=0,change_cursor=0;
    if(level==3) {
        uint64_t first=a->upper_region ? prefix|~(limit-1) : prefix;
        range_cursor=first_not_behind(ranges,count,first);change_cursor=first_not_behind(changes,changed,first);
    }
    for(unsigned slot=0;slot<512;++slot) {
        uint64_t begin=prefix+((uint64_t)slot<<shift),value=0;
        int reserved=slot<(1u<<width) && begin<=high && low<begin+(UINT64_C(1)<<shift);
        size_t child=0;
        if(level<3 && reserved) {
            child=(*next)++;
            if(child>=shape->table_pages)return ArmSmmu4KStateMismatch;
            value=(a->config.table_physical+child*PAGE)|3u;
        } else if(level==3 && reserved && !build) {
            uint64_t va=a->upper_region ? begin|~(limit-1) : begin;
            const ArmSmmu4KRange *c=scan(changes,changed,&change_cursor,va);
            if(c)value=map?descriptor(c,va):0;
            else {
                const ArmSmmu4KRange *r0=scan(ranges,count,&range_cursor,va);
                value=r0?descriptor(r0,va):0;
            }
        }
        size_t index=page*512+slot;
        if(build)a->mapping[index]=value;
        else {++r->descriptor_reads;if(a->mapping[index]!=value)return ArmSmmu4KStateMismatch;}
        if(level<3 && reserved) {
            ArmSmmu4KStatus status=walk(a,shape,level+1,child,begin,next,build,ranges,count,changes,changed,map,r);
            if(status!=ArmSmmu4KSuccess)return status;
        }
    }
    return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus leaf_index(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KResult *shape,uint64_t va,size_t *index,ArmSmmu4KLiveResult *r);
/* The changed leaves alone, through the table path each one's address takes: before the store (after=0) each holds what the
 * operation replaces (nothing for a map, its range's descriptor for an unmap), after it what the operation wrote. */
static ArmSmmu4KStatus verify_changed(const ArmSmmu4KLiveArena *a,const ArmSmmu4KResult *shape,
    const ArmSmmu4KRange *changes,size_t changed,unsigned map,unsigned after,ArmSmmu4KLiveResult *r)
{
    for(size_t i=0;i<changed;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        const uint64_t va=changes[i].iova+offset;size_t index;
        ArmSmmu4KStatus status=leaf_index(a,shape,va,&index,r);if(status!=ArmSmmu4KSuccess)return status;
        const uint64_t want=(map==after)?descriptor(&changes[i],va):0;
        ++r->descriptor_reads;
        if(a->mapping[index]!=want)return ArmSmmu4KStateMismatch;
    }
    return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus verify(const ArmSmmu4KLiveArena *a,const ArmSmmu4KResult *shape,
    const ArmSmmu4KRange *ranges,size_t count,const ArmSmmu4KRange *changes,size_t changed,
    unsigned map,ArmSmmu4KLiveResult *r)
{
    size_t next=1;
    ArmSmmu4KStatus status=walk(a,shape,shape->start_level,0,0,&next,0,ranges,count,changes,changed,map,r);
    return status==ArmSmmu4KSuccess && next!=shape->table_pages?ArmSmmu4KStateMismatch:status;
}
ArmSmmu4KStatus ArmSmmu4KLivePrepare(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state)
{
    if(!state || state->prepared || state->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(a,&shape);
    if(status!=ArmSmmu4KSuccess)return status;
    size_t next=1;
    status=walk(a,&shape,shape.start_level,0,0,&next,1,0,0,0,0,0,0);
    if(status!=ArmSmmu4KSuccess || next!=shape.table_pages) {state->poisoned=1;return ArmSmmu4KStateMismatch;}
    state->tables=shape;state->generation=0;state->prepared=1;state->context_offset=0;
    return ArmSmmu4KSuccess;
}
static int same_config(const ArmSmmu4KConfig *a,const ArmSmmu4KConfig *b)
{
    return a && b && a->input_bits==b->input_bits && a->output_bits==b->output_bits &&
        a->table_physical==b->table_physical && a->leaf_budget==b->leaf_budget;
}
ArmSmmu4KStatus ArmSmmu4KLiveAudit(const ArmSmmu4KLiveArena *a,const ArmSmmu4KLiveState *state,
    const ArmSmmu4KRange *ranges,size_t count,ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    if(!state || !state->prepared || state->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(a,&shape);
    if(status!=ArmSmmu4KSuccess)return status;
    if(state->tables.root_physical!=shape.root_physical || state->tables.mair!=shape.mair ||
        state->tables.table_pages!=shape.table_pages || state->tables.start_level!=shape.start_level ||
        state->tables.upper_region!=shape.upper_region)return ArmSmmu4KArgument;
    size_t leaves=0;
    status=validate_ranges(a,ranges,count,&leaves);if(status!=ArmSmmu4KSuccess)return status;
    if(leaves!=state->tables.leaf_pages)return ArmSmmu4KArgument;
    status=verify(a,&shape,ranges,count,0,0,0,r);
    r->complete=status==ArmSmmu4KSuccess;return status;
}
static ArmSmmu4KStatus leaf_index(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KResult *shape,uint64_t va,size_t *index,ArmSmmu4KLiveResult *r)
{
    size_t page=0;
    for(unsigned level=shape->start_level;level<3;++level) {
        unsigned shift=39-9*level,width=level==shape->start_level?a->config.input_bits-shift:9;
        uint64_t entry=a->mapping[page*512+((va>>shift)&((1u<<width)-1))];++r->descriptor_reads;
        uint64_t physical=entry&PA_MASK;
        if(entry!=(physical|3u) || physical<a->config.table_physical ||
            physical-a->config.table_physical>=shape->table_pages*PAGE)return ArmSmmu4KStateMismatch;
        page=(size_t)((physical-a->config.table_physical)/PAGE);
    }
    *index=page*512+((va>>12)&511);
    return ArmSmmu4KSuccess;
}
static int overlaps(uint64_t a,uint64_t abytes,uint64_t b,uint64_t bbytes)
{return a<b+bbytes && b<a+abytes;}
static int touches_pool(const ArmSmmu4KRange *r,const ArmSmmu4KLiveArena *a)
{return overlaps(r->physical,r->bytes,a->config.table_physical,a->capacity_pages*PAGE);}
static ArmSmmu4KStatus region_binding(const ArmSmmu4KLiveRegion *region,
    const ArmSmmu4KConfig *config,const ArmSmmu4KResult *root,unsigned upper)
{
    if(!region)return (!config && !root)?ArmSmmu4KSuccess:ArmSmmu4KArgument;
    if(!region->arena || !region->state || region->arena->upper_region!=upper ||
        root!=&region->state->tables || !same_config(config,&region->arena->config) ||
        !region->state->prepared || region->state->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(region->arena,&shape);if(status)return status;
    const ArmSmmu4KResult *r=&region->state->tables;
    if(r->root_physical!=shape.root_physical || r->mair!=shape.mair || r->table_pages!=shape.table_pages ||
        r->start_level!=shape.start_level || r->upper_region!=shape.upper_region)return ArmSmmu4KArgument;

    return ArmSmmu4KSuccess;
}

static ArmSmmu4KStatus region_metadata(const ArmSmmu4KLiveRegion *region,
    const ArmSmmu4KConfig *config,const ArmSmmu4KResult *root,unsigned upper,int aliases)
{
    ArmSmmu4KStatus status=region_binding(region,config,root,upper);
    if(status || !region) {
        return status;
    }

    size_t leaves=0;
    status=validate_ranges_checked(region->arena,region->ranges,region->count,&leaves,aliases);if(status)return status;
    return leaves==region->state->tables.leaf_pages?ArmSmmu4KSuccess:ArmSmmu4KArgument;
}

/* The context binding identifies the enabled table arenas and their retained
 * leaf counts. The caller owns the existing resource lists between updates. */
static ArmSmmu4KStatus context_binding(const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper)
{
    if(!context || (!lower && !upper)) {
        return ArmSmmu4KArgument;
    }

    ArmSmmu4KStatus status=region_binding(lower,context->lower,context->lower_tables,0);
    if(status) {
        return status;
    }

    status=region_binding(upper,context->upper,context->upper_tables,1);
    if(status) {
        return status;
    }

    const ArmSmmu4KLiveRegion *regions[]={lower,upper};
    for(unsigned i=0;i<2;++i) {
        const ArmSmmu4KLiveRegion *region=regions[i];
        if(region && region->state->tables.leaf_pages>region->arena->config.leaf_budget) {
            return ArmSmmu4KBudget;
        }
    }

    if(lower && upper) {
        const ArmSmmu4KLiveArena *a=lower->arena,*b=upper->arena;
        if(lower->state==upper->state || a->config.output_bits!=b->config.output_bits ||
            overlaps(a->config.table_physical,a->capacity_pages*PAGE,b->config.table_physical,b->capacity_pages*PAGE) ||
            overlaps((uintptr_t)a->mapping,a->capacity_pages*PAGE,(uintptr_t)b->mapping,b->capacity_pages*PAGE)) {
            return ArmSmmu4KOverlap;
        }
    }

    return ArmSmmu4KSuccess;
}
/* aliases=0 (an update that is not its region's whole-verification one) skips the existing ranges' alias checks against each other
 * and across the regions: each range was checked against all of them when it was mapped, and the 64th update re-proves them. */
static ArmSmmu4KStatus context_metadata(const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper,int aliases)
{
    if(!context || (!lower && !upper))return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=region_metadata(lower,context->lower,context->lower_tables,0,aliases);if(status)return status;
    status=region_metadata(upper,context->upper,context->upper_tables,1,aliases);if(status)return status;
    if(lower && upper) {
        const ArmSmmu4KLiveArena *a=lower->arena,*b=upper->arena;
        if(lower->state==upper->state || a->config.output_bits!=b->config.output_bits ||
            overlaps(a->config.table_physical,a->capacity_pages*PAGE,b->config.table_physical,b->capacity_pages*PAGE) ||
            overlaps((uintptr_t)a->mapping,a->capacity_pages*PAGE,(uintptr_t)b->mapping,b->capacity_pages*PAGE))
            return ArmSmmu4KOverlap;
        for(size_t i=0;i<lower->count;++i)if(touches_pool(&lower->ranges[i],b))return ArmSmmu4KOverlap;
        for(size_t i=0;i<upper->count;++i)if(touches_pool(&upper->ranges[i],a))return ArmSmmu4KOverlap;
        if(aliases && any_conflicting_alias(lower->arena,lower->ranges,lower->count,upper->ranges,upper->count))return ArmSmmu4KOverlap;
    }
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KLiveAuditContext(const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper,ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    ArmSmmu4KStatus status=context_metadata(context,lower,upper,1);if(status)return status;
    const ArmSmmu4KLiveRegion *regions[]={lower,upper};
    for(unsigned i=0;i<2;++i)if(regions[i]) {
        const ArmSmmu4KLiveRegion *v=regions[i];
        status=verify(v->arena,&v->state->tables,v->ranges,v->count,0,0,0,r);if(status)return status;
    }
    r->complete=1;return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus update(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,
    const ArmSmmu4KIo *io,unsigned bank,const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveVisibility *visibility,const ArmSmmu4KRange *before,size_t count,
    const ArmSmmu4KLiveRegion *peer,const ArmSmmu4KRange *changes,size_t change_count,unsigned map,unsigned budget,
    unsigned caller_validated,
    ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    if(!state || !state->prepared || state->poisoned || !context ||
        !visibility || !visibility->publish_table_page || map>1 || !change_count ||
        !budget || budget>1048576 || state->generation==UINT64_MAX)return ArmSmmu4KArgument;

    uint64_t (*now_us)(void)=caller_validated?arm_smmu_4k_live_now_us:0;
    const uint64_t began=now_us?now_us():0;
    uint64_t phase_start=began,phase_us[5]={0};

    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(a,&shape);
    if(status!=ArmSmmu4KSuccess)return status;
    const ArmSmmu4KResult *root=a->upper_region?context->upper_tables:context->lower_tables;
    if((!peer && (a->upper_region?(context->lower || context->lower_tables):(context->upper || context->upper_tables))) ||
        !same_config(a->upper_region?context->upper:context->lower,&a->config) ||
        root!=&state->tables || state->tables.root_physical!=shape.root_physical ||
        state->tables.mair!=shape.mair || state->tables.table_pages!=shape.table_pages ||
        state->tables.start_level!=shape.start_level || state->tables.upper_region!=shape.upper_region)
        return ArmSmmu4KArgument;
    /* The complete-range entry points audit the whole region every 64 updates.
     * UpdateContextRanges receives changes already checked against the caller's
     * objects and validates the actual changed descriptors on every update. */
    const int whole=!caller_validated && (state->generation&63)==0;
    r->whole_verification=whole?1u:0u;
    size_t leaves=state->tables.leaf_pages;
    if(caller_validated) {
        if(leaves>a->config.leaf_budget) {
            return ArmSmmu4KBudget;
        }
    } else {
        status=validate_ranges_checked(a,before,count,&leaves,whole);
        if(status!=ArmSmmu4KSuccess) {
            return status;
        }
        if(leaves!=state->tables.leaf_pages) {
            return ArmSmmu4KArgument;
        }
    }
    /* The changes: sorted, disjoint, each valid; together within the leaf budget. */
    size_t changed=0;
    if(!changes)return ArmSmmu4KBadRange;
    if(change_count>a->config.leaf_budget)return ArmSmmu4KArgument;
    for(size_t i=0;i<change_count;++i) {
        status=validate_range(a,&changes[i]);if(status!=ArmSmmu4KSuccess)return status;
        if(i && changes[i].iova<=changes[i-1].iova+(changes[i-1].bytes-1))return ArmSmmu4KOverlap;
        size_t pages=(size_t)(changes[i].bytes/PAGE);
        if(pages>a->config.leaf_budget-changed)return ArmSmmu4KBudget;
        changed+=pages;
    }
    if(peer) {
        ArmSmmu4KLiveRegion target={a,state,before,count};
        status=caller_validated?
            context_binding(context,a->upper_region?peer:&target,a->upper_region?&target:peer):
            context_metadata(context,a->upper_region?peer:&target,a->upper_region?&target:peer,whole);
        if(status)return status;
        for(size_t i=0;i<change_count;++i)if(touches_pool(&changes[i],peer->arena))return ArmSmmu4KOverlap;
        if(map && !caller_validated && any_conflicting_alias(a,changes,change_count,peer->ranges,peer->count))return ArmSmmu4KOverlap;
    }
    if(map && changed>a->config.leaf_budget-leaves)return ArmSmmu4KBudget;
    if(caller_validated && !map && changed>leaves) {
        return ArmSmmu4KStateMismatch;
    }

    if(map && (any_conflicting_alias(a,changes,change_count,changes,change_count) ||
        (!caller_validated && any_conflicting_alias(a,changes,change_count,before,count))))return ArmSmmu4KOverlap;
    if(!caller_validated) {
        for(size_t i=0;i<change_count;++i) {
            for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
                uint64_t va=changes[i].iova+offset,old=expected(before,count,va);
                if(map?old!=0:old!=descriptor(&changes[i],va)) {
                    return ArmSmmu4KStateMismatch;
                }
            }
        }
    }
    /* The first update observes the owned context and records its register
     * offset. Complete-range audits repeat this observation. The caller retains
     * the context bank and observes its streams between those operations. */
    const int observed=whole || !state->context_offset;
    if(observed) {
        status=ArmSmmu4KObserveOwnedContext(io,bank,context,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    phase_lap(now_us,&phase_start,&phase_us[0]);

    if(whole) {
        status=verify(a,&shape,before,count,0,0,0,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(peer) {
            status=verify(peer->arena,&peer->state->tables,peer->ranges,peer->count,0,0,0,r);
            if(status)goto poisoned;
        }
    } else {
        status=verify_changed(a,&shape,changes,change_count,map,0,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    if(observed) {
        ArmSmmu4KProgramResult initial=r->synchronization;
        status=ArmSmmu4KObserveOwnedContext(io,bank,context,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(initial.id0!=r->synchronization.id0 || initial.id1!=r->synchronization.id1 ||
            initial.id2!=r->synchronization.id2 || initial.context_offset!=r->synchronization.context_offset)
            {status=ArmSmmu4KStateMismatch;goto poisoned;}
    }
    phase_lap(now_us,&phase_start,&phase_us[1]);

    /* All validation precedes the first store. Caller keeps every master
     * quiesced and retains both old/new object references through this call. */
    for(size_t i=0;i<change_count;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        uint64_t va=changes[i].iova+offset;size_t index;
        status=leaf_index(a,&shape,va,&index,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        a->mapping[index]=map?descriptor(&changes[i],va):0;++r->descriptor_writes;
    }
    /* Sorted changes reach leaf pages in order, so each changed page is published once. */
    size_t last_page=SIZE_MAX;
    for(size_t i=0;i<change_count;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        size_t index;
        status=leaf_index(a,&shape,changes[i].iova+offset,&index,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        size_t page=index/512;
        if(page!=last_page) {
            if(visibility->publish_table_page(visibility->cookie,a->mapping+page*512,4096)) {status=ArmSmmu4KIoError;goto poisoned;}
            ++r->published_pages;last_page=page;
        }
    }
    phase_lap(now_us,&phase_start,&phase_us[2]);

    /* A map of leaves that were invalid needs no TLB invalidation: the TLBs hold no translation of an invalid descriptor (as Linux's
     * io-pgtable maps without one). The unmap invalidates the ASID before it reports the translation removed. */
    if(observed) {
        status=ArmSmmu4KSynchronizeOwnedContext(io,bank,context,budget,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
    } else if(!map) {
        status=ArmSmmu4KSyncContextTlb(io,state->context_offset,context->asid,0,budget,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    phase_lap(now_us,&phase_start,&phase_us[3]);

    if(whole) {
        status=verify(a,&shape,before,count,changes,change_count,map,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(peer) {
            status=verify(peer->arena,&peer->state->tables,peer->ranges,peer->count,0,0,0,r);
            if(status)goto poisoned;
        }
    } else {
        status=verify_changed(a,&shape,changes,change_count,map,1,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    if(observed) {
        ArmSmmu4KProgramResult after;
        status=ArmSmmu4KObserveOwnedContext(io,bank,context,&after);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(after.id0!=r->synchronization.id0 || after.id1!=r->synchronization.id1 ||
            after.id2!=r->synchronization.id2 || after.context_offset!=r->synchronization.context_offset)
            {status=ArmSmmu4KStateMismatch;goto poisoned;}
        state->context_offset=r->synchronization.context_offset;
    }
    phase_lap(now_us,&phase_start,&phase_us[4]);

    state->tables.leaf_pages=map?leaves+changed:leaves-changed;++state->generation;
    r->complete=1;r->translation_removed=!map;r->must_retain_resources=0;
    if(now_us) {
        volatile uint64_t *totals=arm_smmu_4k_live_phase_us[map?0:1];
        totals[0]=totals[0]+1;
        for(unsigned i=0;i<5;++i) {
            totals[i+1]=totals[i+1]+phase_us[i];
        }
        const uint64_t elapsed=phase_start-began;
        if(elapsed>totals[6]) {
            totals[6]=elapsed;
        }
        totals[7]=totals[7]+changed;
    }

    return ArmSmmu4KSuccess;
poisoned:
    state->poisoned=1;if(peer)peer->state->poisoned=1;return status;
}
ArmSmmu4KStatus ArmSmmu4KLiveUpdate(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,
    const ArmSmmu4KIo *io,unsigned bank,const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveVisibility *visibility,const ArmSmmu4KRange *before,size_t count,
    const ArmSmmu4KRange *change,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{
    return update(a,state,io,bank,context,visibility,before,count,0,change,1,map,budget,0,r);
}
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContextBatch(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *changes,size_t change_count,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    if(change_upper>1)return ArmSmmu4KArgument;
    const ArmSmmu4KLiveRegion *target=change_upper?upper:lower,*peer=change_upper?lower:upper;
    if(!target || !target->state)return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=context_metadata(context,lower,upper,(target->state->generation&63)==0);if(status)return status;
    return update(target->arena,target->state,io,bank,context,visibility,target->ranges,target->count,
        peer,changes,change_count,map,budget,0,r);
}

ArmSmmu4KStatus ArmSmmu4KLiveUpdateContextRanges(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *changes,size_t change_count,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{
    if(!r) {
        return ArmSmmu4KArgument;
    }
    *r=(ArmSmmu4KLiveResult){0};
    r->must_retain_resources=1;

    if(change_upper>1) {
        return ArmSmmu4KArgument;
    }
    const ArmSmmu4KLiveRegion *target=change_upper?upper:lower,*peer=change_upper?lower:upper;
    ArmSmmu4KStatus status=context_binding(context,lower,upper);
    if(status) {
        return status;
    }
    if(!target) {
        return ArmSmmu4KArgument;
    }

    return update(target->arena,target->state,io,bank,context,visibility,0,0,
        peer,changes,change_count,map,budget,1,r);
}
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *change,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{return ArmSmmu4KLiveUpdateContextBatch(io,bank,context,lower,upper,change_upper,visibility,change,1,map,budget,r);}
