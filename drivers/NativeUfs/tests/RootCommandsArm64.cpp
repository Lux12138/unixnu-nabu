// SPDX-License-Identifier: BSD-2-Clause-Patent
// Freestanding EL1 execution of production UFS code. QEMU virt addresses only.
#include <stdint.h>
#include <stddef.h>
extern "C" int snprintf(char *, size_t, const char *, ...);
extern "C" void *memcpy(void *out, const void *in, size_t n) {
  auto d=static_cast<uint8_t *>(out); auto s=static_cast<const uint8_t *>(in);
  for (size_t i=0;i<n;++i) d[i]=s[i];
  return out;
}
#include "../RootDiagnostics.h"
#include "../TransferPoll.h"
#include "../RootTransportLayout.h"
using IOReturn=uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnError=0xe00002bc,
  kIOReturnBadArgument=0xe00002c2, kIOReturnIOError=0xe00002ca,
  kIOReturnBusy=0xe00002d5, kIOReturnTimeout=0xe00002d6, kIOReturnNotReady=0xe00002d8;
constexpr unsigned kMicrosecondScale=1, kMillisecondScale=1000;
struct Host { uint8_t *virt; uint64_t phys; bool ready,poisoned; uintptr_t hc; void *lock; };
static Host gHost;
static UfsRootDiagnostics gRootDiagnostic;
// Runner extracts these functions verbatim from NativeUfs.cpp, including
// actual DC CIVAC/DC IVAC/DSB and volatile read32/write32. No DMA callbacks.
#include "RootHardwareExtracted.h"
static uint64_t mach_absolute_time() {
  uint64_t ticks; asm volatile("mrs %0, cntpct_el0":"=r"(ticks)); return ticks;
}
static uint64_t frequency() {
  uint64_t value; asm volatile("mrs %0, cntfrq_el0":"=r"(value)); return value;
}
static void clock_interval_to_deadline(uint32_t interval,unsigned scale,uint64_t *end) {
  *end=mach_absolute_time()+uint64_t(interval)*scale*frequency()/1000000;
}
static void IODelay(uint32_t us) {
  uint64_t end; clock_interval_to_deadline(us,1,&end);
  while (mach_absolute_time()<end) asm volatile("yield");
}
// Single EL1 CPU, interrupts masked. This does not test XNU lock scheduling.
static void IOLockLock(void *) {}
static void IOLockUnlock(void *) {}
static IOReturn recordTransport(IOReturn result,uint32_t ocs) {
  gRootDiagnostic.result=result; gRootDiagnostic.ocs=ocs; return result;
}
#include "../RootCommands.h"
#include "../RootQueue.h"

