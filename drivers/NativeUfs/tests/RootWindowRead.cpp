// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnIOError=2, kIOReturnNotPrivileged=3;
constexpr uint64_t kDataMax=131072;
#include "../RootWindowRead.h"
struct Shared {
  std::mutex window, control;
  std::condition_variable changed;
  unsigned attempts=0;
  bool concurrent=false;
  uint8_t bounce[kDataMax];
};
struct Ops {
  Shared &shared;
  std::vector<uint8_t> disk, output;
  bool held=false, parked=false;
  unsigned reads=0, copies=0, checks=0, badRead=0, badVerify=0;
  uint64_t shortCopy=UINT64_MAX;
  Ops(Shared &s, unsigned blocks=70) : shared(s), disk(blocks*4096), output(disk.size(),0xa5) {
    for (size_t i=0;i<disk.size();++i) disk[i]=uint8_t(i*17+i/4096);
  }
  void lock() {
    { std::lock_guard<std::mutex> g(shared.control); ++shared.attempts; shared.changed.notify_all(); }
    shared.window.lock(); held=true;
  }
  void unlock() { assert(held); held=false; shared.window.unlock(); }
  IOReturn read(uint64_t lba,uint32_t blocks,void *out) {
    assert(held && blocks<=32); ++reads;
    if(reads==badRead) return kIOReturnIOError;
    assert(lba*4096+blocks*4096<=disk.size());
    memcpy(out,disk.data()+lba*4096,blocks*4096); return 0;
  }
  IOReturn verify(uint64_t index,const void *data,size_t bytes) {
    assert(held); ++checks;
    if(shared.concurrent && !parked) {
      parked=true;
      std::unique_lock<std::mutex> guard(shared.control);
      shared.changed.wait(guard,[&]{return shared.attempts>=2;});
    }
    if(checks==badVerify) return kIOReturnNotPrivileged;
    assert(index*4096+bytes<=disk.size());
    assert(!memcmp(data,disk.data()+index*4096,bytes)); return 0;
  }
  uint64_t copy(uint64_t offset,const void *data,uint64_t bytes) {
    assert(held); ++copies;
    uint64_t n=bytes<shortCopy?bytes:shortCopy;
    assert(offset+n<=output.size()); memcpy(output.data()+offset,data,n); return n;
  }
};
int main() {
  Shared s;
  assert(!ufsWindowGeometry(UINT64_MAX-4095,4096));
  assert(!ufsWindowGeometry(0,UINT64_MAX-511));
  assert(ufsWindowGeometry(uint64_t(UINT32_MAX)*4096,4096));
  assert(!ufsWindowGeometry(uint64_t(UINT32_MAX)*4096,4608));
  // Every 512-byte offset within a 4K block, spanning two 128K commands.
  for(unsigned offset=0;offset<8;++offset) {
    Ops o(s); auto r=ufsReadWindow(o,0,o.disk.size(),offset,257,s.bounce);
    assert(r.status==0 && r.done==257*512 && o.reads==2 && !o.held);
    assert(!memcmp(o.output.data(),o.disk.data()+offset*512,r.done));
  }
  { Ops o(s); auto r=ufsReadWindow(o,0,4096+512,8,1,s.bounce);
    assert(r.status==0 && r.done==512 && o.checks==1); }
  { Ops o(s); o.badVerify=32; auto r=ufsReadWindow(o,0,o.disk.size(),0,256,s.bounce);
    assert(r.status==3 && !r.done && !o.copies && o.output[0]==0xa5 && !o.held); }
  { Ops o(s); o.badVerify=33; auto r=ufsReadWindow(o,0,o.disk.size(),0,257,s.bounce);
    assert(r.status==3 && r.done==131072 && o.copies==1 && o.output[r.done]==0xa5); }
  { Ops o(s); o.badRead=2; auto r=ufsReadWindow(o,0,o.disk.size(),0,257,s.bounce);
    assert(r.status==2 && r.done==131072 && !o.held); }
  { Ops o(s); o.shortCopy=17; auto r=ufsReadWindow(o,0,o.disk.size(),0,8,s.bounce);
    assert(r.status==2 && r.done==17 && !o.held && o.output[17]==0xa5); }
  { Ops o(s); for(auto count:{0ULL,UINT64_MAX}) {
      auto r=ufsReadWindow(o,0,o.disk.size(),0,count,s.bounce); assert(r.status==1 && !o.reads); }
    auto r=ufsReadWindow(o,0,o.disk.size(),o.disk.size()/512,1,s.bounce); assert(r.status==1 && !o.reads); }
  // First reader pauses inside verification until a second request has tried
  // to acquire the SAME window lock. Both must retain their own read snapshot.
  s.attempts=0; s.concurrent=true;
  Ops first(s), second(s); UfsWindowResult a{},b{};
  std::thread one([&]{ a=ufsReadWindow(first,0,first.disk.size(),0,8,s.bounce); });
  { std::unique_lock<std::mutex> guard(s.control); s.changed.wait(guard,[&]{return s.attempts==1;}); }
  std::thread two([&]{ b=ufsReadWindow(second,0,second.disk.size(),8,8,s.bounce); });
  one.join(); two.join();
  assert(a.status==0 && b.status==0);
  assert(!memcmp(first.output.data(),first.disk.data(),4096));
  assert(!memcmp(second.output.data(),second.disk.data()+4096,4096));
  // The wrapper can invoke a reentrant completion now: guard has released.
  assert(s.window.try_lock()); s.window.unlock();
  puts("PASS: window geometry, 512/4K/128K boundaries, tail, no unverified copy, partial progress, concurrent snapshot lock, completion outside lock");
}
