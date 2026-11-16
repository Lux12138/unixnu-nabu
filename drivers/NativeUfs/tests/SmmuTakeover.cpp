// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <array>
#include <vector>
#include <utility>
#include "../SmmuTakeover.h"
int main()
{
  std::array<uint32_t,0xd0000/4> r{};
  auto reset=[&] {r.fill(0); r[0x20/4]=0x40000080; r[0x24/4]=0x60000046; r[0x28/4]=0x5111;};
  auto plan=[&] {return planUfsSmmu(0x60,0x8cb5e9000ULL,0x22000,[&](unsigned at){return r.at(at/4);});};
  reset(); auto p=plan(); assert(!p.error && !p.reuse && p.slot==127 && p.bank==69);
  r[(0x800+4*110)/4]=0x80000060; r[(0xc00+4*110)/4]=69;
  r[(0x1000+4*69)/4]=0x10000;
  p=plan(); assert(!p.error && p.reuse && p.slot==110);
  r[(0x800+4*111)/4]=0x80000060; r[(0xc00+4*111)/4]=69; assert(plan().error);
  // Another stream still pointing at the top bank (a mapping left by an earlier run): the plan moves to the next free bank down.
  reset(); r[0x800/4]=0x80000061; r[0xc00/4]=69; p=plan(); assert(!p.error && !p.reuse && p.bank==68 && p.slot==127);
  // The next banks may be held too, or translating: each is skipped; with every attempt blocked the first refusal is reported.
  reset(); r[0x800/4]=0x80000061; r[0xc00/4]=69; r[0x804/4]=0x80000062; r[0xc04/4]=68; r[(128+67)*4096/4]=1; p=plan(); assert(!p.error && p.bank==66);
  reset(); for(unsigned k=0;k<8;++k){r[(0x800+4*k)/4]=0x80000100+k;r[(0xc00+4*k)/4]=69-k;}
  p=plan(); assert(p.error && !__builtin_strcmp(p.error,"reserved context bank already used") && p.bank==69);
  reset(); r[0x800/4]=0x80000060; r[0xc00/4]=1; assert(plan().error);
  // Our stream already mapped to the fallback bank by an earlier run (the top bank held by another stream): reused, not refused.
  reset(); r[0x800/4]=0x80000061; r[0xc00/4]=69; r[0x804/4]=0x80000060; r[0xc04/4]=68; r[(0x1000+4*68)/4]=0x10000;
  p=plan(); assert(!p.error && p.reuse && p.bank==68 && p.slot==1);
  reset(); r[0x800/4]=0x80010060; r[0xc00/4]=69; assert(plan().error);
  reset(); r[0x800/4]=0x80008060; r[0xc00/4]=1; assert(!plan().error); // Distinct 16-bit SID.
  reset(); r[(128+69)*4096/4]=1; p=plan(); assert(!p.error && p.bank==68); // translating top bank: use the next one
  reset(); for(unsigned k=0;k<8;++k) r[(128+69-k)*4096/4]=1; assert(plan().error);
  reset(); r[0x24/4]|=0x80000000; assert(plan().error); // Never guess 64K register geometry.
  reset(); for(unsigned i=0;i<128;++i) {r[(0x800+4*i)/4]=0x80000100+i;r[(0xc00+4*i)/4]=1;}
  assert(plan().error);
  // The reported DMA range fits every 36-bit IDR2 limit, but the previous
  // CBA2R=0 setup faults on its first byte (ARM IHI0062D.c table 3-1).
  reset(); p=plan(); assert(!p.error && p.pages==128);
  auto read=[&](unsigned at){return r.at(at/4);};
  std::vector<std::pair<unsigned,uint32_t>> writes;
  unsigned drop=0xffffffffU;
  auto write=[&](unsigned at,uint32_t value) {
    writes.emplace_back(at,value);
    if(at!=drop) r.at(at/4)=value;
    if(at==0x800+4*p.slot) {
      assert(read(0x1800+4*p.bank)==1);
      assert(read(0x1000+4*p.bank)==0x10000);
      assert(read((p.pages+p.bank)*4096)==0);
    }
  };
  auto configure=[&]{return configureUfsSmmu(p,0x60,read,write);};
  auto inputFault=[&](uint64_t address) {
    // Independent bypass-only architecture rule, not a hardware emulator.
    return !(read(0x1800+4*p.bank)&1) && (address>>32);
  };
  assert(inputFault(0x8cb5e9000ULL));
  assert(!inputFault(0xffffffffULL) && inputFault(0x100000000ULL));
  assert(!configure() && !inputFault(0x8cb5e9000ULL));
  assert(!inputFault(0x8cb60afffULL)); // Final byte of the same allocation.
  assert(writes.size()==5);
  p=plan(); writes.clear(); assert(p.reuse && !configure() && writes.empty());
  r[(0x1800+4*p.bank)/4]=0;
  assert(!configure() && writes.size()==1 && writes[0].first==0x1800+4*p.bank);
  // A refused context-format write must stop before stream publication.
  for(unsigned offset : {0x1800+4*69, 0x1000+4*69, 0xc00+4*127, 0x800+4*127}) {
    reset(); p=plan(); writes.clear(); drop=offset;
    assert(configure());
    if(offset!=0x800+4*127) assert(read(0x800+4*127)==0);
  }
  reset(); p=plan(); r[(128+69)*4096/4]=2; drop=(128+69)*4096;
  assert(configure()); // An ignored disable write cannot publish a stream.
  assert(read(0x800+4*p.slot)==0);
  // Full extent, overflow and each independent hardware address width.
  for(unsigned shift : {0U,4U,8U}) {
    reset(); r[0x28/4] &= ~(15U<<shift); assert(plan().error);
    r[0x28/4] |= 6U<<shift; assert(plan().error); // Reserved width.
  }
  reset();
  for(auto range : {std::pair<uint64_t,uint64_t>{0x8cb5e9000ULL,0},
       {0xfffffffffULL,2}, {0xfffffffffffff000ULL,0x22000}, {0x1000000000ULL,1}})
    assert(planUfsSmmu(0x60,range.first,range.second,read).error);
  assert(!planUfsSmmu(0x60,0xfffffffffULL,1,read).error);
  r[0x28/4]&=~(1U<<12); assert(plan().error);
  reset(); r[0x20/4]&=~(1U<<30); assert(plan().error);
  // Reused context with an unexpected VMID/attribute must not be repaired.
  reset(); p=plan(); drop=0xffffffffU; assert(!configure());
  r[(0x1800+4*p.bank)/4]=0x10001; p=plan(); assert(p.error);
  writes.clear(); assert(configure() && writes.empty());
  puts("PASS: SMMU geometry/conflicts, VA64 high-DMA regression, full-range width limits, ordered configuration, reuse/repair and blocked-write refusal");
}
