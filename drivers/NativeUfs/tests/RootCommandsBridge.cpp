// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile the SAME command implementation as the kernel. Test-only callbacks
// replace physical I/O and synchronization, not descriptor or response logic.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include "../RootDiagnostics.h"
#include "../TransferPoll.h"
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnError=0xe00002bc,
  kIOReturnBadArgument=0xe00002c2, kIOReturnIOError=0xe00002ca,
  kIOReturnBusy=0xe00002d5, kIOReturnTimeout=0xe00002d6, kIOReturnNotReady=0xe00002d8;
#include "../RootTransportLayout.h"
constexpr unsigned kMicrosecondScale=1;
constexpr unsigned kMillisecondScale=1000;
static UfsRootDiagnostics gRootDiagnostic;
struct Host { uint8_t *virt; uint64_t phys; bool ready, poisoned; uintptr_t hc; void *lock; };
static Host gHost;
alignas(4096) static uint8_t memory[kDmaBytes];
using Callback = uint64_t (*)(unsigned operation, uint64_t address, uint64_t value, void *bytes);
static Callback callback;
// 0: MMIO read; 1: ordered MMIO write; 2: release DMA; 3: acquire DMA;
// 4: monotonic microseconds; 5: delay microseconds. Callbacks must not throw.
static uint32_t read32(uintptr_t, unsigned offset) { return uint32_t(callback(0,offset,0,nullptr)); }
static void hostWrite(unsigned offset, uint32_t value) { callback(1,offset,value,nullptr); }
static void cacheCivac(const void *start, size_t length) {
  callback(2,gHost.phys+uint64_t(static_cast<const uint8_t *>(start)-memory),length,const_cast<void *>(start));
}
static void cacheInvalidate(const void *start, size_t length) {
  callback(3,gHost.phys+uint64_t(static_cast<const uint8_t *>(start)-memory),length,const_cast<void *>(start));
}
static void clock_interval_to_deadline(uint32_t interval, unsigned scale, uint64_t *deadline) {
  *deadline=callback(4,0,0,nullptr)+uint64_t(interval)*scale;
}
static uint64_t mach_absolute_time() { return callback(4,0,0,nullptr); }
static void IODelay(uint32_t us) { callback(5,0,us,nullptr); }
static void IOLockLock(void *) {}
static void IOLockUnlock(void *) {}
static IOReturn recordTransport(IOReturn result,uint32_t ocs) {
  gRootDiagnostic.result=result; gRootDiagnostic.ocs=ocs; return result;
}
#include "../RootCommands.h"
#include "../RootQueue.h"
extern "C" {
void ufs_test_reset(Callback cb, uint64_t physical) {
  callback=cb; memset(memory,0,sizeof(memory));
  gHost={memory,physical,true,false,0,nullptr}; gRootDiagnostic=UfsRootDiagnostics{};
  gStoreOpen=gLogOpen=false; gStoreFirst=gStoreBlocks=gLogFirst=gLogBlocks=0;
}
uint8_t *ufs_test_memory() { return memory; }
bool ufs_test_poisoned() { return gHost.poisoned; }
bool ufs_test_stop_queue() { const char *why=nullptr; return hostStopQueue(&why); }
bool ufs_test_start_queue() { const char *why=nullptr; return hostStartQueue(&why); }
uint32_t ufs_test_query(uint8_t op,uint8_t idn,uint32_t *value) { return hostQueryRead(op,idn,value); }
uint32_t ufs_test_scsi(uint8_t lun,const uint8_t *cdb,unsigned length,bool read,uint32_t bytes,uint32_t *scsi) {
  return hostScsi(lun,cdb,length,read,bytes,200000,scsi);
}
uint32_t ufs_test_read(uint8_t lun,uint64_t lba,uint32_t blocks,void *out) { return hostRead4k(lun,lba,blocks,out); }
uint32_t ufs_test_store_open(uint64_t first,uint64_t blocks) { return hostStoreOpen(first,blocks) ? 1 : 0; }
uint32_t ufs_test_log_open(uint64_t first,uint64_t blocks) { return hostLogOpen(first,blocks) ? 1 : 0; }
uint32_t ufs_test_write(uint8_t lun,uint64_t lba,uint32_t blocks,const void *src) { return hostWrite4k(lun,lba,blocks,src); }
uint32_t ufs_test_sync() { return hostSyncCache(); }
uint32_t ufs_test_nop() {
  return hostNop();
}
int ufs_test_report(char *out,size_t size) { return formatUfsRootDiagnostics(out,size,gRootDiagnostic,"host harness"); }
}
