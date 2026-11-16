/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KTableMemory.h"
static int read_descriptor(void *cookie,uint64_t physical,uint64_t *value)
{
    const ArmSmmu4KTableMemory *memory=cookie;
    if(!value || (physical&7))return 1;
    for(unsigned i=0;i<memory->count;++i) {
        const ArmSmmu4KTableMapping *p=&memory->pools[i];
        if(physical>=p->physical && physical-p->physical<(uint64_t)p->pages*4096) {
            *value=p->mapping[(physical-p->physical)/8];return 0;
        }
    }
    return 1;
}
ArmSmmu4KStatus ArmSmmu4KBindTableMemory(const ArmSmmu4KTableMemory *memory,ArmSmmu4KTableIo *io)
{
    if(!io)return ArmSmmu4KArgument;*io=(ArmSmmu4KTableIo){0};
    if(!memory || !memory->count || memory->count>2)return ArmSmmu4KArgument;
    for(unsigned i=0;i<memory->count;++i) {
        const ArmSmmu4KTableMapping *p=&memory->pools[i];uintptr_t cpu=(uintptr_t)p->mapping;
        if(!cpu || (cpu&4095) || (p->physical&4095) || !p->pages || p->pages>SIZE_MAX/4096 ||
            p->physical>=(UINT64_C(1)<<48) || p->pages*UINT64_C(4096)>(UINT64_C(1)<<48)-p->physical ||
            p->pages*4096>UINTPTR_MAX-cpu)return ArmSmmu4KBadRange;
        for(unsigned j=0;j<i;++j) {
            const ArmSmmu4KTableMapping *q=&memory->pools[j];
            if((p->physical<q->physical+q->pages*UINT64_C(4096) && q->physical<p->physical+p->pages*UINT64_C(4096)) ||
                (cpu<(uintptr_t)q->mapping+q->pages*4096 && (uintptr_t)q->mapping<cpu+p->pages*4096))return ArmSmmu4KOverlap;
        }
    }
    io->cookie=(void *)memory;io->read_descriptor=read_descriptor;return ArmSmmu4KSuccess;
}