static void puts(const char *s) {
  for (;*s;++s) *reinterpret_cast<volatile uint32_t *>(0x09000000)=*s;
}
static void number(const char *name,uint64_t value) {
  puts(name); puts("=0x");
  for (int shift=60;shift>=0;shift-=4) {
    char c[2]={"0123456789abcdef"[(value>>shift)&15],0}; puts(c);
  }
  puts("\n");
}
[[noreturn]] static void finish(unsigned status) {
  uint64_t args[2]={0x20026,status};
  register uint64_t x0 asm("x0")=0x20;
  register uint64_t x1 asm("x1")=reinterpret_cast<uintptr_t>(args);
  asm volatile("hlt #0xf000"::"r"(x0),"r"(x1):"memory");
  for (;;) asm volatile("wfe");
}
static void check(bool okay,const char *label) {
  if (okay) return;
  puts("FAIL "); puts(label); puts("\n");
  number("result",gRootDiagnostic.result); number("ocs",gRootDiagnostic.ocs);
  number("polls",gRootDiagnostic.polls); number("dbSeen",gRootDiagnostic.dbSeen);
  number("isSeen",gRootDiagnostic.isSeen); finish(1);
}
extern "C" [[noreturn]] void guest_exception() {
  puts("FAIL exception\n"); uint64_t v;
  asm volatile("mrs %0, esr_el1":"=r"(v)); number("ESR",v);
  asm volatile("mrs %0, far_el1":"=r"(v)); number("FAR",v);
  asm volatile("mrs %0, elr_el1":"=r"(v)); number("ELR",v);
  finish(1);
}
alignas(4096) static uint64_t l1[512],l2[512],l3[512];
alignas(4096) static uint8_t output[kDataMax];
extern "C" void enable_translation(uint64_t,uint64_t);
extern "C" void guest_main() {
  uint64_t el; asm volatile("mrs %0, CurrentEL":"=r"(el)); check(el==4,"EL1");
  // Device block for UART/PCI/UFS, cached identity map for fixture code,
  // stack and tables. DMA uses only 4 KiB leaves, with an unmapped guard.
  l1[0]=(uint64_t(3)<<53)|0x405;
  l1[1]=reinterpret_cast<uintptr_t>(l2)|3;
  constexpr uint64_t share=TEST_SHAREABILITY;
  static_assert(share==2 || share==3,"Test outer or inner sharing");
  constexpr uint64_t tcr=(0x50080351aULL & ~0x3000ULL)|(share<<12);
  for (unsigned i=0;i<64;++i) l2[i]=(0x40000000ULL+i*0x200000ULL)|0x401|(share<<8);
  l2[128]=reinterpret_cast<uintptr_t>(l3)|3;
  constexpr uint64_t physical=TEST_DMA_PHYS;
  for (unsigned i=0;i<kDmaBytes/4096;++i) l3[i]=(physical+i*4096)|0x403|(share<<8);
  enable_translation(reinterpret_cast<uintptr_t>(l1),tcr);
  uint64_t state;
  asm volatile("mrs %0, tcr_el1":"=r"(state)); number("TCR",state);
  check(state==tcr,"TCR 4K");
  asm volatile("mrs %0, sctlr_el1":"=r"(state)); number("SCTLR",state);
  check((state&0x1005)==0x1005,"MMU and caches enabled");
  asm volatile("mrs %0, ttbr0_el1":"=r"(state)); number("TTBR0",state);
  check(state==reinterpret_cast<uintptr_t>(l1),"active table");
  asm volatile("mrs %0, mair_el1":"=r"(state)); number("MAIR",state);
  asm volatile("mrs %0, ctr_el0":"=r"(state)); number("CTR",state);
  number("DMA_VA",0x50000000); number("DMA_PA",physical); number("DMA_PTE",l3[0]);
  // AT checks the active hardware translation, not merely table construction.
  asm volatile("at s1e1r, %0; isb"::"r"(uint64_t(0x50000000)):"memory");
  asm volatile("mrs %0, par_el1":"=r"(state)); number("DMA_PAR",state);
  check(!(state&1) && (state&0x0000fffffffff000ULL)==physical,"VA to PA");
  asm volatile("at s1e1r, %0; isb"::"r"(uint64_t(0x50000000+kDmaBytes)):"memory");
  asm volatile("mrs %0, par_el1":"=r"(state)); check(state&1,"4K guard page");

  gHost={reinterpret_cast<uint8_t *>(0x50000000),physical,true,false,0x10000000,nullptr};
  for (unsigned i=0;i<kDmaBytes;++i) gHost.virt[i]=0;
  cacheCivac(gHost.virt,kDmaBytes);
  constexpr uintptr_t pci=0x3f008000;
  check(read32(pci,0)==0x00131b36,"QEMU UFS PCI identity");
  write32(pci,0x10,gHost.hc); write32(pci,0x14,0);
  *reinterpret_cast<volatile uint16_t *>(pci+4)=6;
  asm volatile("dsb sy":::"memory");
  number("CAP",read32(gHost.hc,0)); check(read32(gHost.hc,0)&0x01000000,"64-bit DMA");
  hostWrite(0x34,1); hostWrite(0x90,0x16); // Fixture-only HCE/link startup.
  check(hostWaitRegister(0x30,15,15),"fixture link startup");
  const char *why=nullptr;
  check(hostStopQueue(&why),"queue stop");
#if TEST_BAD_DMA == 1
  gHost.phys=uint32_t(physical);
#elif TEST_BAD_DMA == 2
  gHost.phys=reinterpret_cast<uintptr_t>(gHost.virt);
#endif
  check(hostStartQueue(&why),"queue start");
  auto result=hostNop();
#if TEST_BAD_DMA
  check(result==kIOReturnTimeout && gHost.poisoned,"bad DMA not quarantined on timeout");
  number("result",result); number("ocs",gRootDiagnostic.ocs);
  number("polls",gRootDiagnostic.polls); number("dbSeen",gRootDiagnostic.dbSeen);
  number("isSeen",gRootDiagnostic.isSeen);
  check(hostNop()==kIOReturnNotReady && gRootDiagnostic.command==1,"quarantine resubmitted");
  puts("PASS rejected bad DMA without resubmission\n"); finish(0);
#endif
  check(result==0,"NOP"); puts("PASS NOP\n");
  check(*reinterpret_cast<uint32_t *>(gHost.virt+20)==uint32_t((physical+kUcdOffset)>>32),"UTRD upper PA");
  uint32_t power=0; check(hostQueryRead(3,2,&power)==0,"power query"); number("power",power);
  uint8_t tur[6]={}; uint32_t scsi;
  for (unsigned i=0;i<3;++i) {
    result=hostScsi(0,tur,6,false,0,200000,&scsi);
    if (!result) break;
    check(result==kIOReturnNotReady && (scsi==2 || scsi==0x102),"TUR check condition");
  }
  check(result==0,"TUR ready");
  const unsigned blockCounts[]={1,32};
  for (unsigned blocks: blockCounts) {
    check(hostRead4k(0,8,blocks,output)==0,"READ10");
    for (unsigned i=0;i<blocks*4096;++i) check(output[i]==uint8_t((i*17+(i>>8)*31)^0xa5),"read payload");
    check(*reinterpret_cast<uint32_t *>(gHost.virt+kUcdOffset+kPrdtOffset+4)==uint32_t((physical+kDataOffset)>>32),"PRDT upper PA");
  }
  puts("PASS READ4K READ128K\n");
  for (unsigned i=0;i<4096;++i) output[i]=0xa5;
  check(hostRead4k(0,0x10000,1,output)!=0,"bad LBA accepted");
  for (unsigned i=0;i<4096;++i) check(output[i]==0xa5,"bad LBA copied stale data");
  puts("PASS bad-LBA-no-copy\nPASS ALL\n"); finish(0);
}
