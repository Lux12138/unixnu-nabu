/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KMmio.h"
#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Direct SMMUv2 MMIO binding requires little-endian AArch64
#endif
static int fits(const ArmSmmu4KMmio *m,size_t at,size_t bytes)
{return m && m->mapping && m->bytes && m->bytes-1<=UINTPTR_MAX-(uintptr_t)m->mapping && !((uintptr_t)m->mapping&7) && !(at&(bytes-1)) && at<=m->bytes && bytes<=m->bytes-at;}
static int read32(void *cookie,size_t at,uint32_t *out)
{ArmSmmu4KMmio *m=cookie;if(!out || !fits(m,at,4))return 1;*out=*(volatile uint32_t *)(m->mapping+at);return 0;}
static int read64(void *cookie,size_t at,uint64_t *out)
{ArmSmmu4KMmio *m=cookie;if(!out || !fits(m,at,8))return 1;*out=*(volatile uint64_t *)(m->mapping+at);return 0;}
static int write32(void *cookie,size_t at,uint32_t value)
{ArmSmmu4KMmio *m=cookie;if(!fits(m,at,4))return 1;*(volatile uint32_t *)(m->mapping+at)=value;return 0;}
static int write64(void *cookie,size_t at,uint64_t value)
{ArmSmmu4KMmio *m=cookie;if(!fits(m,at,8))return 1;*(volatile uint64_t *)(m->mapping+at)=value;return 0;}
static int barrier(void *cookie)
{(void)cookie;__asm__ volatile("dsb sy":::"memory");return 0;}
ArmSmmu4KStatus ArmSmmu4KBindMmio(ArmSmmu4KMmio *mapping,ArmSmmu4KIo *out)
{
    if(!out)return ArmSmmu4KArgument;
    *out=(ArmSmmu4KIo){0};
    if(!mapping || !mapping->mapping || ((uintptr_t)mapping->mapping&7) || mapping->bytes<0x2c || mapping->bytes-1>UINTPTR_MAX-(uintptr_t)mapping->mapping)return ArmSmmu4KArgument;
    out->cookie=mapping;out->register_bytes=mapping->bytes;out->read32=read32;out->read64=read64;
    out->write32=write32;out->write64=write64;out->barrier=barrier;
    return ArmSmmu4KSuccess;
}
