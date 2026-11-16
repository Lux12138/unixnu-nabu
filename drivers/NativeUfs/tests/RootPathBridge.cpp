// SPDX-License-Identifier: BSD-2-Clause-Patent
// Same production layout/window/command code; OpenSSL replaces kernel crypto,
// qtest replaces hardware primitives. No fake verifier or signature bypass.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#include <mutex>
#include <vector>
#include "verified_root.h"
#include "RootCommandsBridge.cpp"
constexpr IOReturn kIOReturnNotPrivileged=0xe00002c1;
#include "../RootLayout.h"
#include "../RootWindowRead.h"
extern "C" int vr_host_open(const uint8_t *,size_t,const uint8_t *,uint64_t,vr_view *);
extern "C" int vr_host_block(const vr_view *,uint64_t,const void *,size_t);
static RootLayout layout;
static std::vector<uint8_t> envelope;
static vr_view verified;
static std::mutex windowLock;
static uint8_t bounce[kDataMax];
static unsigned selected;
extern "C" {
uint32_t ufs_path_layout() {
  const char *why=nullptr;
  return findRootLayout(&layout,&why) ? layout.manifestStatus : 0x100;
}
uint64_t ufs_path_window(unsigned index,unsigned field) {
  if(index>=layout.windows) return 0;
  return field==0 ? layout.window[index].offset : layout.window[index].size;
}
uint32_t ufs_path_open(unsigned index,const uint8_t *manifest,size_t bytes,const uint8_t *key) {
  verified={}; envelope.clear();
  if(layout.manifestStatus || index>=layout.windows) return 0x100;
  envelope.assign(manifest,manifest+bytes);
  int result=vr_host_open(envelope.data(),envelope.size(),key,1,&verified);
  if(result || verified.image_bytes!=layout.window[index].size) { verified={}; return result?result:0x101; }
  selected=index; return 0;
}
uint32_t ufs_path_read(uint64_t block,uint64_t count,uint8_t *out,uint64_t capacity,uint64_t *done) {
  *done=0;
  if(!verified.hashes || count>capacity/512) return kIOReturnBadArgument;
  struct Operations {
    uint8_t *out;
    void lock() { windowLock.lock(); }
    void unlock() { windowLock.unlock(); }
    IOReturn read(uint64_t lba,uint32_t blocks,void *data) { return hostRead4k(0,lba,blocks,data); }
    IOReturn verify(uint64_t index,const void *data,size_t length) {
      return vr_host_block(&verified,index,data,length)==VR_OK?kIOReturnSuccess:kIOReturnNotPrivileged;
    }
    uint64_t copy(uint64_t offset,const void *data,uint64_t length) { memcpy(out+offset,data,length); return length; }
  } ops{out};
  auto result=ufsReadWindow(ops,layout.userdataFirst*4096+layout.window[selected].offset,
                             layout.window[selected].size,block,count,bounce);
  *done=result.done; return result.status;
}
}
